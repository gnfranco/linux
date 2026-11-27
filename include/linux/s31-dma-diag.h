/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_S31_DMA_DIAG_H
#define _LINUX_S31_DMA_DIAG_H

#include <linux/dma-direction.h>
#include <linux/types.h>

struct device;

enum s31_dma_diag_event {
	S31_DMA_DIRECT,
	S31_DMA_FORCE_OK,
	S31_DMA_FORCE_FAIL,
	S31_DMA_ADDR_OK,
	S31_DMA_ADDR_FAIL,
	S31_DMA_ALIGN_OK,
	S31_DMA_ALIGN_FAIL,
	S31_DMA_DWC2_TEMP_OK,
	S31_DMA_DWC2_TEMP_FAIL,
	S31_DMA_EVENT_COUNT,
};

#ifdef CONFIG_S31_DMA_DIAGNOSTICS
void s31_dma_diag_record(struct device *dev, enum dma_data_direction dir,
			 size_t size, enum s31_dma_diag_event event);
#else
static inline void s31_dma_diag_record(struct device *dev,
			enum dma_data_direction dir, size_t size,
			enum s31_dma_diag_event event)
{
}
#endif

#endif
