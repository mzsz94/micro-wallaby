/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <microwallaby/input_filter.h>
#include <microwallaby_web/joystick.h>

LOG_MODULE_REGISTER(mw_joystick, CONFIG_MW_WEB_LOG_LEVEL);

#define USER_NODE DT_PATH(zephyr_user)
#define SWITCH_NODE DT_ALIAS(mw_joystick_sw)

#if !DT_NODE_EXISTS(SWITCH_NODE)
#error "The NU-87 web NES overlay must define the mw-joystick-sw alias"
#endif

BUILD_ASSERT(CONFIG_MW_JOYSTICK_MIN < CONFIG_MW_JOYSTICK_CENTER,
	     "joystick minimum must be below center");
BUILD_ASSERT(CONFIG_MW_JOYSTICK_CENTER < CONFIG_MW_JOYSTICK_MAX,
	     "joystick center must be below maximum");
BUILD_ASSERT(CONFIG_MW_JOYSTICK_DEADZONE <
	     (CONFIG_MW_JOYSTICK_CENTER - CONFIG_MW_JOYSTICK_MIN),
	     "joystick dead-zone must fit below center");
BUILD_ASSERT(CONFIG_MW_JOYSTICK_DEADZONE <
	     (CONFIG_MW_JOYSTICK_MAX - CONFIG_MW_JOYSTICK_CENTER),
	     "joystick dead-zone must fit above center");

static const struct gpio_dt_spec joystick_sw = GPIO_DT_SPEC_GET(SWITCH_NODE, gpios);
static const struct adc_dt_spec joystick_x = ADC_DT_SPEC_GET_BY_NAME(USER_NODE, joystick_x);
static const struct adc_dt_spec joystick_y = ADC_DT_SPEC_GET_BY_NAME(USER_NODE, joystick_y);

static struct mw_debounce switch_filter;
static struct mw_joystick_sample latest_sample;
static bool latest_ready;
static uint32_t sample_sequence;
static atomic_t samples_total;
static atomic_t input_errors;
static atomic_t deadline_misses;

K_MUTEX_DEFINE(sample_lock);
K_THREAD_STACK_DEFINE(sample_stack, CONFIG_MW_JOYSTICK_STACK_SIZE);
static struct k_thread sample_thread_data;

static bool raw_level_is_active(const struct gpio_dt_spec *spec, int raw_level)
{
	bool active_low = (spec->dt_flags & GPIO_ACTIVE_LOW) != 0U;

	return active_low ? raw_level == 0 : raw_level != 0;
}

static int configure_adc_channel(const struct adc_dt_spec *spec)
{
	struct adc_channel_cfg channel = {
		.gain = ADC_GAIN_1,
		.reference = ADC_REF_INTERNAL,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
		.channel_id = spec->channel_id,
		.differential = false,
	};

	return adc_channel_setup(spec->dev, &channel);
}

static int read_adc_channel(const struct adc_dt_spec *spec, uint16_t *value)
{
	struct adc_sequence sequence = {
		.channels = BIT(spec->channel_id),
		.buffer = value,
		.buffer_size = sizeof(*value),
		.resolution = CONFIG_MW_ADC_RESOLUTION,
	};

	return adc_read(spec->dev, &sequence);
}

static void publish_sample(const struct mw_joystick_sample *sample)
{
	k_mutex_lock(&sample_lock, K_FOREVER);
	latest_sample = *sample;
	latest_ready = true;
	k_mutex_unlock(&sample_lock);
}

