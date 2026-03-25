#include <stdio.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/device.h>
#include <string.h>

static char rx_buf[50];

#define CONSOLE_STACK 1024
#define SLEEP_MSEC 50

int brightness = 0;
static int rx_idx = 0;

// CHANGE: removed volatile bool rx_ready — semaphore replaces it entirely
K_SEM_DEFINE(rx_ready_sem, 0, 1);

static bool motor_enabled = true;
K_MUTEX_DEFINE(motor_enabled_mutex);   // unchanged
K_MUTEX_DEFINE(brightness_mutex);     // unchanged

K_MSGQ_DEFINE(uart_msgq, 50, 4, 4);
K_MSGQ_DEFINE(en_msgq, sizeof(int), 1, 4);
K_MSGQ_DEFINE(btn_msgq, 38, 1, 4);

K_THREAD_STACK_DEFINE(console_stack, CONSOLE_STACK);
const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static const struct pwm_dt_spec motor = PWM_DT_SPEC_GET(DT_ALIAS(pwm_motor));
static const struct gpio_dt_spec in1 = GPIO_DT_SPEC_GET(DT_ALIAS(motor_in1), gpios);
static const struct gpio_dt_spec in2 = GPIO_DT_SPEC_GET(DT_ALIAS(motor_in2), gpios);
static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(DT_ALIAS(e_stop), gpios);

static struct gpio_callback button_cb_data;


void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    // CHANGE: removed motor_enabled = false and brightness = 0 from here.
    // You CANNOT take a mutex in ISR context — doing so will crash or deadlock.
    // Instead, main() reads btn_msgq and does the protected write there.
    int zero = 0;
    k_msgq_put(&en_msgq, &zero, K_NO_WAIT);
    char printBuff[] = "ESTOP pressed! Enter RESET to enable.";
    k_msgq_put(&btn_msgq, printBuff, K_NO_WAIT);
}


static void uart_fifo_callback(const struct device *dev, void *user_data)
{
    uint8_t byte;

    if (!uart_irq_update(uart_dev)) return;
    if (!uart_irq_rx_ready(uart_dev)) return;

    while (uart_fifo_read(uart_dev, &byte, 1) == 1) {
        uart_poll_out(uart_dev, byte);

        if (byte == '\r' || byte == '\n') {
            rx_buf[rx_idx] = '\0';
            rx_idx = 0;
            // CHANGE: removed rx_ready = true (volatile bool is gone).
            // k_msgq_put copies rx_buf into the queue's internal buffer atomically,
            // so no separate mutex is needed for rx_buf here — the copy is the protection.
            k_msgq_put(&uart_msgq, rx_buf, K_NO_WAIT);
            k_sem_give(&rx_ready_sem);   // CHANGE: signal after the put, not before
        } else if (rx_idx < sizeof(rx_buf) - 1) {
            rx_buf[rx_idx++] = byte;
        }
    }
}


void console_thread(void *p1, void *p2, void *p3)
{
    char buffer[50];
    printk("Enter duty cycle (0-100):\n");

    while (1) {
        // CHANGE: block on msgq first — it carries the actual data copy.
        // The sem then confirms the data is a complete line.
        k_msgq_get(&uart_msgq, buffer, K_FOREVER);
        k_sem_take(&rx_ready_sem, K_FOREVER);

        // CHANGE: use buffer (the msgq copy), not rx_buf directly.
        // rx_buf can be overwritten by the ISR the moment msgq_get returns.
        if (strcmp(buffer, "RESET") == 0) {
            k_mutex_lock(&motor_enabled_mutex, K_FOREVER);   // CHANGE: added
            motor_enabled = true;
            k_mutex_unlock(&motor_enabled_mutex);            // CHANGE: added
            printk("Motor reset. You can now set duty cycle.\n");
            continue;
        }

        int duty = atoi(buffer);   // CHANGE: atoi on buffer, not rx_buf
        duty = 100 - duty;

        if (duty < 0 || duty > 100) {
            printk("Invalid duty cycle\n");
            continue;
        }

        k_mutex_lock(&motor_enabled_mutex, K_FOREVER);   // CHANGE: added
        bool enabled = motor_enabled;
        k_mutex_unlock(&motor_enabled_mutex);            // CHANGE: added

        if (!enabled) {
            printk("Motor disabled! Enter RESET to enable.\n");
            continue;
        }

        k_mutex_lock(&brightness_mutex, K_FOREVER);   // CHANGE: added
        brightness = duty;
        k_mutex_unlock(&brightness_mutex);            // CHANGE: added

        printk("Duty cycle set to %d%%\n", 100 - duty);
    }
}


int main(void)
{
    int ret;

    gpio_pin_configure_dt(&in1, GPIO_OUTPUT_ACTIVE);
    gpio_pin_configure_dt(&in2, GPIO_OUTPUT_ACTIVE);

    if (!device_is_ready(uart_dev) ||
        !pwm_is_ready_dt(&motor) ||
        !device_is_ready(btn.port)) {
        return 0;
    }

    ret = gpio_pin_configure_dt(&btn, GPIO_INPUT);
    if (ret < 0) return 0;

    ret = gpio_pin_interrupt_configure_dt(&btn, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0) return 0;

    gpio_init_callback(&button_cb_data, button_pressed, BIT(btn.pin));
    gpio_add_callback(btn.port, &button_cb_data);

    uart_irq_callback_set(uart_dev, uart_fifo_callback);
    uart_irq_rx_enable(uart_dev);

    struct k_thread console_thread_data;
    k_thread_create(&console_thread_data, console_stack,
                    K_THREAD_STACK_SIZEOF(console_stack),
                    console_thread, NULL, NULL, NULL,
                    5, 0, K_NO_WAIT);

    char printBuff[38];

    while (1) {
        if (k_msgq_get(&btn_msgq, printBuff, K_FOREVER) == 0) {
            printk("%s\n", printBuff);

            // CHANGE: motor_enabled write moved here from ISR — safe in thread context
            k_mutex_lock(&motor_enabled_mutex, K_FOREVER);   // CHANGE: added
            motor_enabled = false;
            k_mutex_unlock(&motor_enabled_mutex);            // CHANGE: added
        }

        // CHANGE: K_NO_WAIT here — en_msgq may or may not have a value yet
        int local_en = 0;
        k_msgq_get(&en_msgq, &local_en, K_NO_WAIT);

        int local_brightness;
        k_mutex_lock(&brightness_mutex, K_FOREVER);   // CHANGE: added
        local_brightness = brightness;
        k_mutex_unlock(&brightness_mutex);            // CHANGE: added

        bool local_enabled;
        k_mutex_lock(&motor_enabled_mutex, K_FOREVER);   // CHANGE: added
        local_enabled = motor_enabled;
        k_mutex_unlock(&motor_enabled_mutex);            // CHANGE: added

        if (local_enabled) {
            gpio_pin_set_dt(&in1, 0);
            gpio_pin_set_dt(&in2, 1);
            pwm_set_pulse_dt(&motor, local_brightness * motor.period / 100);
        } else {
            pwm_set_pulse_dt(&motor, 0);
        }
    }

    return 0;
}