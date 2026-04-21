#include <stdio.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/device.h>
#include <zephyr/console/console.h>
#include <string.h>


static char rx_buf[50];

#define CONSOLE_STACK 1024

/* 1000 msec = 1 sec */
#define SLEEP_MSEC   50

// struct k_work myWork;
// struct k_work pwm_work;

int brightness =0;

int64_t end_time;
int64_t start_time;
int64_t total_time;

#define ENCODER_PPR          64      /* pulses per rev, motor shaft */
#define GEAR_RATIO           70      /* gearbox ratio */
#define PULSES_PER_OUT_REV   (ENCODER_PPR * GEAR_RATIO)  /* = 4480 */
#define PULSES_COUNTED       16       /* edges counted per ISR batch */
#define KP               0.3f
#define PLACEHOLDER_M   2.61f    // duty per RPM
#define PLACEHOLDER_B   -14.4f  // duty offset
#define PWM_PERIOD_NS    20000U  /* 50 kHz — adjust to your motor driver */



unsigned char uart;
static int rx_idx =0;
volatile bool rx_ready = false;
int8_t ticks;
/*
 * A build error on this line means your board is unsupported.
 * See the sample documentation for information on how to fix this. */


// static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
// static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

K_MSGQ_DEFINE(uart_msgq, 50, 4, 4);
K_MSGQ_DEFINE(en_msgq, sizeof(int), 1, 4);

K_MSGQ_DEFINE(target_rpm_msgq, sizeof(int), 1, 4);
K_MSGQ_DEFINE(ticks_msgq, sizeof(int64_t), 1, 4);

K_MSGQ_DEFINE(measured_rpm_msgq, sizeof(float), 1, 4);

K_MSGQ_DEFINE(output_msgq, sizeof(float), 1, 4);

K_MSGQ_DEFINE(btn_msgq, 38, 1, 4);

K_THREAD_STACK_DEFINE(console_stack, CONSOLE_STACK);
K_THREAD_STACK_DEFINE(consumer_stack, CONSOLE_STACK);

const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static const struct pwm_dt_spec motor = PWM_DT_SPEC_GET(DT_ALIAS(pwm_motor));
static const struct gpio_dt_spec in1 = GPIO_DT_SPEC_GET(DT_ALIAS(motor_in1), gpios);
static const struct gpio_dt_spec in2 = GPIO_DT_SPEC_GET(DT_ALIAS(motor_in2), gpios);
static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(DT_ALIAS(e_stop), gpios);

static const struct gpio_dt_spec ena = GPIO_DT_SPEC_GET(DT_ALIAS(encoder_a), gpios);
static const struct gpio_dt_spec enb = GPIO_DT_SPEC_GET(DT_ALIAS(encoder_b), gpios);


K_SEM_DEFINE(rx_ready_sem, 0, 1);

static bool motor_enabled = true;
K_MUTEX_DEFINE(motor_enabled_mutex);   // unchanged
K_MUTEX_DEFINE(brightness_mutex);     // unchanged

static struct gpio_callback button_cb_data;
static struct gpio_callback encoder_cb_data;

// void my_work_handler(struct k_work *work) {
//     printk("Work processed\n");

// }

void button_pressed(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
    //    motor_enabled = false;
	   int local = 0;

	   k_msgq_put(&en_msgq, &local, K_NO_WAIT);
	   char printBuff[] = "ESTOP pressed! Enter RESET to enable.";
	   k_msgq_put(&btn_msgq, printBuff, K_NO_WAIT);

}

void ticks_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	int64_t now = k_uptime_ticks();

    if (ticks == 0) {
        start_time = now;
    }

    ticks++;

    if (ticks >= 16) {
        int64_t elapsed = now - start_time;

        k_msgq_put(&ticks_msgq, &elapsed, K_NO_WAIT);
        ticks = 0;
        start_time = 0;
    }
}

