/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT gpio_leds

/**
 * @file
 * @brief GPIO driven LEDs
 */

#include <zephyr/drivers/led.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(led_gpio, CONFIG_LED_LOG_LEVEL);

struct led_gpio_config {
	size_t num_leds;
	const struct gpio_dt_spec *led;
};

struct led_gpio_blink_context {
	struct k_work_delayable work;
	const struct device *dev;
	uint32_t led_idx;
	uint32_t on_time_ms;
	uint32_t off_time_ms;
	bool current_state;
};

struct led_gpio_data {
	struct led_gpio_blink_context *blink_ctx;
};

static void led_gpio_blink_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct led_gpio_blink_context *ctx = CONTAINER_OF(dwork,
							   struct led_gpio_blink_context,
							   work);
	const struct device *dev = ctx->dev;
	const struct led_gpio_config *config = dev->config;
	const struct gpio_dt_spec *led_gpio = &config->led[ctx->led_idx];
	int ret;

	/* Toggle LED state */
	ctx->current_state = !ctx->current_state;
	ret = gpio_pin_set_dt(led_gpio, ctx->current_state);
	if (ret < 0) {
		LOG_ERR("Failed to toggle LED %u: %d", ctx->led_idx, ret);
		return;
	}

	/* Schedule next toggle */
	uint32_t delay_ms = ctx->current_state ? ctx->on_time_ms : ctx->off_time_ms;
	if (delay_ms > 0) {
		k_work_reschedule(&ctx->work, K_MSEC(delay_ms));
	}
}

static int led_gpio_set_brightness(const struct device *dev, uint32_t led, uint8_t value)
{
	const struct led_gpio_config *config = dev->config;
	struct led_gpio_data *data = dev->data;
	const struct gpio_dt_spec *led_gpio;

	if ((led >= config->num_leds) || (value > 100)) {
		return -EINVAL;
	}

	/* Cancel any ongoing blink operation for this LED */
	if (data && data->blink_ctx) {
		k_work_cancel_delayable(&data->blink_ctx[led].work);
	}

	led_gpio = &config->led[led];

	return gpio_pin_set_dt(led_gpio, value > 0);
}

static int led_gpio_on(const struct device *dev, uint32_t led)
{
	return led_gpio_set_brightness(dev, led, 100);
}

static int led_gpio_off(const struct device *dev, uint32_t led)
{
	return led_gpio_set_brightness(dev, led, 0);
}

static int led_gpio_blink(const struct device *dev, uint32_t led,
			  uint32_t delay_on, uint32_t delay_off)
{
	const struct led_gpio_config *config = dev->config;
	struct led_gpio_data *data = dev->data;
	struct led_gpio_blink_context *ctx;

	if (led >= config->num_leds) {
		return -EINVAL;
	}

	if (!data || !data->blink_ctx) {
		return -ENOTSUP;
	}

	ctx = &data->blink_ctx[led];

	/* Cancel any existing blink operation */
	k_work_cancel_delayable(&ctx->work);

	/* If delay_on is 0, keep LED off */
	if (delay_on == 0) {
		return led_gpio_off(dev, led);
	}

	/* If delay_off is 0, keep LED on */
	if (delay_off == 0) {
		return led_gpio_on(dev, led);
	}

	/* Initialize blink context */
	ctx->dev = dev;
	ctx->led_idx = led;
	ctx->on_time_ms = delay_on;
	ctx->off_time_ms = delay_off;
	ctx->current_state = false; /* Will be toggled to true immediately */

	/* Start blinking - turn LED on first */
	ctx->current_state = true;
	int ret = gpio_pin_set_dt(&config->led[led], ctx->current_state);
	if (ret < 0) {
		return ret;
	}

	/* Schedule first toggle (to turn off) */
	k_work_reschedule(&ctx->work, K_MSEC(delay_on));

	return 0;
}

static int led_gpio_init(const struct device *dev)
{
	const struct led_gpio_config *config = dev->config;
	struct led_gpio_data *data = dev->data;
	int err = 0;

	if (!config->num_leds) {
		LOG_ERR("%s: no LEDs found (DT child nodes missing)", dev->name);
		err = -ENODEV;
	}

	/* Initialize blink work items for each LED */
	if (data && data->blink_ctx) {
		for (size_t i = 0; i < config->num_leds; i++) {
			k_work_init_delayable(&data->blink_ctx[i].work,
					      led_gpio_blink_work_handler);
		}
	}

	for (size_t i = 0; (i < config->num_leds) && !err; i++) {
		const struct gpio_dt_spec *led = &config->led[i];

		if (device_is_ready(led->port)) {
			err = gpio_pin_configure_dt(led, GPIO_OUTPUT_INACTIVE);

			if (err) {
				LOG_ERR("Cannot configure GPIO (err %d)", err);
			}
		} else {
			LOG_ERR("%s: GPIO device not ready", dev->name);
			err = -ENODEV;
		}
	}

	return err;
}

static const struct led_driver_api led_gpio_api = {
	.on		= led_gpio_on,
	.off		= led_gpio_off,
	.set_brightness	= led_gpio_set_brightness,
	.blink		= led_gpio_blink,
};

#define LED_GPIO_DEVICE(i)					\
								\
static const struct gpio_dt_spec gpio_dt_spec_##i[] = {		\
	DT_INST_FOREACH_CHILD_SEP_VARGS(i, GPIO_DT_SPEC_GET, (,), gpios) \
};								\
								\
static const struct led_gpio_config led_gpio_config_##i = {	\
	.num_leds	= ARRAY_SIZE(gpio_dt_spec_##i),	\
	.led		= gpio_dt_spec_##i,			\
};								\
								\
static struct led_gpio_blink_context led_gpio_blink_ctx_##i[ARRAY_SIZE(gpio_dt_spec_##i)]; \
								\
static struct led_gpio_data led_gpio_data_##i = {		\
	.blink_ctx	= led_gpio_blink_ctx_##i,		\
};								\
								\
DEVICE_DT_INST_DEFINE(i, &led_gpio_init, NULL,			\
		      &led_gpio_data_##i, &led_gpio_config_##i,	\
		      POST_KERNEL, CONFIG_LED_INIT_PRIORITY,	\
		      &led_gpio_api);

DT_INST_FOREACH_STATUS_OKAY(LED_GPIO_DEVICE)
