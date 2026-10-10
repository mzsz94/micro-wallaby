/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <microwallaby/app_store.h>
#include <microwallaby/diag_stats.h>
#include <microwallaby/input_filter.h>
#include <microwallaby/sensor_fast.h>

#define USER_NODE DT_PATH(zephyr_user)
#define TOUCH_NODE DT_ALIAS(mw_touch_d0)

#if !DT_NODE_EXISTS(TOUCH_NODE)
#error "The selected fixture overlay must define the mw-touch-d0 alias"
#endif

static const struct gpio_dt_spec touch_d0 = GPIO_DT_SPEC_GET(TOUCH_NODE, gpios);
static const struct adc_dt_spec adc_primary = ADC_DT_SPEC_GET_BY_IDX(USER_NODE, 0);

#if defined(CONFIG_MW_FIXTURE_COMBINED)
#define SERVICE_NODE DT_ALIAS(mw_joystick_sw)
#if !DT_NODE_EXISTS(SERVICE_NODE)
#error "Fixture A must define the mw-joystick-sw alias"
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
static const struct gpio_dt_spec service_sw = GPIO_DT_SPEC_GET(SERVICE_NODE, gpios);
static const struct adc_dt_spec adc_secondary = ADC_DT_SPEC_GET_BY_IDX(USER_NODE, 1);
static struct mw_debounce service_filter;
#endif

static struct mw_debounce touch_filter;
static uint32_t sample_sequence;
static bool hardware_ready;

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

int mw_sensor_fast_init(void)
{
	int ret;
	enum mw_fixture_profile profile = MW_FIXTURE_TOUCH_ANALOG;

#if defined(CONFIG_MW_FIXTURE_COMBINED)
	profile = MW_FIXTURE_COMBINED;
#endif
	mw_diag_stats_init(profile, k_uptime_get());
	hardware_ready = false;

	if (!gpio_is_ready_dt(&touch_d0)) {
		mw_diag_stats_record_error(MW_DIAG_ERROR_GPIO);
		return -ENODEV;
	}
	if (!device_is_ready(adc_primary.dev)) {
		mw_diag_stats_record_error(MW_DIAG_ERROR_ADC);
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&touch_d0, GPIO_INPUT);
	if (ret < 0) {
		mw_diag_stats_record_error(MW_DIAG_ERROR_GPIO);
		return ret;
	}

	ret = configure_adc_channel(&adc_primary);
	if (ret < 0) {
		mw_diag_stats_record_error(MW_DIAG_ERROR_ADC);
		return ret;
	}

#if defined(CONFIG_MW_FIXTURE_COMBINED)
	if (!gpio_is_ready_dt(&service_sw)) {
		mw_diag_stats_record_error(MW_DIAG_ERROR_GPIO);
		return -ENODEV;
	}
	if (!device_is_ready(adc_secondary.dev)) {
		mw_diag_stats_record_error(MW_DIAG_ERROR_ADC);
		return -ENODEV;
	}

	/* The overlay supplies GPIO_PULL_UP. Keep this input in polling mode. */
	ret = gpio_pin_configure_dt(&service_sw, GPIO_INPUT);
	if (ret < 0) {
		mw_diag_stats_record_error(MW_DIAG_ERROR_GPIO);
		return ret;
	}

	ret = configure_adc_channel(&adc_secondary);
	if (ret < 0) {
		mw_diag_stats_record_error(MW_DIAG_ERROR_ADC);
		return ret;
	}
#endif

	hardware_ready = true;
	return 0;
}

