
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <stdio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>    


#define MY_GPIO_NODE DT_NODELABEL(toggle)
#define MY_GPIO_NODE_BUTTON DT_NODELABEL(my_button)
#define DEBOUNCE_MS         50   


static const struct gpio_dt_spec toggle_gpio = GPIO_DT_SPEC_GET(MY_GPIO_NODE, gpios);
static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(MY_GPIO_NODE_BUTTON, gpios);

const struct device *const mpu9250 = DEVICE_DT_GET_ANY(invensense_mpu9250);
                                       
static struct gpio_callback button_cb_data;   

struct sample_window {
	struct sensor_value windows[20][3];
};


// queue carries sample windows, can carry 4 sample windows at a time
K_MSGQ_DEFINE(windows_msgq, sizeof(struct sample_window), 4, 1);

// sesnor_value is zephyr specific: value can be obtained using the formula val1 + val2 * 10^(-6)
struct sensor_value accel[3];

struct k_timer my_timer;
int counter =0;

static struct k_work_delayable debounce_work;

K_SEM_DEFINE(sample_sem, 0, 1);



static void debounce_handler(struct k_work *work)
{
    int val = gpio_pin_get_dt(&btn);
    if (val < 0) {
        return;
    }

    // if in input taking state
    if (val == 1) {
        if (k_timer_remaining_get(&my_timer) == 0) {
            // start timer of 20Hz
            k_timer_start(&my_timer, K_MSEC(50), K_MSEC(50));
        } else {
            k_timer_stop(&my_timer);
        }
       // printf("Button pressed\n");
    }
}

// when button pressed, schedule debounce work
void button_pressed(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins) {
	k_work_reschedule(&debounce_work, K_MSEC(DEBOUNCE_MS));
            }


// when timer triggered
void my_timer_handler(struct k_timer *dummy)
{
	// instead of submitting to a work queue
    // k_work_submit(&my_work);

	// timer now signals a producer thread
	k_sem_give(&sample_sem);

}

K_TIMER_DEFINE(my_timer, my_timer_handler, NULL);

void producer_thread(void *a, void *b, void *c) {
	struct sample_window sw = {0};
    while (1) {
        k_sem_take(&sample_sem, K_FOREVER); // block until told to sample
		
		// gpio for checking timer
		gpio_pin_toggle_dt(&toggle_gpio);

		// fetch sample
        int rc = sensor_sample_fetch(mpu9250);
		if (rc == 0) {
			rc = sensor_channel_get(mpu9250, SENSOR_CHAN_ACCEL_XYZ, accel);
		}
        
		for(int i=19; i > 0; i--) {
            memcpy(sw.windows[i], sw.windows[i-1], sizeof(sw.windows[0]));
		}
        memcpy(sw.windows[0], accel, sizeof(accel));
		k_msgq_put(&windows_msgq, &sw, K_NO_WAIT);

		// if (rc == 0) {
		// 				counter++;

		// 	printf("%d,%lld,%d.%06d,%d.%06d,%d.%06d\n", 
		// 		counter,
		// 		uptime_ms,
		// 		accel[0].val1, abs(accel[0].val2), 
		// 		accel[1].val1, abs(accel[1].val2), 
		// 		accel[2].val1, abs(accel[2].val2));
		// } else {
		// 	printf("sample fetch/get failed: %d\n", rc);
		// }

        // push to a message queue instead of printf-ing directly
        // k_msgq_put(&accel_msgq, &accel, K_NO_WAIT);
    }
}

K_THREAD_DEFINE(producer, 1024, producer_thread, NULL, NULL, NULL, 5, 0, 0);

void consumer_thread(void *a, void *b, void *c) {

	struct sample_window sw;
	struct sensor_value xyz[3];

	
	while(1) {
		k_msgq_get(&windows_msgq, &sw, K_FOREVER); // dont want to miss any samples

		printf("Starting new window\n");

		for(int i=0; i<20; i++) {
			int64_t uptime_ms = k_uptime_get();

			xyz[0] = sw.windows[i][0];
			xyz[1] = sw.windows[i][1];
			xyz[2] = sw.windows[i][2];
			printf("%lld, %d.%06d,%d.%06d,%d.%06d\n", 
				uptime_ms,
				xyz[0].val1, abs(xyz[0].val2), 
				xyz[1].val1, abs(xyz[1].val2), 
				xyz[2].val1, abs(xyz[2].val2));
		}
	}
	
}

K_THREAD_DEFINE(consumer, 1024, consumer_thread, NULL, NULL, NULL, 7, 0, 0);



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
}
