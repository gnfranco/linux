// SPDX-License-Identifier: GPL-2.0
/*
 * Coherent per-device memory handling.
 * Borrowed from i386
 */
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/dma-direct.h>
#include <linux/dma-map-ops.h>
#ifdef CONFIG_S31_WIFI_SRAM_SESSION
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/uaccess.h>
#endif
#ifdef CONFIG_S31_DMA_ARENA_PROOF
#include <linux/genalloc.h>
#include <linux/of.h>
#endif
#ifdef CONFIG_DMA_GLOBAL_POOL_STATS
#include <linux/bitmap.h>
#include <linux/debugfs.h>
#include <linux/log2.h>
#include <linux/seq_file.h>
#endif

struct dma_coherent_mem {
	void		*virt_base;
	dma_addr_t	device_base;
	unsigned long	pfn_base;
	int		size;
	unsigned long	*bitmap;
	spinlock_t	spinlock;
	bool		use_dev_dma_pfn_offset;
#ifdef CONFIG_DMA_GLOBAL_POOL_STATS
	unsigned long	used_pages;
	unsigned long	peak_pages;
	u64		alloc_failures;
#endif
};

static inline struct dma_coherent_mem *dev_get_coherent_memory(struct device *dev)
{
	if (dev && dev->dma_mem)
		return dev->dma_mem;
	return NULL;
}

static inline dma_addr_t dma_get_device_base(struct device *dev,
					     struct dma_coherent_mem * mem)
{
	if (mem->use_dev_dma_pfn_offset)
		return phys_to_dma(dev, PFN_PHYS(mem->pfn_base));
	return mem->device_base;
}

static struct dma_coherent_mem *dma_init_coherent_memory(phys_addr_t phys_addr,
		dma_addr_t device_addr, size_t size, bool use_dma_pfn_offset)
{
	struct dma_coherent_mem *dma_mem;
	int pages = size >> PAGE_SHIFT;
	void *mem_base;

	if (!size)
		return ERR_PTR(-EINVAL);

	mem_base = memremap(phys_addr, size, MEMREMAP_WC);
	if (!mem_base)
		return ERR_PTR(-EINVAL);

	dma_mem = kzalloc(sizeof(struct dma_coherent_mem), GFP_KERNEL);
	if (!dma_mem)
		goto out_unmap_membase;
	dma_mem->bitmap = bitmap_zalloc(pages, GFP_KERNEL);
	if (!dma_mem->bitmap)
		goto out_free_dma_mem;

	dma_mem->virt_base = mem_base;
	dma_mem->device_base = device_addr;
	dma_mem->pfn_base = PFN_DOWN(phys_addr);
	dma_mem->size = pages;
	dma_mem->use_dev_dma_pfn_offset = use_dma_pfn_offset;
	spin_lock_init(&dma_mem->spinlock);

	return dma_mem;

out_free_dma_mem:
	kfree(dma_mem);
out_unmap_membase:
	memunmap(mem_base);
	pr_err("Reserved memory: failed to init DMA memory pool at %pa, size %zd MiB\n",
		&phys_addr, size / SZ_1M);
	return ERR_PTR(-ENOMEM);
}

static void _dma_release_coherent_memory(struct dma_coherent_mem *mem)
{
	if (!mem)
		return;

	memunmap(mem->virt_base);
	bitmap_free(mem->bitmap);
	kfree(mem);
}

static int dma_assign_coherent_memory(struct device *dev,
				      struct dma_coherent_mem *mem)
{
	if (!dev)
		return -ENODEV;

	if (dev->dma_mem)
		return -EBUSY;

	dev->dma_mem = mem;
	return 0;
}

