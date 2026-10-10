/* SPDX-License-Identifier: Apache-2.0 */

#ifndef MICROWALLABY_DIAG_STATS_H_
#define MICROWALLABY_DIAG_STATS_H_

#include <stdbool.h>
#include <stdint.h>

#include <microwallaby/io_types.h>

#define MW_DIAG_A0_WINDOW_SAMPLES 500U
#define MW_DIAG_BOOT_IDLE_MS 300000U
#define MW_DIAG_CENTER_WINDOW_SAMPLES 500U
#define MW_DIAG_CENTER_WINDOW_MS 5000U

enum mw_diag_error_source {
	MW_DIAG_ERROR_INPUT,
	MW_DIAG_ERROR_ADC,
	MW_DIAG_ERROR_GPIO,
};

enum mw_diag_adc_channel {
	MW_DIAG_ADC_PRIMARY,
	MW_DIAG_ADC_SECONDARY,
};

enum mw_diag_a0_phase {
	MW_DIAG_A0_DISABLED,
	MW_DIAG_A0_IDLE,
	MW_DIAG_A0_TOUCHED,
	MW_DIAG_A0_COMPLETE,
};

struct mw_diag_digital_stats {
	uint32_t transitions;
	uint32_t assertions;
	uint32_t releases;
	uint32_t inactive_ms;
	uint32_t max_inactive_ms;
	bool active;
	bool observed;
};

struct mw_diag_axis_stats {
	uint32_t samples;
	uint16_t minimum;
	uint16_t maximum;
	uint16_t mean;
	uint16_t noise_stddev;
};

struct mw_diag_a0_window_stats {
	uint32_t samples;
	uint32_t span_ms;
	uint16_t minimum;
	uint16_t median;
	uint16_t maximum;
	bool complete;
};

struct mw_diag_snapshot {
	uint32_t sample_attempts;
	uint32_t sample_successes;
	uint32_t input_errors;
	uint32_t adc_errors;
	uint32_t gpio_errors;
	struct mw_diag_digital_stats touch;
	struct mw_diag_digital_stats service_switch;
	struct mw_diag_axis_stats axis_primary;
	struct mw_diag_axis_stats axis_secondary;
	struct mw_diag_axis_stats center_primary;
	struct mw_diag_axis_stats center_secondary;
	uint32_t center_elapsed_ms;
	bool center_complete;
	uint32_t boot_idle_elapsed_ms;
	uint32_t boot_idle_touch_assertions;
	uint32_t boot_idle_switch_assertions;
	bool boot_idle_complete;
	enum mw_diag_a0_phase a0_phase;
	uint16_t a0_progress;
	struct mw_diag_a0_window_stats a0_idle;
	struct mw_diag_a0_window_stats a0_touched;
};

void mw_diag_stats_init(enum mw_fixture_profile profile, int64_t now_ms);
void mw_diag_stats_record_sample_attempt(void);
void mw_diag_stats_record_sample_success(void);
void mw_diag_stats_record_error(enum mw_diag_error_source source);
void mw_diag_stats_record_touch(bool active, bool changed, int64_t now_ms);
void mw_diag_stats_record_service_switch(bool active, bool changed, int64_t now_ms);
void mw_diag_stats_record_adc(enum mw_diag_adc_channel channel, uint16_t value,
			      bool touch_active, int64_t now_ms);
void mw_diag_stats_snapshot(struct mw_diag_snapshot *snapshot, int64_t now_ms);
const char *mw_diag_a0_phase_name(enum mw_diag_a0_phase phase);

#endif /* MICROWALLABY_DIAG_STATS_H_ */
