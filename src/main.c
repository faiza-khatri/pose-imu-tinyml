#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/device.h>
/* 1000 msec = 1 sec */
#define SLEEP_MSEC   50

// struct k_work myWork;
struct k_work pwm_work;
int brightness =0;
unsigned char uart;
/*
 * A build error on this line means your board is unsupported.
 * See the sample documentation for information on how to fix this.
 */
// static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
// static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);


const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static const struct pwm_dt_spec motor = PWM_DT_SPEC_GET(DT_ALIAS(pwm_motor));

static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(DT_ALIAS(eStop), gpios);


static struct gpio_callback button_cb_data;

// void my_work_handler(struct k_work *work) {
//     printk("Work processed\n");

// }

void button_pressed(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
        k_work_submit(&pwm_work);
}



void update_pwm_handler(struct k_work *work){
	brightness = 
	pwm_set_pulse_dt(&motor, brightness); 
}

int main(void)
{
//    int ret;
	printk("System started\n");

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
	k_work_init(&pwm_work, update_pwm_handler);

	while (1) {

    if (uart_poll_in(uart_dev, &uart) == 0) {
        printk("Received: %c\n", uart);

        brightness = (uart - '0') * 2000;  // convert ASCII digit
        k_work_submit(&pwm_work);
    }

    //k_sleep(K_MSEC(10));
}
    

	return 0;
}