void consumer_ticks() {
    int64_t time_elapsed;
	float target_rpm = 0;
	float measured_rpm = 0;

	while (1) {
        if (k_msgq_get(&ticks_msgq, &time_elapsed, K_FOREVER) == 0) {

            /* Convert elapsed ticks to seconds as a double */
            double elapsed_s = (double)time_elapsed / (double)CONFIG_SYS_CLOCK_TICKS_PER_SEC;

			measured_rpm = (float)(((double)PULSES_COUNTED
                                / (double)PULSES_PER_OUT_REV)
                                / elapsed_s * 60.0);

			//printk("Measured RPM = %d\n", (int)measured_rpm);
			k_msgq_purge(&measured_rpm_msgq);
			k_msgq_put(&measured_rpm_msgq, &measured_rpm, K_NO_WAIT);								
			int new_target;
            if (k_msgq_get(&target_rpm_msgq, &new_target, K_NO_WAIT) == 0) {
                target_rpm = (float)new_target;
            }

			/* 2. P control */
			float base_pwm = PLACEHOLDER_M * target_rpm + PLACEHOLDER_B;
			float error  = target_rpm - measured_rpm;
			float output = base_pwm + KP * error;

			if (output < 0.0f)   output = 0.0f;
			if (output > 100.0f) output = 100.0f;  

	// 		printk("target=%.1f measured=%.1f error=%.1f base=%.1f output=%.1f\n",
    //    (double)target_rpm, (double)measured_rpm, 
    //    (double)error, (double)base_pwm, (double)output);



			k_msgq_purge(&output_msgq);
			k_msgq_put(&output_msgq, &output, K_NO_WAIT);
			// k_msgq_overwrite(&output_msgq, &output);
            
        }
    }
}



static void uart_fifo_callback(const struct device *dev, void *user_data)
{
	uint8_t c;

	if(!uart_irq_update(uart_dev)) return;
    if(!uart_irq_rx_ready(uart_dev) ) return ;

	while(uart_fifo_read(uart_dev, &c,1)==1){

		uart_poll_out(uart_dev, c);

		if(c == '\r' || c == '\n'){

			rx_buf[rx_idx] = '\0';
			rx_idx=0;
			// rx_ready = true;
			k_msgq_put(&uart_msgq, rx_buf, K_NO_WAIT);
            k_sem_give(&rx_ready_sem);   

		}

		else if (rx_idx < sizeof(rx_buf)-1){
			rx_buf[rx_idx++] = c;
		}
	}
}


void console_thread(void *p1, void *p2, void *p3)
{
	char uart_rpm[10];
	float measured;


	printk("Enter target rpm: \n");

	k_sem_take(&rx_ready_sem, K_FOREVER);
    k_msgq_get(&uart_msgq, uart_rpm, K_NO_WAIT);

    int rpm = atoi(uart_rpm);
    if (rpm < 0 || rpm > 150) {
        printk("Invalid rpm, defaulting to 0\n");
        rpm = 0;
    }

    k_msgq_put(&target_rpm_msgq, &rpm, K_FOREVER);
    printk("Target RPM = %d\n", rpm);

	
	while(1){
        // if(k_msgq_get(&measured_rpm_msgq, &measured, K_NO_WAIT)==0)
        // // 	printk("Measured RPM = %f\n", measured);

		// k_sem_take(&rx_ready_sem, K_FOREVER);
		// k_msgq_get(&uart_msgq, uart_rpm, K_MSEC(2000));
        // // sscanf(buffer, "%9s %9s", duty_cycle, uart_rpm);
        
		// bool reset = strcmp(uart_rpm, "RESET") == 0;

		// if(reset) {
		// 	k_mutex_lock(&motor_enabled_mutex, K_FOREVER);
		// 	motor_enabled = true;
		// 	k_mutex_unlock(&motor_enabled_mutex);
		// 	printk("Motor reset. You can now set duty cycle.\n");
		// 	continue;

		// } 
			
		// // int duty = atoi(duty_cycle);

        // int rpm = atoi(uart_rpm);

		// // duty = 100 - duty;

		// // if(duty < 0 || duty > 100){
		// // 	printk("invalid duty cycle\n");
		// // 	continue;
		// // }

        // if(rpm < 0 || rpm > 150) {
        //     printk("Invalid rpm\n");
        //     continue;
        // }

        // k_mutex_lock(&motor_enabled_mutex, K_FOREVER);   // CHANGE: added
        // bool enabled = motor_enabled;
        // k_mutex_unlock(&motor_enabled_mutex);            // CHANGE: added

		// if(!enabled){
		// 	printk("Motor disabled! Press RESET to enable.\n");
		// 	continue;
		// }

	    // k_msgq_put(&en_msgq, &duty, K_NO_WAIT);  

	    // if(k_msgq_put(&target_rpm_msgq, &rpm, K_NO_WAIT) == 0)
		if (k_msgq_get(&measured_rpm_msgq, &measured, K_FOREVER) == 0) {
            printk("Target rpm = %d, Measured RPM = %d\n", rpm, (int)measured);
        }

	}
}