int mw_sensor_fast_sample(struct mw_input_snapshot *snapshot)
{
	bool touch_changed;
	int touch_raw;
	int ret;

	mw_diag_stats_record_sample_attempt();
	if (snapshot == NULL) {
		mw_diag_stats_record_error(MW_DIAG_ERROR_INPUT);
		return -EINVAL;
	}

	*snapshot = (struct mw_input_snapshot){
		.sequence = ++sample_sequence,
		.captured_ms = k_uptime_get(),
#if defined(CONFIG_MW_FIXTURE_COMBINED)
		.profile = MW_FIXTURE_COMBINED,
		.joystick_present = true,
#else
		.profile = MW_FIXTURE_TOUCH_ANALOG,
		.touch_analog_present = true,
#endif
	};

	if (!hardware_ready) {
		mw_diag_stats_record_error(MW_DIAG_ERROR_INPUT);
		return -ENODEV;
	}

	touch_raw = gpio_pin_get_raw(touch_d0.port, touch_d0.pin);
	if (touch_raw < 0) {
		mw_stats_increment(MW_STAT_GPIO_ERROR);
		mw_diag_stats_record_error(MW_DIAG_ERROR_GPIO);
		return touch_raw;
	}
	snapshot->touch_raw_level = touch_raw != 0;
	snapshot->touch_active = mw_debounce_update(&touch_filter,
					     raw_level_is_active(&touch_d0, touch_raw),
					     CONFIG_MW_DEBOUNCE_SAMPLES,
					     &touch_changed);
	mw_diag_stats_record_touch(snapshot->touch_active, touch_changed,
				   snapshot->captured_ms);

	ret = read_adc_channel(&adc_primary, &snapshot->analog_primary);
	if (ret < 0) {
		mw_stats_increment(MW_STAT_ADC_ERROR);
		mw_diag_stats_record_error(MW_DIAG_ERROR_ADC);
		return ret;
	}
	mw_diag_stats_record_adc(MW_DIAG_ADC_PRIMARY, snapshot->analog_primary,
				 snapshot->touch_active, snapshot->captured_ms);

#if defined(CONFIG_MW_FIXTURE_COMBINED)
	bool service_changed;
	int service_raw = gpio_pin_get_raw(service_sw.port, service_sw.pin);

	if (service_raw < 0) {
		mw_stats_increment(MW_STAT_GPIO_ERROR);
		mw_diag_stats_record_error(MW_DIAG_ERROR_GPIO);
		return service_raw;
	}
	snapshot->service_raw_level = service_raw != 0;
	snapshot->service_pressed = mw_debounce_update(&service_filter,
						 raw_level_is_active(&service_sw, service_raw),
						 CONFIG_MW_DEBOUNCE_SAMPLES,
						 &service_changed);
	mw_diag_stats_record_service_switch(snapshot->service_pressed, service_changed,
					    snapshot->captured_ms);

	/* The Ameba ADC driver supports one channel per adc_read(). */
	ret = read_adc_channel(&adc_secondary, &snapshot->analog_secondary);
	if (ret < 0) {
		mw_stats_increment(MW_STAT_ADC_ERROR);
		mw_diag_stats_record_error(MW_DIAG_ERROR_ADC);
		return ret;
	}
	mw_diag_stats_record_adc(MW_DIAG_ADC_SECONDARY, snapshot->analog_secondary,
				 snapshot->touch_active, snapshot->captured_ms);

	snapshot->joystick_x_permille = mw_axis_normalize(
		snapshot->analog_primary, CONFIG_MW_JOYSTICK_MIN,
		CONFIG_MW_JOYSTICK_CENTER, CONFIG_MW_JOYSTICK_MAX,
		CONFIG_MW_JOYSTICK_DEADZONE);
	snapshot->joystick_y_permille = mw_axis_normalize(
		snapshot->analog_secondary, CONFIG_MW_JOYSTICK_MIN,
		CONFIG_MW_JOYSTICK_CENTER, CONFIG_MW_JOYSTICK_MAX,
		CONFIG_MW_JOYSTICK_DEADZONE);
#endif

	snapshot->input_ok = true;
	mw_diag_stats_record_sample_success();
	return 0;
}

const char *mw_fixture_name(enum mw_fixture_profile profile)
{
	return profile == MW_FIXTURE_COMBINED ? "combined" : "touch-analog";
}
