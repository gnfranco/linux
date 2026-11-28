// SPDX-License-Identifier: GPL-2.0-only
/* Experimental reservation of the two ESP32-S31 Wi-Fi group 1 IRQs. */

#include <linux/bits.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/limits.h>
#include <linux/irq.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

/* Read-only fields from ESP32-S31 HP_SYS_CLKRST, not modem MMIO. */
#define S31_HP_CLKRST_BASE	0x20587000
#define S31_HP_MODEM_CTRL0	0x040
#define S31_HP_MODEM_CONF	0x1e0
#define S31_MODEM_BUS_GATE	BIT(0)
#define S31_MODEM_CONF_FIELDS	GENMASK(5, 0)

static void __iomem *esp32s31_wifi_map_snapshot(struct platform_device *pdev,
					       const char *name,
					       resource_size_t start,
					       resource_size_t size)
{
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res || res->start != start || resource_size(res) != size)
		return ERR_PTR(-EINVAL);

	return devm_ioremap_resource(&pdev->dev, res);
}

#define S31_PMU_ACTIVE_POWER	0x000
#define S31_PMU_MODEM_PWR	0x128
#define S31_PMU_MODEM_TOP	0x134
#define S31_PMU_MAIN_STATE	0x1c0
#define S31_PMU_PWR_STATE	0x1c4
#define S31_PMU_CLK_STATE0	0x1c8
#define S31_PMU_CLK_STATE3	0x1d4
#define S31_PMU_MODEM_PD	(BIT(27) | BIT(30))
#define S31_PMU_FORCE_MASK	GENMASK(5, 0)
#define S31_PMU_FORCE_ON	(BIT(2) | BIT(3) | BIT(4))

/*
 * Bench-only observation. Force bits describe programmed policy, not rail
 * telemetry. Require the exact forced-on policy before attempting reads in
 * the modem windows; never switch power, reset or clock state here.
 */
static int esp32s31_wifi_pmu_snapshot(struct platform_device *pdev,
				     unsigned int ctrl, unsigned int conf)
{
	struct device *dev = &pdev->dev;
	void __iomem *pmu, *syscon, *lpcon;
	u32 active, pwr, top, main, state, clk0, clk3, sd, ld;
	u32 sc0, sc1, sicg, lc, licg;

	if (!of_property_present(dev->of_node, "esp,modem-pmu-snapshot"))
		return 0;

	pmu = esp32s31_wifi_map_snapshot(pdev, "pmu", 0x20704000, 0x400);
	if (IS_ERR(pmu))
		return PTR_ERR(pmu);

	active = readl(pmu + S31_PMU_ACTIVE_POWER);
	pwr = readl(pmu + S31_PMU_MODEM_PWR);
	top = readl(pmu + S31_PMU_MODEM_TOP);
	main = readl(pmu + S31_PMU_MAIN_STATE);
	state = readl(pmu + S31_PMU_PWR_STATE);
	clk0 = readl(pmu + S31_PMU_CLK_STATE0);
	clk3 = readl(pmu + S31_PMU_CLK_STATE3);

	dev_info(dev, "modem-pmu active=%08x pwr=%08x top=%08x main=%08x state=%08x clk0=%08x clk3=%08x\n",
		 active, pwr, top, main, state, clk0, clk3);
	if (!main || main == U32_MAX || !(ctrl & S31_MODEM_BUS_GATE) ||
	    (conf & S31_MODEM_CONF_FIELDS) != 0x25 ||
	    (active & S31_PMU_MODEM_PD) ||
	    (pwr & S31_PMU_FORCE_MASK) != S31_PMU_FORCE_ON ||
	    (top & S31_PMU_FORCE_MASK) != S31_PMU_FORCE_ON) {
		dev_info(dev, "modem-window-snapshot skipped: policy unqualified\n");
		return 0;
	}

	/* Do not map the modem apertures unless the above policy passed. */
	syscon = esp32s31_wifi_map_snapshot(pdev, "modem-syscon",
					     0x20109c00, 0x34);
	if (IS_ERR(syscon))
		return PTR_ERR(syscon);
	lpcon = esp32s31_wifi_map_snapshot(pdev, "modem-lpcon",
					    0x2010f000, 0x5c);
	if (IS_ERR(lpcon))
		return PTR_ERR(lpcon);

	sd = readl(syscon + 0x30);
	ld = readl(lpcon + 0x58);
	dev_info(dev, "modem-window-date sys=%08x lp=%08x\n", sd, ld);
	if (!sd || !ld || sd == U32_MAX || ld == U32_MAX)
		return -EIO;

	sc0 = readl(syscon + 0x04);
	sc1 = readl(syscon + 0x14);
	sicg = readl(syscon + 0x0c);
	lc = readl(lpcon + 0x18);
	licg = readl(lpcon + 0x20);
	if (sc0 != readl(syscon + 0x04) || sc1 != readl(syscon + 0x14) ||
	    sicg != readl(syscon + 0x0c) || lc != readl(lpcon + 0x18) ||
	    licg != readl(lpcon + 0x20) ||
	    active != readl(pmu + S31_PMU_ACTIVE_POWER) ||
	    pwr != readl(pmu + S31_PMU_MODEM_PWR) ||
	    top != readl(pmu + S31_PMU_MODEM_TOP))
		return -EAGAIN;

	dev_info(dev, "modem-window-snapshot clk0=%08x clk1=%08x sys_icg=%08x lp_clk=%08x lp_icg=%08x\n",
		 sc0, sc1, sicg, lc, licg);
	return 0;
}

