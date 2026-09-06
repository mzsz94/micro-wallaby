/* SPDX-License-Identifier: Apache-2.0 */

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <microwallaby/diag_stats.h>

struct digital_accumulator {
	uint32_t transitions;
	uint32_t assertions;
	uint32_t releases;
	uint32_t max_inactive_ms;
	int64_t inactive_since_ms;
	bool active;
	bool observed;
};

/* Welford mean and M2 use eight fractional bits; no floating point is needed. */
struct axis_accumulator {
	uint32_t samples;
	uint16_t minimum;
	uint16_t maximum;
	int64_t mean_q8;
	uint64_t m2_q8;
};

struct diag_accumulator {
	enum mw_fixture_profile profile;
	uint32_t sample_attempts;
	uint32_t sample_successes;
	uint32_t input_errors;
	uint32_t adc_errors;
	uint32_t gpio_errors;
	struct digital_accumulator touch;
	struct digital_accumulator service_switch;
	struct axis_accumulator axis_primary;
	struct axis_accumulator axis_secondary;
	struct axis_accumulator center_primary;
	struct axis_accumulator center_secondary;
	int64_t center_started_ms;
	bool center_complete;
	int64_t boot_idle_started_ms;
	uint32_t boot_idle_touch_assertions;
	uint32_t boot_idle_switch_assertions;
	enum mw_diag_a0_phase a0_phase;
	uint16_t a0_progress;
	int64_t a0_started_ms;
	struct mw_diag_a0_window_stats a0_idle;
	struct mw_diag_a0_window_stats a0_touched;
	bool a0_processing;
};

static struct k_spinlock diag_lock;
static struct diag_accumulator diag;
static uint16_t a0_samples[MW_DIAG_A0_WINDOW_SAMPLES];

static void increment_saturated(uint32_t *value)
{
	if (*value < UINT32_MAX) {
		(*value)++;
	}
}

