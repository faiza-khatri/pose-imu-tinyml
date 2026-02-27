#include <stdio.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/device.h>
#include <zephyr/console/console.h>


static char rx_buf[50];
#define CONSOLE_STACK 1024
/* 1000 msec = 1 sec */
#define SLEEP_MSEC   50

// struct k_work myWork;
// struct k_work pwm_work;

int brightness =0;
unsigned char uart;
static int rx_idx =0;
volatile bool rx_ready = false; 
/*
 * A build error on this line means your board is unsupported.
 * See the sample documentation for information on how to fix this. */


// static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
// static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

K_MSGQ_DEFINE(uart_msgq, 10, 4, 4);
K_THREAD_STACK_DEFINE(console_stack, CONSOLE_STACK);
const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static const struct pwm_dt_spec motor = PWM_DT_SPEC_GET(DT_ALIAS(pwm_motor));

static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(DT_ALIAS(e_stop), gpios);


static struct gpio_callback button_cb_data;

// void my_work_handler(struct k_work *work) {
//     printk("Work processed\n");

// }

volatile bool motor_enabled = true;

void button_pressed(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
       motor_enabled = false;
	   brightness = 0;
	   //pwm_set_pulse_dt(&motor, 0); 
	  printk("Estop pressed! ENter RESEST to restart.\n");
}



// void update_pwm_handler(struct k_work *work){
// 	brightness = 
// 	pwm_set_pulse_dt(&motor, brightness); 
// }

static void uart_fifo_callback(const struct device *dev, void *user_data)
{	
	uint8_t duty_cycle;

	if(!uart_irq_update(uart_dev)){
		return;
	}

	if(!uart_irq_rx_ready(uart_dev) ){

		return ;
	}

	while(uart_fifo_read(uart_dev, &duty_cycle,1)==1){

		uart_poll_out(uart_dev, duty_cycle);

		if(duty_cycle == '\r' || duty_cycle == '\n'){

			rx_buf[rx_idx] = '\0';
			rx_idx=0;
			rx_ready = true;
			k_msgq_put(&uart_msgq, rx_buf, K_NO_WAIT);

		}

		else if (rx_idx < sizeof(rx_buf)-1){
			rx_buf[rx_idx++] = duty_cycle;
		}
	}
}


void console_thread(void *p1, void *p2, void *p3)   
{
	char buffer[50];                              
	printk("Enter duty cycle (0-100):\n");

	while(1){
		k_msgq_get(&uart_msgq, buffer, K_FOREVER); 

		if(!rx_ready){                             
			k_sleep(K_MSEC(10));
			continue;
		}
		rx_ready = false;                         

		if (strcmp(rx_buf, "RESET") == 0) {
			motor_enabled = true;
			printk("Motor reset. You can now set duty cycle.\n");
			continue;
		}

		int duty = atoi(rx_buf);                   

		if(duty < 0 || duty > 100){
			printk("invalid duty cycle\n");
			continue;
		}

		//change brightness here
		if(!motor_enabled){
			printk("Motor disabled! Press RESET to enable.\n");
			continue;
		}

		brightness = duty;
		printk("Duty cycle set to %d%%\n", duty);
	}
}


int main(void)
{
   int ret;
	printk("System started\n");

	// console_init();

	// task 2: device is ready checks
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
    
	//uint32_t pulse = 1000;
	// k_work_init(&pwm_work, update_pwm_handler);

	uart_irq_callback_set(uart_dev, uart_fifo_callback);
	uart_irq_rx_enable(uart_dev);

	struct k_thread console_thread_data;

	k_thread_create(&console_thread_data, console_stack,
                    K_THREAD_STACK_SIZEOF(console_stack),
                    console_thread, NULL, NULL, NULL,
                    5, 0, K_NO_WAIT);

	while (1) {


    // if (uart_poll_in(uart_dev, &uart) == 0) {
    //     printk("Received: %c\n", uart);

    //     brightness = (uart - '0') * 2000;  // convert ASCII digit


    //     k_work_submit(&pwm_work); //this will go in console thread function wahan change brightness acc to duty cycle and submit
    //}
	

		if(motor_enabled){
            pwm_set_pulse_dt(&motor, brightness * motor.period / 100);
        } else {
            pwm_set_pulse_dt(&motor, 0);
        }

       k_sleep(K_MSEC(20));

	}
    

	return 0;
}
