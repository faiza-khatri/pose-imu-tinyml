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

#include "tflite_model_runner.h"   /* exposes: int run_inference(const float *window,
                                                 int n_samples,
                                                 int n_axes); */                                                   


#define MY_GPIO_NODE DT_NODELABEL(toggle)
#define MY_GPIO_NODE_BUTTON DT_NODELABEL(my_button)
#define DEBOUNCE_MS         50   /* tune this if needed */

static const struct gpio_dt_spec toggle_gpio = GPIO_DT_SPEC_GET(MY_GPIO_NODE, gpios);
static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(MY_GPIO_NODE_BUTTON, gpios);

const struct device *const mpu9250 = DEVICE_DT_GET_ANY(invensense_mpu9250);
                                       
static struct gpio_callback button_cb_data;             

typedef struct {
    float data[WINDOW_SIZE * N_AXES];
    int64_t tail_timestamp_ms;      
} window_snapshot_t;

typedef struct {
    int     label;
    int64_t timestamp_ms;
} infer_result_t;

/*
 * sample_q: producer → consumer
 * Depth 2: one being processed + one ready to go.
 * If the consumer is still busy when the third window arrives the
 * producer's k_msgq_put(K_NO_WAIT) call will fail and we increment
 * a drop counter – no blocking, no priority inversion.
 */
#define SAMPLE_Q_DEPTH  2
K_MSGQ_DEFINE(sample_q,
              sizeof(window_snapshot_t),
              SAMPLE_Q_DEPTH,
              _Alignof(window_snapshot_t));

/*
 * result_q: consumer → console
 * Depth 4: console may sleep briefly; small backlog is fine.
 */
#define RESULT_Q_DEPTH  4
K_MSGQ_DEFINE(result_q,
              sizeof(infer_result_t),
              RESULT_Q_DEPTH,
              _Alignof(infer_result_t));


static float  rolling_window[WINDOW_SIZE][N_AXES]; 
static int    window_head = 0;   // index of oldest sample
static int    sample_counter = 0;

// FOR MY GROUP MEMBERS: atomic_init makes sure only one thread can write this var
static atomic_t producer_drops = ATOMIC_INIT(0);

struct sensor_value accel[3];

struct k_timer my_timer;
int counter =0;

static struct k_work_delayable debounce_work;
static bool   sampling_active = false;   // for producer

static void producer_work_handler(struct k_work *work);
K_WORK_DEFINE(producer_work, producer_work_handler);

static void producer_work_handler(struct k_work *work)
{
    // etch sample from sensor
    struct sensor_value accel[3];
    int rc = sensor_sample_fetch(mpu9250);
    if (rc != 0) {
        printk("sensor_sample_fetch failed: %d\n", rc);
        return;
    }
    rc = sensor_channel_get(mpu9250, SENSOR_CHAN_ACCEL_XYZ, accel);
    if (rc != 0) {
        printk("sensor_channel_get failed: %d\n", rc);
        return;
    }

    int64_t now_ms = k_uptime_get();

    // insert into rolling window (overwrite oldest slot) 
    // window_head always points to the slot we are about to fill
    rolling_window[window_head][0] =
        (float)accel[0].val1 + (float)accel[0].val2 * 1e-6f;
    rolling_window[window_head][1] =
        (float)accel[1].val1 + (float)accel[1].val2 * 1e-6f;
    rolling_window[window_head][2] =
        (float)accel[2].val1 + (float)accel[2].val2 * 1e-6f;

    window_head = (window_head + 1) % WINDOW_SIZE;
    sample_counter++;

    /* 3. Only dispatch once we have a full window ------------------- */
    if (sample_counter < WINDOW_SIZE) {
        return;
    }

    /* 4. Build a flat snapshot (row-major, oldest → newest) --------- */
    /*    Copy is intentional: the consumer gets its own immutable     */
    /*    buffer; the producer keeps rolling without any lock.         */
    window_snapshot_t snap;
    snap.tail_timestamp_ms = now_ms;

    /*  window_head now points to the OLDEST sample after the          */
    /*  increment above.                                               */
    for (int i = 0; i < WINDOW_SIZE; i++) {
        int src = (window_head + i) % WINDOW_SIZE;
        snap.data[i * N_AXES + 0] = rolling_window[src][0];
        snap.data[i * N_AXES + 1] = rolling_window[src][1];
        snap.data[i * N_AXES + 2] = rolling_window[src][2];
    }

    /* 5. Non-blocking put ------------------------------------------- */
    /*    K_NO_WAIT: if the consumer is stalled we drop the window and  */
    /*    keep the timer deadline.  Blocking here would desynchronise  */
    /*    the 20 Hz cadence and potentially starve higher-prio work.   */
    if (k_msgq_put(&sample_q, &snap, K_NO_WAIT) != 0) {
        atomic_inc(&producer_drops);
    }
}

static void sampling_timer_handler(struct k_timer *t)
{
    k_work_submit(&producer_work);
}
K_TIMER_DEFINE(sampling_timer, sampling_timer_handler, NULL);



// static void debounce_handler(struct k_work *work)
// {
//     int val = gpio_pin_get_dt(&btn);
//     if (val < 0) {
//         return;
//     }

//     /* Only act on the active (pressed) level */
//     if (val == 1) {
//         if (k_timer_remaining_get(&my_timer) == 0) {
//             k_timer_start(&my_timer, K_MSEC(50), K_MSEC(50));
//         } else {
//             k_timer_stop(&my_timer);
//         }
//        // printf("Button pressed\n");
//     }
// }

static void debounce_handler(struct k_work *work)
{
    int val = gpio_pin_get_dt(&btn);
    if (val < 0) {
        return;
    }
    if (val == 1) {
        if (!sampling_active) {
            /* Reset producer state before starting */
            memset(rolling_window, 0, sizeof(rolling_window));
            window_head    = 0;
            sample_counter = 0;
            k_timer_start(&sampling_timer,
                          K_MSEC(1000 / SAMPLE_RATE_HZ),
                          K_MSEC(1000 / SAMPLE_RATE_HZ));
            sampling_active = true;
            printk("Sampling started\n");
        } else {
            k_timer_stop(&sampling_timer);
            sampling_active = false;
            printk("Sampling stopped\n");
        }
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

// void my_timer_handler(struct k_timer *dummy)
// {
//     k_work_submit(&my_work);
// }

// K_TIMER_DEFINE(my_timer, my_timer_handler, NULL);










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