/*
 * Declare a region of memory to be handed out by dma_alloc_coherent() when it
 * is asked for coherent memory for this device.  This shall only be used
 * from platform code, usually based on the device tree description.
 *
 * phys_addr is the CPU physical address to which the memory is currently
 * assigned (this will be ioremapped so the CPU can access the region).
 *
 * device_addr is the DMA address the device needs to be programmed with to
 * actually address this memory (this will be handed out as the dma_addr_t in
 * dma_alloc_coherent()).
 *
 * size is the size of the area (must be a multiple of PAGE_SIZE).
 *
 * As a simplification for the platforms, only *one* such region of memory may
 * be declared per device.
 */
int dma_declare_coherent_memory(struct device *dev, phys_addr_t phys_addr,
				dma_addr_t device_addr, size_t size)
{
	struct dma_coherent_mem *mem;
	int ret;

	mem = dma_init_coherent_memory(phys_addr, device_addr, size, false);
	if (IS_ERR(mem))
		return PTR_ERR(mem);

	ret = dma_assign_coherent_memory(dev, mem);
	if (ret)
		_dma_release_coherent_memory(mem);
	return ret;
}

void dma_release_coherent_memory(struct device *dev)
{
	if (dev) {
		_dma_release_coherent_memory(dev->dma_mem);
		dev->dma_mem = NULL;
	}
}

static void *__dma_alloc_from_coherent(struct device *dev,
				       struct dma_coherent_mem *mem,
				       ssize_t size, dma_addr_t *dma_handle,
				       unsigned long reserve_pages)
{
	int order = get_order(size);
	unsigned long flags;
	int pageno;
	void *ret;

	spin_lock_irqsave(&mem->spinlock, flags);

	if (unlikely(size > ((dma_addr_t)mem->size << PAGE_SHIFT)))
		goto err;

	/* Optional CPU-only lab reservation: check headroom atomically. */
	if (reserve_pages) {
		unsigned long free = mem->size - bitmap_weight(mem->bitmap,
							      mem->size);

		if (free < reserve_pages ||
		    (1UL << order) > free - reserve_pages) {
			spin_unlock_irqrestore(&mem->spinlock, flags);
			return NULL;
		}
	}

	pageno = bitmap_find_free_region(mem->bitmap, mem->size, order);
	if (unlikely(pageno < 0))
		goto err;

#ifdef CONFIG_DMA_GLOBAL_POOL_STATS
	mem->used_pages += 1UL << order;
	mem->peak_pages = max(mem->peak_pages, mem->used_pages);
#endif

	/*
	 * Memory was found in the coherent area.
	 */
	if (dma_handle)
		*dma_handle = dma_get_device_base(dev, mem) +
				((dma_addr_t)pageno << PAGE_SHIFT);
	ret = mem->virt_base + ((dma_addr_t)pageno << PAGE_SHIFT);
	spin_unlock_irqrestore(&mem->spinlock, flags);
	memset(ret, 0, size);
	return ret;
err:
#ifdef CONFIG_DMA_GLOBAL_POOL_STATS
	mem->alloc_failures++;
#endif
	spin_unlock_irqrestore(&mem->spinlock, flags);
	return NULL;
}

/**
 * dma_alloc_from_dev_coherent() - allocate memory from device coherent pool
 * @dev:	device from which we allocate memory
 * @size:	size of requested memory area
 * @dma_handle:	This will be filled with the correct dma handle
 * @ret:	This pointer will be filled with the virtual address
 *		to allocated area.
 *
 * This function should be only called from per-arch dma_alloc_coherent()
 * to support allocation from per-device coherent memory pools.
 *
 * Returns 0 if dma_alloc_coherent should continue with allocating from
 * generic memory areas, or !0 if dma_alloc_coherent should return @ret.
 */
int dma_alloc_from_dev_coherent(struct device *dev, ssize_t size,
		dma_addr_t *dma_handle, void **ret)
{
	struct dma_coherent_mem *mem = dev_get_coherent_memory(dev);

	if (!mem)
		return 0;

	*ret = __dma_alloc_from_coherent(dev, mem, size, dma_handle, 0);
	return 1;
}