static int sample_once(struct mw_joystick_sample *sample)
{
	bool switch_changed;
	int switch_raw;
	int ret;

	*sample = (struct mw_joystick_sample) {
		.sequence = ++sample_sequence,
		.captured_ms = k_uptime_get(),
	};

	switch_raw = gpio_pin_get_raw(joystick_sw.port, joystick_sw.pin);
	if (switch_raw < 0) {
		return switch_raw;
	}
	sample->sw_pressed = mw_debounce_update(
		&switch_filter, raw_level_is_active(&joystick_sw, switch_raw),
		CONFIG_MW_DEBOUNCE_SAMPLES, &switch_changed);

	/* The Ameba ADC driver accepts one channel per adc_read(). */
	ret = read_adc_channel(&joystick_x, &sample->raw_x);
	if (ret < 0) {
		return ret;
	}
	ret = read_adc_channel(&joystick_y, &sample->raw_y);
	if (ret < 0) {
		return ret;
	}

	sample->x_permille = mw_axis_normalize(
		sample->raw_x, CONFIG_MW_JOYSTICK_MIN, CONFIG_MW_JOYSTICK_CENTER,
		CONFIG_MW_JOYSTICK_MAX, CONFIG_MW_JOYSTICK_DEADZONE);
	sample->y_permille = mw_axis_normalize(
		sample->raw_y, CONFIG_MW_JOYSTICK_MIN, CONFIG_MW_JOYSTICK_CENTER,
		CONFIG_MW_JOYSTICK_MAX, CONFIG_MW_JOYSTICK_DEADZONE);
	sample->input_ok = true;

	return 0;
}

static void sample_thread(void *unused1, void *unused2, void *unused3)
{
	const int64_t period_ms = CONFIG_MW_JOYSTICK_SAMPLE_PERIOD_MS;
	int64_t next_deadline_ms = k_uptime_get();
	uint32_t consecutive_errors = 0U;

	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	while (true) {
		struct mw_joystick_sample sample;
		int ret = sample_once(&sample);
		int64_t now_ms;

		atomic_inc(&samples_total);
		if (ret < 0) {
			sample.input_ok = false;
			atomic_inc(&input_errors);
			consecutive_errors++;
			if (consecutive_errors == 1U || consecutive_errors % 100U == 0U) {
				LOG_ERR("Joystick sampling failed: %d (count=%u)", ret,
					consecutive_errors);
			}
		} else {
			consecutive_errors = 0U;
		}
		publish_sample(&sample);

		/* Use absolute deadlines so ADC time does not accumulate as sampler
		 * drift. Count every skipped 10 ms slot for the soak-test record.
		 */
		next_deadline_ms += period_ms;
		now_ms = k_uptime_get();
		if (now_ms > next_deadline_ms) {
			uint32_t missed = (uint32_t)((now_ms - next_deadline_ms) /
						     period_ms) + 1U;

			atomic_add(&deadline_misses, (atomic_val_t)missed);
			next_deadline_ms += (int64_t)missed * period_ms;
		}
		k_sleep(K_TIMEOUT_ABS_MS(next_deadline_ms));
	}
}

int mw_joystick_start(void)
{
	int ret;

	if (!gpio_is_ready_dt(&joystick_sw) || !device_is_ready(joystick_x.dev) ||
	    !device_is_ready(joystick_y.dev)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&joystick_sw, GPIO_INPUT);
	if (ret < 0) {
		return ret;
	}
	ret = configure_adc_channel(&joystick_x);
	if (ret < 0) {
		return ret;
	}
	ret = configure_adc_channel(&joystick_y);
	if (ret < 0) {
		return ret;
	}

	k_thread_create(&sample_thread_data, sample_stack,
			K_THREAD_STACK_SIZEOF(sample_stack), sample_thread,
			NULL, NULL, NULL, CONFIG_MW_JOYSTICK_THREAD_PRIORITY, 0,
			K_NO_WAIT);
	k_thread_name_set(&sample_thread_data, "mw_joystick");
	LOG_INF("Joystick sampler started at %d Hz",
		1000 / CONFIG_MW_JOYSTICK_SAMPLE_PERIOD_MS);

	return 0;
}

bool mw_joystick_latest(struct mw_joystick_sample *sample)
{
	bool ready;

	if (sample == NULL) {
		return false;
	}

	k_mutex_lock(&sample_lock, K_FOREVER);
	*sample = latest_sample;
	ready = latest_ready;
	k_mutex_unlock(&sample_lock);

	return ready;
}

void mw_joystick_get_stats(struct mw_joystick_stats *stats)
{
	if (stats == NULL) {
		return;
	}

	*stats = (struct mw_joystick_stats) {
		.samples = (uint32_t)atomic_get(&samples_total),
		.input_errors = (uint32_t)atomic_get(&input_errors),
		.deadline_misses = (uint32_t)atomic_get(&deadline_misses),
	};
}
