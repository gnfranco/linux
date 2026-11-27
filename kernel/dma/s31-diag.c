// SPDX-License-Identifier: GPL-2.0
/* Experimental, bounded S31 DMA path counters. Not a DMA API. */
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/dma-map-ops.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/limits.h>
#include <linux/percpu.h>
#include <linux/s31-dma-diag.h>
#include <linux/seq_file.h>
#include <linux/string.h>

#define S31_DIAG_DEVICES	8
#define S31_DIAG_NAME	48
#define S31_DIAG_BINS	6
#define S31_DIAG_DIRS	3

struct s31_dma_diag_slot {
	bool valid;
	char name[S31_DIAG_NAME];
	u32 count[S31_DIAG_DIRS][S31_DIAG_BINS][S31_DMA_EVENT_COUNT];
};

struct s31_dma_diag_cpu {
	struct s31_dma_diag_slot slot[S31_DIAG_DEVICES];
	u32 lost;
};

static DEFINE_PER_CPU(struct s31_dma_diag_cpu, s31_dma_diag);

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

void s31_dma_diag_record(struct device *dev, enum dma_data_direction dir,
			 size_t size, enum s31_dma_diag_event event)
{
	struct s31_dma_diag_cpu *cpu;
	struct s31_dma_diag_slot *slot;
	unsigned long flags;
	const char *name;
	unsigned int i;

	if (dev_is_dma_coherent(dev))
		return;

	name = dev_name(dev);
	cpu = get_cpu_ptr(&s31_dma_diag);
	local_irq_save(flags);
	if (!name || !*name || strlen(name) >= S31_DIAG_NAME ||
	    (unsigned int)dir >= S31_DIAG_DIRS ||
	    (unsigned int)event >= S31_DMA_EVENT_COUNT)
		goto lost;

	for (i = 0; i < S31_DIAG_DEVICES; i++) {
		slot = &cpu->slot[i];
		if (!slot->valid) {
			strscpy(slot->name, name, sizeof(slot->name));
			/* Publish the immutable name before readers see the slot. */
			smp_store_release(&slot->valid, true);
		} else if (strcmp(slot->name, name)) {
			continue;
		}
		s31_dma_diag_inc(&slot->count[dir][s31_dma_diag_bin(size)][event]);
		goto out;
	}
lost:
	s31_dma_diag_inc(&cpu->lost);
out:
	local_irq_restore(flags);
	put_cpu_ptr(&s31_dma_diag);
}
EXPORT_SYMBOL_GPL(s31_dma_diag_record);

static void s31_dma_diag_show_slot(struct seq_file *s,
				   const struct s31_dma_diag_slot *slot,
				   unsigned int cpu)
{
	unsigned int dir, bin, event;
	u32 count;

	/* Pairs with publication of the immutable device name. */
	if (!smp_load_acquire(&slot->valid))
		return;

	for (dir = 0; dir < S31_DIAG_DIRS; dir++)
		for (bin = 0; bin < S31_DIAG_BINS; bin++)
			for (event = 0; event < S31_DMA_EVENT_COUNT; event++) {
				count = READ_ONCE(slot->count[dir][bin][event]);
				if (count)
					seq_printf(s, "%u %s %s %s %s %u\n",
						   cpu, slot->name, dir_name[dir],
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
			s31_dma_diag_show_slot(s, &cpu->slot[i], idx);
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