static int __dma_release_from_coherent(struct dma_coherent_mem *mem,
				       int order, void *vaddr)
{
	if (mem && vaddr >= mem->virt_base && vaddr <
		   (mem->virt_base + ((dma_addr_t)mem->size << PAGE_SHIFT))) {
		int page = (vaddr - mem->virt_base) >> PAGE_SHIFT;
		unsigned long flags;

		spin_lock_irqsave(&mem->spinlock, flags);
		bitmap_release_region(mem->bitmap, page, order);
#ifdef CONFIG_DMA_GLOBAL_POOL_STATS
		mem->used_pages -= 1UL << order;
#endif
		spin_unlock_irqrestore(&mem->spinlock, flags);
		return 1;
	}
	return 0;
}

/**
 * dma_release_from_dev_coherent() - free memory to device coherent memory pool
 * @dev:	device from which the memory was allocated
 * @order:	the order of pages allocated
 * @vaddr:	virtual address of allocated pages
 *
 * This checks whether the memory was allocated from the per-device
 * coherent memory pool and if so, releases that memory.
 *
 * Returns 1 if we correctly released the memory, or 0 if the caller should
 * proceed with releasing memory from generic pools.
 */
int dma_release_from_dev_coherent(struct device *dev, int order, void *vaddr)
{
	struct dma_coherent_mem *mem = dev_get_coherent_memory(dev);

	return __dma_release_from_coherent(mem, order, vaddr);
}

static int __dma_mmap_from_coherent(struct dma_coherent_mem *mem,
		struct vm_area_struct *vma, void *vaddr, size_t size, int *ret)
{
	if (mem && vaddr >= mem->virt_base && vaddr + size <=
		   (mem->virt_base + ((dma_addr_t)mem->size << PAGE_SHIFT))) {
		unsigned long off = vma->vm_pgoff;
		int start = (vaddr - mem->virt_base) >> PAGE_SHIFT;
		unsigned long user_count = vma_pages(vma);
		int count = PAGE_ALIGN(size) >> PAGE_SHIFT;

		*ret = -ENXIO;
		if (off < count && user_count <= count - off) {
			unsigned long pfn = mem->pfn_base + start + off;
			*ret = remap_pfn_range(vma, vma->vm_start, pfn,
					       user_count << PAGE_SHIFT,
					       vma->vm_page_prot);
		}
		return 1;
	}
	return 0;
}

/**
 * dma_mmap_from_dev_coherent() - mmap memory from the device coherent pool
 * @dev:	device from which the memory was allocated
 * @vma:	vm_area for the userspace memory
 * @vaddr:	cpu address returned by dma_alloc_from_dev_coherent
 * @size:	size of the memory buffer allocated
 * @ret:	result from remap_pfn_range()
 *
 * This checks whether the memory was allocated from the per-device
 * coherent memory pool and if so, maps that memory to the provided vma.
 *
 * Returns 1 if @vaddr belongs to the device coherent pool and the caller
 * should return @ret, or 0 if they should proceed with mapping memory from
 * generic areas.
 */
int dma_mmap_from_dev_coherent(struct device *dev, struct vm_area_struct *vma,
			   void *vaddr, size_t size, int *ret)
{
	struct dma_coherent_mem *mem = dev_get_coherent_memory(dev);

	return __dma_mmap_from_coherent(mem, vma, vaddr, size, ret);
}

#ifdef CONFIG_DMA_GLOBAL_POOL
static struct dma_coherent_mem *dma_coherent_default_memory __ro_after_init;

