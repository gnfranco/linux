/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ESP32S31_WIFI_IRQ_STATE_H
#define ESP32S31_WIFI_IRQ_STATE_H

#include <linux/types.h>
#include <linux/limits.h>

/*
 * Pure group protocol, not an IRQ handler. All transitions must be serialized
 * by the future owner under one lock. External hardware proof is a prerequisite
 * to arm/rearm: neither observed bits nor a userspace ACK prove deassertion.
 * No IRQ API, MMIO or dispatch from this header. Zero-init is DISABLED.
 */
#define ESP_WIFI_IRQ_MAC	1U
#define ESP_WIFI_IRQ_PWR	2U
#define ESP_WIFI_IRQ_ALL	(ESP_WIFI_IRQ_MAC | ESP_WIFI_IRQ_PWR)

enum esp_wifi_irq_phase {
	ESP_WIFI_IRQ_DISABLED,
	ESP_WIFI_IRQ_ARMED,
	ESP_WIFI_IRQ_PENDING,
	ESP_WIFI_IRQ_IN_FLIGHT,
	ESP_WIFI_IRQ_FAULT,
};

struct esp_wifi_irq_token {
	u64 epoch;
	u64 sequence;
};

struct esp_wifi_irq_group {
	enum esp_wifi_irq_phase phase;
	u64 epoch;
	u64 sequence;
	unsigned int pending;
};

static inline bool esp_wifi_irq_start(struct esp_wifi_irq_group *g,
				      bool hardware_quiescent)
{
	if (g->phase != ESP_WIFI_IRQ_DISABLED || !hardware_quiescent)
		return false;
	if (g->epoch == U64_MAX) {
		g->phase = ESP_WIFI_IRQ_FAULT;
		return false;
	}
	g->epoch++;
	g->sequence = 0;
	g->pending = 0;
	g->phase = ESP_WIFI_IRQ_ARMED;
	return true;
}

/* Called only for observed events, not assumed peripheral state. */
static inline bool esp_wifi_irq_observe(struct esp_wifi_irq_group *g,
					unsigned int sources)
{
	if (g->phase == ESP_WIFI_IRQ_FAULT)
		return false;
	if (!sources || (sources & ~ESP_WIFI_IRQ_ALL) ||
	    g->phase == ESP_WIFI_IRQ_DISABLED) {
		g->phase = ESP_WIFI_IRQ_FAULT;
		return false;
	}
	g->pending |= sources;
	if (g->phase == ESP_WIFI_IRQ_ARMED)
		g->phase = ESP_WIFI_IRQ_PENDING;
	return true;
}

/* Future caller must have physically masked/synchronized the entire group. */
static inline bool esp_wifi_irq_claim(struct esp_wifi_irq_group *g,
				      bool group_masked,
				      struct esp_wifi_irq_token *token,
				      unsigned int *sources)
{
	if (g->phase != ESP_WIFI_IRQ_PENDING || !group_masked ||
	    !token || !sources)
		return false;
	if (g->sequence == U64_MAX) {
		g->phase = ESP_WIFI_IRQ_FAULT;
		return false;
	}
	g->sequence++;
	token->epoch = g->epoch;
	token->sequence = g->sequence;
	*sources = g->pending;
	g->pending = 0;
	g->phase = ESP_WIFI_IRQ_IN_FLIGHT;
	return true;
}

/*
 * Only the trusted kernel-side completion path may supply drained=true, after
 * peripheral ACK/status qualification. No such path exists yet. Stale tokens
 * never change state. Pending events require another claim while still masked.
 */
static inline bool esp_wifi_irq_complete(struct esp_wifi_irq_group *g,
					 struct esp_wifi_irq_token token,
					 bool drained)
{
	if (g->phase != ESP_WIFI_IRQ_IN_FLIGHT ||
	    token.epoch != g->epoch || token.sequence != g->sequence)
		return false;
	if (!drained) {
		g->phase = ESP_WIFI_IRQ_FAULT;
		return false;
	}
	g->phase = g->pending ? ESP_WIFI_IRQ_PENDING : ESP_WIFI_IRQ_ARMED;
	return true;
}

/* Stop/recovery requires masking, synchronize, dispatcher drain and device idle. */
static inline bool esp_wifi_irq_stop(struct esp_wifi_irq_group *g,
				     bool hardware_quiescent)
{
	if (!hardware_quiescent) {
		g->phase = ESP_WIFI_IRQ_FAULT;
		return false;
	}
	g->pending = 0;
	g->phase = ESP_WIFI_IRQ_DISABLED;
	return true;
}

#endif