/* Optional experimental diagnostic; do not touch clocks or reset here. */
static int esp32s31_wifi_clock_snapshot(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np;
	struct resource res;
	struct regmap *map;
	unsigned int ctrl, conf;
	int ret;

	if (!of_property_present(dev->of_node, "esp,modem-hp-clkrst"))
		return 0;

	np = of_parse_phandle(dev->of_node, "esp,modem-hp-clkrst", 0);
	if (!np)
		return -EINVAL;
	if (!of_device_is_compatible(np, "esp,esp32s31-clkrst") ||
	    of_address_to_resource(np, 0, &res) ||
	    res.start != S31_HP_CLKRST_BASE || resource_size(&res) < 0x1e4) {
		of_node_put(np);
		return -EINVAL;
	}
	map = syscon_node_to_regmap(np);
	of_node_put(np);
	if (IS_ERR(map))
		return PTR_ERR(map);

	ret = regmap_read(map, S31_HP_MODEM_CTRL0, &ctrl);
	if (ret)
		return ret;
	ret = regmap_read(map, S31_HP_MODEM_CONF, &conf);
	if (ret)
		return ret;

	dev_info(dev, "modem-clock-snapshot bus=%u conf=0x%02x apb=%u reset=%u source_gate=%u source_sel=%u pll=%u xtal=%u\n",
		 !!(ctrl & S31_MODEM_BUS_GATE), (unsigned int)(conf & S31_MODEM_CONF_FIELDS),
		 !!(conf & BIT(0)), !!(conf & BIT(1)), !!(conf & BIT(2)),
		 !!(conf & BIT(3)), !!(conf & BIT(4)), !!(conf & BIT(5)));
	return esp32s31_wifi_pmu_snapshot(pdev, ctrl, conf);
}

static const struct {
	const char *name;
	u32 source;
} wifi_irqs[] = {
	{ "mac", 120 },
	{ "pwr", 122 },
};

static irqreturn_t esp32s31_wifi_irq(int irq, void *data)
{
	/* Peripheral acknowledgment and deassertion are unverified. */
	return IRQ_NONE;
}