#ifdef CONFIG_DMA_GLOBAL_POOL_STATS
static int dma_global_pool_stats_show(struct seq_file *m, void *unused)
{
	struct dma_coherent_mem *mem = dma_coherent_default_memory;
	unsigned long flags, used, tracked, peak, largest = 0;
	unsigned long total, pages, start;
	unsigned long *snapshot;
	u64 failures;

	if (!mem || mem->size <= 0)
		return -ENODEV;

	total = mem->size;
	snapshot = bitmap_alloc(total, GFP_KERNEL);
	if (!snapshot)
		return -ENOMEM;

	spin_lock_irqsave(&mem->spinlock, flags);
	bitmap_copy(snapshot, mem->bitmap, total);
	tracked = mem->used_pages;
	peak = mem->peak_pages;
	failures = mem->alloc_failures;
	spin_unlock_irqrestore(&mem->spinlock, flags);

	used = bitmap_weight(snapshot, total);
	/* Match the allocator's power-of-two page extent and alignment. */
	for (pages = rounddown_pow_of_two(total); pages; pages >>= 1) {
		start = bitmap_find_next_zero_area(snapshot, total, 0,
						   pages, pages - 1);
		if (start <= total - pages) {
			largest = pages;
			break;
		}
	}
	bitmap_free(snapshot);

	seq_printf(m, "page_bytes %lu\n", PAGE_SIZE);
	seq_printf(m, "total_pages %lu\n", total);
	seq_printf(m, "used_pages %lu\n", used);
	seq_printf(m, "free_pages %lu\n", total - used);
	seq_printf(m, "tracked_pages %lu\n", tracked);
	seq_printf(m, "peak_pages %lu\n", peak);
	seq_printf(m, "largest_allocatable_pages %lu\n", largest);
	seq_printf(m, "allocation_failures %llu\n", failures);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dma_global_pool_stats);

static int __init dma_global_pool_stats_init(void)
{
	if (dma_coherent_default_memory)
		debugfs_create_file("dma_global_pool_stats", 0400, NULL, NULL,
				    &dma_global_pool_stats_fops);
	return 0;
}
late_initcall(dma_global_pool_stats_init);
#endif

void *dma_alloc_from_global_coherent(struct device *dev, ssize_t size,
				     dma_addr_t *dma_handle)
{
	if (!dma_coherent_default_memory)
		return NULL;

	return __dma_alloc_from_coherent(dev, dma_coherent_default_memory, size,
					 dma_handle, 0);
}

int dma_release_from_global_coherent(int order, void *vaddr)
{
	if (!dma_coherent_default_memory)
		return 0;

	return __dma_release_from_coherent(dma_coherent_default_memory, order,
			vaddr);
}

int dma_mmap_from_global_coherent(struct vm_area_struct *vma, void *vaddr,
				   size_t size, int *ret)
{
	if (!dma_coherent_default_memory)
		return 0;

	return __dma_mmap_from_coherent(dma_coherent_default_memory, vma,
					vaddr, size, ret);
}

#ifdef CONFIG_S31_DMA_ARENA_PROOF
/*
 * CPU-only ownership proof. Do not use a fictitious DMA device: the
 * NULL handle deliberately skips device address translation. Never
 * expose the pool or the acquired extent outside this test.
 */
