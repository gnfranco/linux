// SPDX-License-Identifier: GPL-2.0
/* Experimental, bounded S31 DMA path counters. Not a DMA API. */
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/dma-map-ops.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/limits.h>
#include <linux/of.h>
#include <linux/percpu.h>
#include <linux/s31-dma-diag.h>
#include <linux/seq_file.h>

#define S31_DIAG_BINS	6
#define S31_DIAG_DIRS	3

enum s31_dma_diag_device {
	S31_DIAG_USB,
	S31_DIAG_EMAC,
	S31_DIAG_OTHER,
	S31_DIAG_DEVICES,
};

struct s31_dma_diag_slot {
	u32 count[S31_DIAG_DIRS][S31_DIAG_BINS][S31_DMA_EVENT_COUNT];
};

struct s31_dma_diag_cpu {
	struct s31_dma_diag_slot slot[S31_DIAG_DEVICES];
	u32 lost;
};

static DEFINE_PER_CPU(struct s31_dma_diag_cpu, s31_dma_diag);

static const char * const device_name[] = { "usb", "emac", "other" };
static const char * const bin_name[] = {
	"0-16", "17-32", "33-64", "65-96", "97-127", "128+",
};
static const char * const dir_name[] = { "bidi", "to", "from" };
static const char * const event_name[] = {
	"direct", "force_ok", "force_fail", "addr_ok", "addr_fail",
	"align_ok", "align_fail", "dwc2_temp_ok", "dwc2_temp_fail",
};

static unsigned int s31_dma_diag_bin(size_t size)
{
	if (size <= 16)
		return 0;
	if (size <= 32)
		return 1;
	if (size <= 64)
		return 2;
	if (size <= 96)
		return 3;
	if (size <= 127)
		return 4;
	return 5;
}

static void s31_dma_diag_inc(u32 *count)
{
	u32 value = READ_ONCE(*count);

	if (value != U32_MAX)
		WRITE_ONCE(*count, value + 1);
}

/*
 * Use fixed labels for the single USB and EMAC nodes in the S31 DT.
 * Do not read mutable kobject names in DMA/interrupt context.
 */
static enum s31_dma_diag_device s31_dma_diag_device(struct device *dev)
{
	if (of_device_is_compatible(dev->of_node, "esp,esp32s31-hsotg"))
		return S31_DIAG_USB;
	if (of_device_is_compatible(dev->of_node, "esp,esp32s31-dwmac"))
		return S31_DIAG_EMAC;
	return S31_DIAG_OTHER;
}

void s31_dma_diag_record(struct device *dev, enum dma_data_direction dir,
			 size_t size, enum s31_dma_diag_event event)
{
	struct s31_dma_diag_cpu *cpu;
	struct s31_dma_diag_slot *slot;
	enum s31_dma_diag_device id;
	unsigned long flags;

	if (dev_is_dma_coherent(dev))
		return;

	id = s31_dma_diag_device(dev);
	cpu = get_cpu_ptr(&s31_dma_diag);
	local_irq_save(flags);
	if ((unsigned int)dir >= S31_DIAG_DIRS ||
	    (unsigned int)event >= S31_DMA_EVENT_COUNT) {
		s31_dma_diag_inc(&cpu->lost);
	} else {
		slot = &cpu->slot[id];
		s31_dma_diag_inc(&slot->count[dir][s31_dma_diag_bin(size)][event]);
	}
	local_irq_restore(flags);
	put_cpu_ptr(&s31_dma_diag);
}
EXPORT_SYMBOL_GPL(s31_dma_diag_record);

static void s31_dma_diag_show_slot(struct seq_file *s,
				   const struct s31_dma_diag_slot *slot,
				   unsigned int cpu, unsigned int device)
{
	unsigned int dir, bin, event;
	u32 count;

	for (dir = 0; dir < S31_DIAG_DIRS; dir++)
		for (bin = 0; bin < S31_DIAG_BINS; bin++)
			for (event = 0; event < S31_DMA_EVENT_COUNT; event++) {
				count = READ_ONCE(slot->count[dir][bin][event]);
				if (count)
					seq_printf(s, "%u %s %s %s %s %u\n",
						   cpu, device_name[device], dir_name[dir],
						   bin_name[bin], event_name[event],
						   count);
			}
}

static int s31_dma_diag_show(struct seq_file *s, void *unused)
{
	const struct s31_dma_diag_cpu *cpu;
	unsigned int idx, i;

	seq_puts(s, "# cpu device direction bytes event count\n");
	for_each_possible_cpu(idx) {
		cpu = per_cpu_ptr(&s31_dma_diag, idx);
		seq_printf(s, "# lost %u %u\n", idx, READ_ONCE(cpu->lost));
		for (i = 0; i < S31_DIAG_DEVICES; i++)
			s31_dma_diag_show_slot(s, &cpu->slot[i], idx, i);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(s31_dma_diag);

static int __init s31_dma_diag_init(void)
{
	struct dentry *dir;

	dir = debugfs_create_dir("s31_dma_diag", NULL);
	if (!IS_ERR(dir))
		debugfs_create_file("stats", 0400, dir, NULL, &s31_dma_diag_fops);
	return 0;
}
late_initcall(s31_dma_diag_init);