int main(void)
{
   int ret;
	// console_init();

	// task 2: device is ready checks

	gpio_pin_configure_dt(&in1, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure_dt(&in2, GPIO_OUTPUT_ACTIVE);

    ret = gpio_pin_configure_dt(&ena, GPIO_INPUT);
    if(ret < 0) return 0;

	
	ret = gpio_pin_interrupt_configure_dt(&ena, GPIO_INT_EDGE_TO_ACTIVE);
    if(ret < 0) return 0;

	if (!device_is_ready(uart_dev) ||
	    !pwm_is_ready_dt(&motor) ||
	    !device_is_ready(btn.port)) {
		return 0;
	}



	ret = gpio_pin_configure_dt(&btn, GPIO_INPUT);


	if (ret < 0) {
		return 0;
	}

    ret = gpio_pin_interrupt_configure_dt(&btn, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0) {
		return 0;
	}

    gpio_init_callback(&button_cb_data, button_pressed, BIT(btn.pin));
	gpio_add_callback(btn.port, &button_cb_data);

    gpio_init_callback(&encoder_cb_data, ticks_isr, BIT(ena.pin));
    gpio_add_callback(ena.port, &encoder_cb_data);


	//uint32_t pulse = 1000;
	// k_work_init(&pwm_work, update_pwm_handler);

	uart_irq_callback_set(uart_dev, uart_fifo_callback);
	uart_irq_rx_enable(uart_dev);

	struct k_thread console_thread_data;

	k_thread_create(&console_thread_data, console_stack,
                    K_THREAD_STACK_SIZEOF(console_stack),
                    console_thread, NULL, NULL, NULL,
                    5, 0, K_NO_WAIT);
	
    
	struct k_thread consumer_thread_data;

	k_thread_create(&consumer_thread_data, consumer_stack,
                    K_THREAD_STACK_SIZEOF(consumer_stack),
                    consumer_ticks, NULL, NULL, NULL,
                    3, 0, K_NO_WAIT);

	char printBuff[37];
	float output = 90;
	int local_pwm = 90;




	while (1) {

		if(k_msgq_get(&btn_msgq, printBuff, K_NO_WAIT) == 0) {
			printk("%s\n", printBuff);

            k_mutex_lock(&motor_enabled_mutex, K_FOREVER);   // CHANGE: added
            motor_enabled = false;
            k_mutex_unlock(&motor_enabled_mutex);            // CHANGE: added
		}

        k_msgq_get(&en_msgq, &local_pwm, K_NO_WAIT);

        // int local_target_rpm = 0;
        // k_msgq_get(&target_rpm_msgq, &local_target_rpm, K_NO_WAIT);

		int got_output = k_msgq_get(&output_msgq, &output, K_NO_WAIT);
		local_pwm = (int)output;
		// printk("PWM applying: %d (got=%d)\n", local_pwm, got_output);

        bool local_enabled;
        k_mutex_lock(&motor_enabled_mutex, K_FOREVER);   // CHANGE: added
        local_enabled = motor_enabled;
        k_mutex_unlock(&motor_enabled_mutex);            // CHANGE: added


		if(local_enabled){
			gpio_pin_set_dt(&in1,0);
			gpio_pin_set_dt(&in2, 1);
            pwm_set_pulse_dt(&motor, local_pwm * motor.period / 100);
        } else {
            pwm_set_pulse_dt(&motor, 0);
        }

       k_sleep(K_MSEC(20));

	}
	return 0;
}