static int __init s31_dma_arena_proof(void)
{
	struct dma_coherent_mem *mem = dma_coherent_default_memory;
	const size_t bytes = 4 * PAGE_SIZE;
	struct genpool_data_align align_a = { .align = 64 };
	struct genpool_data_align align_b = { .align = 256 };
	struct gen_pool *pool;
	unsigned long a = 0, b = 0, extra = 0;
	void *base, *denied;
	bool pass = false, released;

	if (!of_machine_is_compatible("esp,esp32s31") ||
	    !IS_ENABLED(CONFIG_PAGE_SIZE_4KB) || !mem || mem->size != 64 ||
	    PFN_PHYS(mem->pfn_base) != 0x2f030000) {
		pr_info("s31-dma-arena: SKIP unsupported layout\n");
		return 0;
	}

	/* Impossible floor must deny without consuming a page. */
	denied = __dma_alloc_from_coherent(NULL, mem, bytes, NULL, 64);
	if (denied) {
		/* Complete the allocation's zeroing before returning pages. */
		mb();
		__dma_release_from_coherent(mem, get_order(bytes), denied);
		pr_err("s31-dma-arena: FAIL headroom policy\n");
		return 0;
	}
	base = __dma_alloc_from_coherent(NULL, mem, bytes, NULL, 48);
	if (!base) {
		pr_info("s31-dma-arena: SKIP insufficient headroom or extent\n");
		return 0;
	}
	/* Complete initial WC writes before inspecting the arena. */
	mb();
	if (memchr_inv(base, 0, bytes))
		goto out_release;
	pool = gen_pool_create(6, -1);
	if (!pool)
		goto out_release;
	if (gen_pool_add(pool, (unsigned long)base, bytes, -1))
		goto out_destroy;

	a = gen_pool_alloc_algo(pool, 1600, gen_pool_first_fit_align,
				&align_a);
	b = gen_pool_alloc_algo(pool, 500, gen_pool_first_fit_align,
				&align_b);
	if (!a || !b || !IS_ALIGNED(a, 64) || !IS_ALIGNED(b, 256) ||
	    (a < b + 512 && b < a + 1600) ||
	    gen_pool_avail(pool) != bytes - 1600 - 512)
		goto out_free;
	memset((void *)a, 0xa5, 1600);
	memset((void *)b, 0x5a, 500);
	/* Order WC pattern writes before the readback. */
	mb();
	if (memchr_inv((void *)a, 0xa5, 1600) ||
	    memchr_inv((void *)b, 0x5a, 500))
		goto out_free;

	/* All remaining space is smaller than the original arena. */
	extra = gen_pool_alloc(pool, bytes);
	if (extra)
		goto out_free;
	pass = true;
out_free:
	if (extra)
		gen_pool_free(pool, extra, bytes);
	if (b)
		gen_pool_free(pool, b, 500);
	if (a)
		gen_pool_free(pool, a, 1600);
	if (gen_pool_avail(pool) != bytes)
		pass = false;
out_destroy:
	gen_pool_destroy(pool);
out_release:
	memset(base, 0, bytes);
	/* Finish scrubbing before readback and returning ownership. */
	mb();
	if (memchr_inv(base, 0, bytes))
		pass = false;
	released = __dma_release_from_coherent(mem, get_order(bytes), base);
	if (pass && released)
		pr_info("s31-dma-arena: PASS bytes=%zu floor_pages=48\n",
			bytes);
	else
		pr_err("s31-dma-arena: FAIL allocation or cleanup\n");
	return 0;
}
late_initcall(s31_dma_arena_proof);
#endif

#ifdef CONFIG_S31_WIFI_SRAM_SESSION
#define S31_SESSION_BYTES (4 * PAGE_SIZE)

/*
 * Experimental CPU-only lease. An ordinary file reference is held by
 * each mapped VMA, so closing the descriptor alone cannot free the
 * pages. No DMA address, arbitrary PFN, MMIO or radio access is exposed.
 * The authorized owner must finish CPU accesses before final unmap.
 */
static DEFINE_MUTEX(s31_session_lock);
static void *s31_session_base;
static bool s31_session_mapped;

static int s31_session_open(struct inode *inode, struct file *file)
{
	struct dma_coherent_mem *mem = dma_coherent_default_memory;
	void *base;
	int ret = -EBUSY;

	if (!capable(CAP_SYS_RAWIO) ||
	    (file->f_mode & (FMODE_READ | FMODE_WRITE)) !=
	    (FMODE_READ | FMODE_WRITE))
		return -EPERM;

	mutex_lock(&s31_session_lock);
	if (s31_session_base)
		goto out;
	base = __dma_alloc_from_coherent(NULL, mem, S31_SESSION_BYTES,
					 NULL, 48);
	if (!base) {
		ret = -ENOMEM;
		goto out;
	}
	/* Order initial zeroing before exposing the acquired pages. */
	mb();
	s31_session_base = base;
	file->private_data = base;
	ret = 0;
out:
	mutex_unlock(&s31_session_lock);
	return ret;
}