static int esp32s31_wifi_irq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	irq_hw_number_t slots[ARRAY_SIZE(wifi_irqs)];
	int irqs[ARRAY_SIZE(wifi_irqs)];
	struct of_phandle_args spec;
	int i, index, ret;
	bool valid;

	if (!np || of_irq_count(np) != ARRAY_SIZE(wifi_irqs) ||
	    of_property_count_strings(np, "interrupt-names") != ARRAY_SIZE(wifi_irqs))
		return dev_err_probe(dev, -EINVAL, "expected MAC and PWR interrupts\n");

	/* Check both specifiers before a name lookup can allocate a route. */
	for (i = 0; i < ARRAY_SIZE(wifi_irqs); i++) {
		index = of_property_match_string(np, "interrupt-names", wifi_irqs[i].name);
		if (index < 0 || of_irq_parse_one(np, index, &spec))
			return dev_err_probe(dev, -EINVAL, "invalid %s interrupt\n",
					     wifi_irqs[i].name);
		valid = of_device_is_compatible(spec.np, "esp,esp32s31-intmtx") &&
			spec.args_count == 2 && spec.args[0] == wifi_irqs[i].source &&
			spec.args[1] == IRQ_TYPE_LEVEL_HIGH;
		of_node_put(spec.np);
		if (!valid)
			return dev_err_probe(dev, -EINVAL, "invalid %s interrupt route\n",
					     wifi_irqs[i].name);
	}

	for (i = 0; i < ARRAY_SIZE(wifi_irqs); i++) {
		irqs[i] = platform_get_irq_byname(pdev, wifi_irqs[i].name);
		if (irqs[i] < 0)
			return irqs[i];
		if (irq_get_trigger_type(irqs[i]) != IRQ_TYPE_LEVEL_HIGH)
			return dev_err_probe(dev, -EINVAL, "%s IRQ must be level-high\n",
					     wifi_irqs[i].name);
	}
	if (irqs[0] == irqs[1])
		return dev_err_probe(dev, -EINVAL, "MAC and PWR share a Linux IRQ\n");

	/*
	 * Never start either line. Physical masking before route allocation
	 * is a separate irqchip prerequisite; leave the DT node disabled
	 * until that is qualified. Devres frees and synchronizes installed
	 * actions on failure or unbind, not the OF IRQ-domain mappings.
	 */
	for (i = 0; i < ARRAY_SIZE(wifi_irqs); i++) {
		ret = devm_request_irq(dev, irqs[i], esp32s31_wifi_irq,
				       IRQF_NO_AUTOEN | IRQF_TRIGGER_HIGH,
				       dev_name(dev), pdev);
		if (ret)
			return dev_err_probe(dev, ret, "cannot reserve %s IRQ\n",
					     wifi_irqs[i].name);
	}

	for (i = 0; i < ARRAY_SIZE(wifi_irqs); i++) {
		struct irq_data *d = irq_get_irq_data(irqs[i]);
		bool masked = false;

		if (!d || d->hwirq != wifi_irqs[i].source || !d->parent_data)
			return dev_err_probe(dev, -EINVAL, "invalid %s IRQ mapping\n",
					     wifi_irqs[i].name);
		slots[i] = d->parent_data->hwirq;
		ret = irq_get_irqchip_state(irqs[i], IRQCHIP_STATE_MASKED, &masked);
		if (ret || !masked)
			return dev_err_probe(dev, ret ?: -EIO,
					     "%s source=%u virq=%d slot=%lu physical_masked=%s\n",
					     wifi_irqs[i].name, wifi_irqs[i].source,
					     irqs[i], slots[i], ret ? "unknown" : "0");
	}
	if (slots[0] == slots[1])
		return dev_err_probe(dev, -EINVAL, "MAC and PWR share slot %lu\n", slots[0]);

	for (i = 0; i < ARRAY_SIZE(wifi_irqs); i++)
		dev_info(dev, "%s source=%u virq=%d slot=%lu physical_masked=1\n",
			 wifi_irqs[i].name, wifi_irqs[i].source, irqs[i], slots[i]);

	ret = esp32s31_wifi_clock_snapshot(pdev);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read modem clock snapshot\n");

	return 0;
}

static const struct of_device_id esp32s31_wifi_irq_match[] = {
	{ .compatible = "esp,esp32s31-wifi-irq" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_wifi_irq_match);

static struct platform_driver esp32s31_wifi_irq_driver = {
	.probe = esp32s31_wifi_irq_probe,
	.driver = {
		.name = "esp32s31-wifi-irq",
		.of_match_table = esp32s31_wifi_irq_match,
	},
};
module_platform_driver(esp32s31_wifi_irq_driver);

MODULE_DESCRIPTION("ESP32-S31 masked Wi-Fi IRQ reservation");
MODULE_LICENSE("GPL");
