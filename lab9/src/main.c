/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <stdio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>    


#define MY_GPIO_NODE DT_NODELABEL(toggle)
#define MY_GPIO_NODE_BUTTON DT_NODELABEL(my_button)
#define DEBOUNCE_MS         50   /* tune this if needed */

static const struct gpio_dt_spec toggle_gpio = GPIO_DT_SPEC_GET(MY_GPIO_NODE, gpios);
static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(MY_GPIO_NODE_BUTTON, gpios);

const struct device *const mpu9250 = DEVICE_DT_GET_ANY(invensense_mpu9250);

static struct gpio_callback button_cb_data;


struct sensor_value accel[3];

struct k_timer my_timer;
int counter =0;

static struct k_work_delayable debounce_work;

static void debounce_handler(struct k_work *work)
{
    int val = gpio_pin_get_dt(&btn);
    if (val < 0) {
        return;
    }

    /* Only act on the active (pressed) level */
    if (val == 1) {
        if (k_timer_remaining_get(&my_timer) == 0) {
            k_timer_start(&my_timer, K_MSEC(50), K_MSEC(50));
        } else {
            k_timer_stop(&my_timer);
        }
       // printf("Button pressed\n");
    }
}

void button_pressed(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins) {
	k_work_reschedule(&debounce_work, K_MSEC(DEBOUNCE_MS));
}


void gpio_toggle_work_handler(struct k_work *work) {

	gpio_pin_toggle_dt(&toggle_gpio);
	int64_t uptime_ms = k_uptime_get();

	int rc = sensor_sample_fetch(mpu9250);

	if (rc == 0) {
		rc = sensor_channel_get(mpu9250, SENSOR_CHAN_ACCEL_XYZ, accel);
	}
	// if (rc == 0) {
	// 	rc = sensor_channel_get(mpu9250, SENSOR_CHAN_GYRO_XYZ, gyro);
	// }
	

	if (rc == 0) {
					counter++;

		printf("%d,%lld,%d.%06d,%d.%06d,%d.%06d\n", 
			counter,
			uptime_ms,
			accel[0].val1, abs(accel[0].val2), 
			accel[1].val1, abs(accel[1].val2), 
			accel[2].val1, abs(accel[2].val2));
	} else {
		printf("sample fetch/get failed: %d\n", rc);
	}

};

K_WORK_DEFINE(my_work, gpio_toggle_work_handler);

void my_timer_handler(struct k_timer *dummy)
{
    k_work_submit(&my_work);
}

K_TIMER_DEFINE(my_timer, my_timer_handler, NULL);






int main(void)
{	
	k_work_init_delayable(&debounce_work, debounce_handler);
	int ret = gpio_pin_configure_dt(&btn, GPIO_INPUT);


	if (ret < 0) {
		return 0;
	}

    ret = gpio_pin_interrupt_configure_dt(&btn, GPIO_INT_EDGE_BOTH);
    if (ret < 0) {
		return 0;
	}

    gpio_init_callback(&button_cb_data, button_pressed, BIT(btn.pin));
	gpio_add_callback(btn.port, &button_cb_data);

	if(!gpio_is_ready_dt(&toggle_gpio)) {
		return 0;
	}

	gpio_pin_configure_dt(&toggle_gpio, GPIO_OUTPUT_INACTIVE);

    const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));

	uint8_t reg = 0x75;
	uint8_t chipId = 0;

	i2c_write_read(i2c_dev, 0x68, &reg, 1, &chipId, 1);

	// printk("WHO_AM_I = 0x%02x\n", chipId);

    if (!mpu9250 || !device_is_ready(mpu9250)) {
        printk("Device is not ready or not found\n");
        return 0;
    }
	printf("timestamp_ms,accel_x,accel_y,accel_z\n");



    // struct sensor_value gyro[3];
    // struct sensor_value accel_x;

    // while (1) {


	// 	k_sleep(K_MSEC(1000));
	// }   
}