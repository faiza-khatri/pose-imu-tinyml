
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <stdio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>    
#include <math.h>
#include <string.h>

#include "main_functions.h"


#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_log.h>
#include <tensorflow/lite/schema/schema_generated.h>

extern TfLiteTensor *input;
extern TfLiteTensor *output;
extern tflite::MicroInterpreter *interpreter;
extern uint32_t t_start_full;


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

K_THREAD_STACK_DEFINE(producer_stack, 4096);
K_THREAD_STACK_DEFINE(consumer_stack, 8192);
static struct k_thread producer_data;
static struct k_thread consumer_data;



// struct k_timer my_timer;
int counter =0;

static struct k_work_delayable debounce_work;

K_SEM_DEFINE(sample_sem, 0, 1);

// when timer triggered
void my_timer_handler(struct k_timer *dummy)
{
	// timer now signals a producer thread
	k_sem_give(&sample_sem);

}

K_TIMER_DEFINE(my_timer, my_timer_handler, NULL);

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




void producer_thread(void *a, void *b, void *c) {
	// sesnor_value is zephyr specific: value can be obtained using the formula val1 + val2 * 10^(-6)
	struct sensor_value accel[3];

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
    }
}


void consumer_thread(void *a, void *b, void *c) {
	struct sample_window sw;
	struct sensor_value xyz[3];

	
	while(1) {
		k_msgq_get(&windows_msgq, &sw, K_FOREVER); // dont want to miss any samples

		// guard against failed setup
        if (input == nullptr || output == nullptr || interpreter == nullptr) {
            printk("TFLite not initialized\n");
            continue;
        }

		// give the window to the model
		// before we were giving one input. now we will give 60 (20*3)
		for (int i = 0; i < 20; i++) {
			for (int j = 0; j < 3; j++) {
				// sensor_value to float (val1 + val2/1000000.0)
				float raw = sw.windows[i][j].val1 + 
							sw.windows[i][j].val2 / 1000000.0f;
				
				// apply same normalization as training
				float X_min = -9.8f;
				float X_max =  9.8f;
				float normalized = 2.0f * (raw - X_min) / (X_max - X_min) - 1.0f;
				
				// then quantize the normalized value
				int8_t quantized = (int8_t)round(normalized / input->params.scale)
								+ input->params.zero_point;
				input->data.int8[i * 3 + j] = quantized;
			}
		}

		uint32_t t_start = k_cycle_get_32(); //start timer
		
		TfLiteStatus invoke_status = interpreter->Invoke();
		if (invoke_status != kTfLiteOk) {
			MicroPrintf("Invoke failed\n");
			return;
		}
		
		uint32_t t_end = k_cycle_get_32(); //end timer
		uint32_t cycles = t_end - t_start; 
		uint32_t freq = sys_clock_hw_cycles_per_sec();
		uint32_t latency_ms= (uint32_t)(((uint64_t)cycles * 1000U) / freq);
		
		// obtain the score for each and convert to float
		const char* poses[] = {"Pose 1", "Pose 2", "Pose 3", "Pose 4", "Pose 5"};
		float maxScore = 0;
		int maxIdx = 0;
		for (int i = 0; i < 5; i++) {
			float score = (output->data.int8[i] - output->params.zero_point) 
						* output->params.scale;
			if(score > maxScore) {
				maxScore = score;
				maxIdx = i;
			}
			printf("Pose %d: %f, ", i+1, score);
		}
		printf("\n");
		printf("Predicted Pose: %s | Score: %f | Latency: %u ms\n", poses[maxIdx], maxScore, latency_ms);
		printf("\n");


	}

	uint32_t t_end_full = k_cycle_get_32(); //end timer end-to-end
	uint32_t cycles = t_end_full - t_start_full; 
	uint32_t freq_full = sys_clock_hw_cycles_per_sec();
	uint32_t full_latency_ms= (uint32_t)(((uint64_t)cycles * 1000U) / freq_full);

	printf("\nEnd-to-End Latency: %u ms\n", full_latency_ms);


	
}


int main(void)
{	
	// from main_functions
	// setting up model, allocating tensors
	setup();

	
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

	k_thread_create(&producer_data, producer_stack, K_THREAD_STACK_SIZEOF(producer_stack),
                	producer_thread, NULL, NULL, NULL, 5, 0, K_NO_WAIT);

	k_thread_create(&consumer_data, consumer_stack, K_THREAD_STACK_SIZEOF(consumer_stack),
					consumer_thread, NULL, NULL, NULL, 7, 0, K_NO_WAIT);	



}