static int s31_session_release(struct inode *inode, struct file *file)
{
	void *base = file->private_data;

	mutex_lock(&s31_session_lock);
	if (WARN_ON_ONCE(base != s31_session_base))
		goto out;
	memset(base, 0, S31_SESSION_BYTES);
	/* Finish scrubbing before another consumer may acquire pages. */
	mb();
	if (WARN_ON_ONCE(memchr_inv(base, 0, S31_SESSION_BYTES)))
		goto out;
	/*
	 * Keep every user-mapped extent out of other DMA clients until
	 * cross-alias visibility and reuse have been qualified on S31.
	 * Even a failed mapping attempt may have installed some PTEs.
	 */
	if (s31_session_mapped) {
		pr_info("s31-wifi-sram: mapped arena quarantined until reboot\n");
		goto out;
	}
	if (WARN_ON_ONCE(!dma_release_from_global_coherent(
			get_order(S31_SESSION_BYTES), base)))
		goto out;
	s31_session_base = NULL;
out:
	mutex_unlock(&s31_session_lock);
	return 0;
}

static int s31_session_mmap(struct file *file, struct vm_area_struct *vma)
{
	int ret = -ENXIO;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	if (vma->vm_pgoff || vma_pages(vma) != 4 ||
	    !(vma->vm_flags & VM_SHARED) ||
	    (vma->vm_flags & VM_EXEC))
		return -EINVAL;
	vm_flags_clear(vma, VM_MAYEXEC);
	vm_flags_set(vma, VM_IO | VM_DONTCOPY | VM_DONTDUMP | VM_DONTEXPAND);
	mutex_lock(&s31_session_lock);
	s31_session_mapped = true;
	mutex_unlock(&s31_session_lock);
	/*
	 * Match the global pool's MEMREMAP_WC policy. On Sv32 this
	 * does not change PTE cacheability; platform attributes matter.
	 */
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	if (!dma_mmap_from_global_coherent(vma, file->private_data,
					  S31_SESSION_BYTES, &ret))
		return -ENXIO;
	return ret;
}

/* Bounded CPU cross-view test only; no concurrent unsynchronized
 * writes through mapping and file I/O. Normal file references pin
 * ownership for the duration of these operations.
 */
static ssize_t s31_session_read(struct file *file, char __user *buf,
				size_t count, loff_t *pos)
{
	void *tmp;
	size_t copied;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	if (*pos < 0)
		return -EINVAL;
	if (*pos >= S31_SESSION_BYTES || !count)
		return 0;
	count = min_t(size_t, count, S31_SESSION_BYTES - *pos);
	/* Order a preceding CPU access before cross-view readback. */
	mb();
	tmp = kmemdup(file->private_data + *pos, count, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;
	copied = count - copy_to_user(buf, tmp, count);
	kfree(tmp);
	if (!copied)
		return -EFAULT;
	*pos += copied;
	return copied;
}

static ssize_t s31_session_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *pos)
{
	void *tmp;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	if (*pos < 0)
		return -EINVAL;
	if (*pos >= S31_SESSION_BYTES || !count)
		return 0;
	count = min_t(size_t, count, S31_SESSION_BYTES - *pos);
	tmp = memdup_user(buf, count);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);
	memcpy(file->private_data + *pos, tmp, count);
	kfree(tmp);
	/* Complete CPU writes before a subsequent mapped readback. */
	mb();
	*pos += count;
	return count;
}

static const struct file_operations s31_session_fops = {
	.owner = THIS_MODULE,
	.open = s31_session_open,
	.release = s31_session_release,
	.mmap = s31_session_mmap,
	.read = s31_session_read,
	.write = s31_session_write,
};

static struct miscdevice s31_session_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "s31-wifi-sram-lab",
	.fops = &s31_session_fops,
	.mode = 0600,
};

