// SPDX-License-Identifier: GPL-2.0-only
/* Experimental reservation of the two ESP32-S31 Wi-Fi group 1 IRQs. */

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>

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