static uint32_t elapsed_ms(int64_t start_ms, int64_t now_ms)
{
	uint64_t elapsed;

	if (start_ms < 0 || now_ms <= start_ms) {
		return 0U;
	}

	elapsed = (uint64_t)(now_ms - start_ms);
	return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

static void update_digital(struct digital_accumulator *accumulator, bool active,
			   bool changed, int64_t now_ms)
{
	if (!accumulator->observed) {
		accumulator->observed = true;
		accumulator->active = active;
		accumulator->inactive_since_ms = active ? -1 : now_ms;
		return;
	}

	if (!changed) {
		return;
	}

	increment_saturated(&accumulator->transitions);
	accumulator->active = active;
	if (active) {
		uint32_t idle_ms = elapsed_ms(accumulator->inactive_since_ms, now_ms);

		increment_saturated(&accumulator->assertions);
		if (idle_ms > accumulator->max_inactive_ms) {
			accumulator->max_inactive_ms = idle_ms;
		}
		accumulator->inactive_since_ms = -1;
	} else {
		increment_saturated(&accumulator->releases);
		accumulator->inactive_since_ms = now_ms;
	}
}

static void update_axis(struct axis_accumulator *axis, uint16_t value)
{
	int64_t sample_q8 = (int64_t)value << 8;

	if (axis->samples == 0U) {
		axis->samples = 1U;
		axis->minimum = value;
		axis->maximum = value;
		axis->mean_q8 = sample_q8;
		return;
	}

	if (value < axis->minimum) {
		axis->minimum = value;
	}
	if (value > axis->maximum) {
		axis->maximum = value;
	}

	if (axis->samples < UINT32_MAX) {
		uint32_t count = axis->samples + 1U;
		int64_t delta_q8 = sample_q8 - axis->mean_q8;
		int64_t delta2_q8;
		uint64_t term_q8;

		axis->mean_q8 += delta_q8 / (int64_t)count;
		delta2_q8 = sample_q8 - axis->mean_q8;
		term_q8 = (uint64_t)((delta_q8 * delta2_q8) >> 8);
		if (UINT64_MAX - axis->m2_q8 < term_q8) {
			axis->m2_q8 = UINT64_MAX;
		} else {
			axis->m2_q8 += term_q8;
		}
		axis->samples = count;
	}
}

static uint32_t integer_sqrt(uint64_t value)
{
	uint64_t result = 0U;
	uint64_t bit = 1ULL << 62;

	while (bit > value) {
		bit >>= 2;
	}

	while (bit != 0U) {
		if (value >= result + bit) {
			value -= result + bit;
			result = (result >> 1) + bit;
		} else {
			result >>= 1;
		}
		bit >>= 2;
	}

	return result > UINT32_MAX ? UINT32_MAX : (uint32_t)result;
}

static struct mw_diag_axis_stats axis_snapshot(const struct axis_accumulator *axis)
{
	struct mw_diag_axis_stats result = {
		.samples = axis->samples,
	};

	if (axis->samples == 0U) {
		return result;
	}

	result.minimum = axis->minimum;
	result.maximum = axis->maximum;
	result.mean = (uint16_t)((axis->mean_q8 + 128) >> 8);
	if (axis->samples > 1U) {
		uint64_t variance_q8 = axis->m2_q8 / (axis->samples - 1U);
		uint64_t variance = (variance_q8 + 128U) >> 8;
		uint32_t stddev = integer_sqrt(variance);

		result.noise_stddev = (uint16_t)MIN(stddev, UINT16_MAX);
	}

	return result;
}

static void swap_u16(uint16_t *left, uint16_t *right)
{
	uint16_t value = *left;

	*left = *right;
	*right = value;
}

static void heap_sift_down(uint16_t *values, size_t root, size_t end)
{
	while ((root * 2U) + 1U < end) {
		size_t child = (root * 2U) + 1U;

		if (child + 1U < end && values[child] < values[child + 1U]) {
			child++;
		}
		if (values[root] >= values[child]) {
			return;
		}
		swap_u16(&values[root], &values[child]);
		root = child;
	}
}

static void heap_sort_u16(uint16_t *values, size_t count)
{
	for (size_t root = count / 2U; root > 0U;) {
		root--;
		heap_sift_down(values, root, count);
	}

	for (size_t end = count; end > 1U;) {
		end--;
		swap_u16(&values[0], &values[end]);
		heap_sift_down(values, 0U, end);
	}
}

static void record_a0_sample(uint16_t value, bool touch_active, int64_t now_ms)
{
	enum mw_diag_a0_phase completed_phase = MW_DIAG_A0_DISABLED;
	uint32_t span_ms = 0U;
	k_spinlock_key_t key = k_spin_lock(&diag_lock);
	bool expected_active = diag.a0_phase == MW_DIAG_A0_TOUCHED;

	if ((diag.a0_phase != MW_DIAG_A0_IDLE &&
	     diag.a0_phase != MW_DIAG_A0_TOUCHED) || diag.a0_processing) {
		k_spin_unlock(&diag_lock, key);
		return;
	}

	if (touch_active != expected_active) {
		diag.a0_progress = 0U;
		diag.a0_started_ms = -1;
		k_spin_unlock(&diag_lock, key);
		return;
	}

	if (diag.a0_progress == 0U) {
		diag.a0_started_ms = now_ms;
	}
	if (diag.a0_progress < MW_DIAG_A0_WINDOW_SAMPLES) {
		a0_samples[diag.a0_progress++] = value;
	}

	span_ms = elapsed_ms(diag.a0_started_ms, now_ms);
	if (diag.a0_progress == MW_DIAG_A0_WINDOW_SAMPLES && span_ms >= 5000U) {
		completed_phase = diag.a0_phase;
		diag.a0_processing = true;
	}
	k_spin_unlock(&diag_lock, key);

	if (completed_phase == MW_DIAG_A0_DISABLED) {
		return;
	}

	/* One bounded sort per idle/touched window, outside the spin lock. */
	heap_sort_u16(a0_samples, MW_DIAG_A0_WINDOW_SAMPLES);
	struct mw_diag_a0_window_stats result = {
		.samples = MW_DIAG_A0_WINDOW_SAMPLES,
		.span_ms = span_ms,
		.minimum = a0_samples[0],
		.median = (uint16_t)(((uint32_t)a0_samples[249] + a0_samples[250]) / 2U),
		.maximum = a0_samples[MW_DIAG_A0_WINDOW_SAMPLES - 1U],
		.complete = true,
	};

	key = k_spin_lock(&diag_lock);
	if (completed_phase == MW_DIAG_A0_IDLE) {
		diag.a0_idle = result;
		diag.a0_phase = MW_DIAG_A0_TOUCHED;
	} else {
		diag.a0_touched = result;
		diag.a0_phase = MW_DIAG_A0_COMPLETE;
	}
	diag.a0_progress = 0U;
	diag.a0_started_ms = -1;
	diag.a0_processing = false;
	k_spin_unlock(&diag_lock, key);
}

void mw_diag_stats_init(enum mw_fixture_profile profile, int64_t now_ms)
{
	k_spinlock_key_t key = k_spin_lock(&diag_lock);

	memset(&diag, 0, sizeof(diag));
	diag.profile = profile;
	diag.boot_idle_started_ms = now_ms;
	diag.center_started_ms = -1;
	diag.touch.inactive_since_ms = -1;
	diag.service_switch.inactive_since_ms = -1;
	diag.a0_started_ms = -1;
	diag.a0_phase = profile == MW_FIXTURE_TOUCH_ANALOG ? MW_DIAG_A0_IDLE
								     : MW_DIAG_A0_DISABLED;
	k_spin_unlock(&diag_lock, key);
}

void mw_diag_stats_record_sample_attempt(void)
{
	k_spinlock_key_t key = k_spin_lock(&diag_lock);

	increment_saturated(&diag.sample_attempts);
	k_spin_unlock(&diag_lock, key);
}

void mw_diag_stats_record_sample_success(void)
{
	k_spinlock_key_t key = k_spin_lock(&diag_lock);

	increment_saturated(&diag.sample_successes);
	k_spin_unlock(&diag_lock, key);
}

void mw_diag_stats_record_error(enum mw_diag_error_source source)
{
	k_spinlock_key_t key = k_spin_lock(&diag_lock);

	increment_saturated(&diag.input_errors);
	if (source == MW_DIAG_ERROR_ADC) {
		increment_saturated(&diag.adc_errors);
	} else if (source == MW_DIAG_ERROR_GPIO) {
		increment_saturated(&diag.gpio_errors);
	}
	k_spin_unlock(&diag_lock, key);
}

void mw_diag_stats_record_touch(bool active, bool changed, int64_t now_ms)
{
	k_spinlock_key_t key = k_spin_lock(&diag_lock);
	bool first_active = !diag.touch.observed && active;

	update_digital(&diag.touch, active, changed, now_ms);
	if (diag.profile == MW_FIXTURE_COMBINED &&
	    elapsed_ms(diag.boot_idle_started_ms, now_ms) < MW_DIAG_BOOT_IDLE_MS &&
	    (first_active || (changed && active))) {
		increment_saturated(&diag.boot_idle_touch_assertions);
	}
	k_spin_unlock(&diag_lock, key);
}

void mw_diag_stats_record_service_switch(bool active, bool changed, int64_t now_ms)
{
	k_spinlock_key_t key = k_spin_lock(&diag_lock);
	bool first_active = !diag.service_switch.observed && active;

	update_digital(&diag.service_switch, active, changed, now_ms);
	if (diag.profile == MW_FIXTURE_COMBINED &&
	    elapsed_ms(diag.boot_idle_started_ms, now_ms) < MW_DIAG_BOOT_IDLE_MS &&
	    (first_active || (changed && active))) {
		increment_saturated(&diag.boot_idle_switch_assertions);
	}
	k_spin_unlock(&diag_lock, key);
}

void mw_diag_stats_record_adc(enum mw_diag_adc_channel channel, uint16_t value,
			      bool touch_active, int64_t now_ms)
{
	k_spinlock_key_t key = k_spin_lock(&diag_lock);
	bool record_a0 = diag.profile == MW_FIXTURE_TOUCH_ANALOG &&
			 channel == MW_DIAG_ADC_PRIMARY;
	struct axis_accumulator *center_axis = channel == MW_DIAG_ADC_PRIMARY ?
		&diag.center_primary : &diag.center_secondary;

	if (channel == MW_DIAG_ADC_PRIMARY) {
		update_axis(&diag.axis_primary, value);
	} else {
		update_axis(&diag.axis_secondary, value);
	}

	/* Freeze the first five-second neutral window before any stick sweep. */
	if (diag.profile == MW_FIXTURE_COMBINED && !diag.center_complete) {
		if (diag.center_started_ms < 0) {
			diag.center_started_ms = now_ms;
		}
		if (center_axis->samples < MW_DIAG_CENTER_WINDOW_SAMPLES) {
			update_axis(center_axis, value);
		}
		if (diag.center_primary.samples == MW_DIAG_CENTER_WINDOW_SAMPLES &&
		    diag.center_secondary.samples == MW_DIAG_CENTER_WINDOW_SAMPLES &&
		    elapsed_ms(diag.center_started_ms, now_ms) >=
			    MW_DIAG_CENTER_WINDOW_MS) {
			diag.center_complete = true;
		}
	}
	k_spin_unlock(&diag_lock, key);

	if (record_a0) {
		record_a0_sample(value, touch_active, now_ms);
	}
}

void mw_diag_stats_snapshot(struct mw_diag_snapshot *snapshot, int64_t now_ms)
{
	k_spinlock_key_t key = k_spin_lock(&diag_lock);
	uint32_t touch_inactive_ms = diag.touch.active ? 0U :
		elapsed_ms(diag.touch.inactive_since_ms, now_ms);
	uint32_t switch_inactive_ms = diag.service_switch.active ? 0U :
		elapsed_ms(diag.service_switch.inactive_since_ms, now_ms);
	uint32_t boot_idle_elapsed_ms = elapsed_ms(diag.boot_idle_started_ms, now_ms);
	uint32_t center_elapsed_ms = elapsed_ms(diag.center_started_ms, now_ms);

	*snapshot = (struct mw_diag_snapshot){
		.sample_attempts = diag.sample_attempts,
		.sample_successes = diag.sample_successes,
		.input_errors = diag.input_errors,
		.adc_errors = diag.adc_errors,
		.gpio_errors = diag.gpio_errors,
		.touch = {
			.transitions = diag.touch.transitions,
			.assertions = diag.touch.assertions,
			.releases = diag.touch.releases,
			.inactive_ms = touch_inactive_ms,
			.max_inactive_ms = MAX(diag.touch.max_inactive_ms,
					       touch_inactive_ms),
			.active = diag.touch.active,
			.observed = diag.touch.observed,
		},
		.service_switch = {
			.transitions = diag.service_switch.transitions,
			.assertions = diag.service_switch.assertions,
			.releases = diag.service_switch.releases,
			.inactive_ms = switch_inactive_ms,
			.max_inactive_ms = MAX(diag.service_switch.max_inactive_ms,
					       switch_inactive_ms),
			.active = diag.service_switch.active,
			.observed = diag.service_switch.observed,
		},
		.axis_primary = axis_snapshot(&diag.axis_primary),
		.axis_secondary = axis_snapshot(&diag.axis_secondary),
		.center_primary = axis_snapshot(&diag.center_primary),
		.center_secondary = axis_snapshot(&diag.center_secondary),
		.center_elapsed_ms = MIN(center_elapsed_ms, MW_DIAG_CENTER_WINDOW_MS),
		.center_complete = diag.center_complete,
		.boot_idle_elapsed_ms = MIN(boot_idle_elapsed_ms, MW_DIAG_BOOT_IDLE_MS),
		.boot_idle_touch_assertions = diag.boot_idle_touch_assertions,
		.boot_idle_switch_assertions = diag.boot_idle_switch_assertions,
		.boot_idle_complete = diag.profile == MW_FIXTURE_COMBINED &&
			boot_idle_elapsed_ms >= MW_DIAG_BOOT_IDLE_MS &&
			diag.touch.observed && diag.service_switch.observed,
		.a0_phase = diag.a0_phase,
		.a0_progress = diag.a0_progress,
		.a0_idle = diag.a0_idle,
		.a0_touched = diag.a0_touched,
	};
	k_spin_unlock(&diag_lock, key);
}

const char *mw_diag_a0_phase_name(enum mw_diag_a0_phase phase)
{
	switch (phase) {
	case MW_DIAG_A0_IDLE:
		return "idle";
	case MW_DIAG_A0_TOUCHED:
		return "touched";
	case MW_DIAG_A0_COMPLETE:
		return "complete";
	default:
		return "disabled";
	}
}