static int __init s31_session_init(void)
{
	struct dma_coherent_mem *mem = dma_coherent_default_memory;

	if (!of_machine_is_compatible("esp,esp32s31") ||
	    !IS_ENABLED(CONFIG_PAGE_SIZE_4KB) || !mem || mem->size != 64 ||
	    PFN_PHYS(mem->pfn_base) != 0x2f030000)
		return -ENODEV;

	return misc_register(&s31_session_misc);
}
late_initcall(s31_session_init);
#endif

int dma_init_global_coherent(phys_addr_t phys_addr, size_t size)
{
	struct dma_coherent_mem *mem;

	mem = dma_init_coherent_memory(phys_addr, phys_addr, size, true);
	if (IS_ERR(mem))
		return PTR_ERR(mem);
	dma_coherent_default_memory = mem;
	pr_info("DMA: default coherent area is set\n");
	return 0;
}
#endif /* CONFIG_DMA_GLOBAL_POOL */

/*
 * Support for reserved memory regions defined in device tree
 */
#ifdef CONFIG_OF_RESERVED_MEM
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>

#ifdef CONFIG_DMA_GLOBAL_POOL
static phys_addr_t dma_reserved_default_memory_base __initdata;
static phys_addr_t dma_reserved_default_memory_size __initdata;
#endif

static int rmem_dma_device_init(struct reserved_mem *rmem, struct device *dev)
{
	struct dma_coherent_mem *mem = rmem->priv;

	if (!mem) {
		mem = dma_init_coherent_memory(rmem->base, rmem->base,
					       rmem->size, true);
		if (IS_ERR(mem))
			return PTR_ERR(mem);
		rmem->priv = mem;
	}

	/* Warn if the device potentially can't use the reserved memory */
	if (mem->device_base + rmem->size - 1 >
	    min_not_zero(dev->coherent_dma_mask, dev->bus_dma_limit))
		dev_warn(dev, "reserved memory is beyond device's set DMA address range\n");

	dma_assign_coherent_memory(dev, mem);
	return 0;
}

static void rmem_dma_device_release(struct reserved_mem *rmem,
				    struct device *dev)
{
	if (dev)
		dev->dma_mem = NULL;
}

static const struct reserved_mem_ops rmem_dma_ops = {
	.device_init	= rmem_dma_device_init,
	.device_release	= rmem_dma_device_release,
};

static int __init rmem_dma_setup(struct reserved_mem *rmem)
{
	unsigned long node = rmem->fdt_node;

	if (of_get_flat_dt_prop(node, "reusable", NULL))
		return -EINVAL;

#ifdef CONFIG_ARM
	if (!of_get_flat_dt_prop(node, "no-map", NULL)) {
		pr_err("Reserved memory: regions without no-map are not yet supported\n");
		return -EINVAL;
	}
#endif

#ifdef CONFIG_DMA_GLOBAL_POOL
	if (of_get_flat_dt_prop(node, "linux,dma-default", NULL)) {
		WARN(dma_reserved_default_memory_size,
		     "Reserved memory: region for default DMA coherent area is redefined\n");
		dma_reserved_default_memory_base = rmem->base;
		dma_reserved_default_memory_size = rmem->size;
	}
#endif

	rmem->ops = &rmem_dma_ops;
	pr_info("Reserved memory: created DMA memory pool at %pa, size %ld MiB\n",
		&rmem->base, (unsigned long)rmem->size / SZ_1M);
	return 0;
}

#ifdef CONFIG_DMA_GLOBAL_POOL
static int __init dma_init_reserved_memory(void)
{
	if (!dma_reserved_default_memory_size)
		return -ENOMEM;
	return dma_init_global_coherent(dma_reserved_default_memory_base,
					dma_reserved_default_memory_size);
}
core_initcall(dma_init_reserved_memory);
#endif /* CONFIG_DMA_GLOBAL_POOL */

RESERVEDMEM_OF_DECLARE(dma, "shared-dma-pool", rmem_dma_setup);
#endif
