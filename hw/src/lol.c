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

int freq = 0;
int rpm = 0;
const int PPR = 64;
const int gear_ratio = 70;
const int ppr = PPR * gear_ratio;
static int target_rpm = 50;

#define CONSOLE_STACK 1024
#define CONSUMER_STACK 1024      
// #define SLEEP_MSEC 50

int brightness = 0;
unsigned char uart;
static int rx_idx = 0;
volatile bool rx_ready = false;

K_MSGQ_DEFINE(uart_msgq, 50, 4, 4);
K_MSGQ_DEFINE(en_msgq, sizeof(int), 1, 4);

K_MSGQ_DEFINE(btn_msgq, 38, 1, 4);
//ticks
K_MSGQ_DEFINE(ticks_msgq, sizeof(uint32_t), 4, 4); 


static volatile int ticks = 0;
uint32_t start_time = 0;
uint32_t end_time = 0;
uint32_t total_time = 0;

K_THREAD_STACK_DEFINE(console_stack, CONSOLE_STACK);
K_THREAD_STACK_DEFINE(consumer_stack, CONSUMER_STACK); // FIX 1: add consumer stack

const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static const struct pwm_dt_spec motor = PWM_DT_SPEC_GET(DT_ALIAS(pwm_motor));
static const struct gpio_dt_spec in1 = GPIO_DT_SPEC_GET(DT_ALIAS(motor_in1), gpios);
static const struct gpio_dt_spec in2 = GPIO_DT_SPEC_GET(DT_ALIAS(motor_in2), gpios);
static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(DT_ALIAS(e_stop), gpios);

// hw3 ---------------------------------------------------
static const struct gpio_dt_spec ena = GPIO_DT_SPEC_GET(DT_ALIAS(encoder_a), gpios);
static const struct gpio_dt_spec enb = GPIO_DT_SPEC_GET(DT_ALIAS(encoder_b), gpios);

K_SEM_DEFINE(rx_ready_sem, 0, 1);

static bool motor_enabled = true;
K_MUTEX_DEFINE(motor_enabled_mutex);
K_MUTEX_DEFINE(brightness_mutex);
K_MUTEX_DEFINE(target_rpm_mutex);

static struct gpio_callback button_cb_data;
static struct gpio_callback enc_a_cb_data;  // FIX 4: callbacks for encoder were
static struct gpio_callback enc_b_cb_data;  // missing entirely
static struct k_thread consumer_thread_data; // FIX 1: thread data struct

void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    int local = 0;
    k_msgq_put(&en_msgq, &local, K_NO_WAIT);
    char printBuff[] = "ESTOP pressed! Enter RESET to enable.";
    k_msgq_put(&btn_msgq, printBuff, K_NO_WAIT);
}

void ticks_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    ticks++;

    if (ticks == 8) {
        end_time = k_cycle_get_32();
        total_time = end_time - start_time;
        start_time = end_time;
        ticks = 0;

        // K_FOREVER in an ISR will deadlock the system if queue is full.
        k_msgq_put(&ticks_msgq, &total_time, K_NO_WAIT);
    }
}

static void uart_fifo_callback(const struct device *dev, void *user_data)
{
    // FIX 7: removed uart_poll_out of uninitialized variables duty_cycle and
    // uart_rpm — these were garbage values being echoed back on every character,
    // corrupting UART output
    uint8_t c;

    if (!uart_irq_update(uart_dev)) return;
    if (!uart_irq_rx_ready(uart_dev)) return;

    while (uart_fifo_read(uart_dev, &c, 1) == 1) {
        if (c == '\r' || c == '\n') {
            rx_buf[rx_idx] = '\0';
            rx_idx = 0;
            k_msgq_put(&uart_msgq, rx_buf, K_NO_WAIT);
            k_sem_give(&rx_ready_sem);
        } else if (rx_idx < sizeof(rx_buf) - 1) {
            rx_buf[rx_idx++] = c;
        }
    }
}

void console_thread(void *p1, void *p2, void *p3)
{
    char buffer_rpm[50];
    printk("Enter target RPM or RESET:\n");

    while (1) {
        k_msgq_get(&uart_msgq, buffer_rpm, K_FOREVER);
        k_sem_take(&rx_ready_sem, K_FOREVER);

        if (strcmp(buffer_rpm, "RESET") == 0) {
            k_mutex_lock(&motor_enabled_mutex, K_FOREVER);
            motor_enabled = true;
            k_mutex_unlock(&motor_enabled_mutex);
            printk("Motor enabled.\n");
            continue;
        }

        int rpm_init = atoi(buffer_rpm);

        if (rpm_init <= 0 || rpm_init > 500) {
            printk("Invalid RPM. Enter 1-500.\n");
            continue;
        }

        k_mutex_lock(&motor_enabled_mutex, K_FOREVER);
        bool enabled = motor_enabled;
        k_mutex_unlock(&motor_enabled_mutex);

        if (!enabled) {
            printk("Motor disabled! Send RESET first.\n");
            continue;
        }

        k_mutex_lock(&target_rpm_mutex, K_FOREVER);
        target_rpm = rpm_init;
        k_mutex_unlock(&target_rpm_mutex);

        printk("Target RPM set to %d\n", rpm_init);
    }
}

void consumer_ticks(void *p1, void *p2, void *p3)
{
    uint32_t ticks_time;
    float measured_rpm = 0;
    const uint32_t cycles_per_sec = sys_clock_hw_cycles_per_sec();
    float Kp = 2.5f;
    float m = 0.5f;  //placeholder
    float b = 10.0f;  //placeholder

    while (1) {
        k_msleep(100);

        if (k_msgq_get(&ticks_msgq, &ticks_time, K_NO_WAIT) == 0 && ticks_time > 0) {
            float time_sec = (float)ticks_time / (float)cycles_per_sec;
            float f_pulse = 8.0f / time_sec;
            measured_rpm = (f_pulse * 60.0f) / ppr;
            printk("Measured RPM: %.2f\n", (double)measured_rpm);
        }

        k_mutex_lock(&target_rpm_mutex, K_FOREVER);
        int local_target = target_rpm;
        k_mutex_unlock(&target_rpm_mutex);

        k_mutex_lock(&motor_enabled_mutex, K_FOREVER);
        bool enabled = motor_enabled;
        k_mutex_unlock(&motor_enabled_mutex);

        if (!enabled) {
            k_mutex_lock(&brightness_mutex, K_FOREVER);
            brightness = 0;
            k_mutex_unlock(&brightness_mutex);
            printk("Motor disabled\n");
            continue;
        }

        float base_pwm = m * local_target + b;
        float error = local_target - measured_rpm;
        float pwm = base_pwm + (Kp * error);

        if (pwm < 0) pwm = 0;
        if (pwm > 100) pwm = 100;

        k_mutex_lock(&brightness_mutex, K_FOREVER);
        brightness = (int)pwm;  
        k_mutex_unlock(&brightness_mutex);

        printk("Target RPM=%d, Measured=%.2f, Error=%.2f, PWM=%.2f%%\n",
               local_target, (double)measured_rpm, (double)error, (double)pwm);
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

    // --- E-STOP button setup (unchanged) ---
    ret = gpio_pin_configure_dt(&btn, GPIO_INPUT);
    if (ret < 0) return 0;

    ret = gpio_pin_interrupt_configure_dt(&btn, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0) return 0;

    gpio_init_callback(&button_cb_data, button_pressed, BIT(btn.pin));
    gpio_add_callback(btn.port, &button_cb_data);


    ret = gpio_pin_configure_dt(&ena, GPIO_INPUT);
    if (ret < 0) return 0;
    ret = gpio_pin_interrupt_configure_dt(&ena, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0) return 0;
    gpio_init_callback(&enc_a_cb_data, ticks_isr, BIT(ena.pin));
    gpio_add_callback(ena.port, &enc_a_cb_data);

    ret = gpio_pin_configure_dt(&enb, GPIO_INPUT);
    if (ret < 0) return 0;
    ret = gpio_pin_interrupt_configure_dt(&enb, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0) return 0;
    gpio_init_callback(&enc_b_cb_data, ticks_isr, BIT(enb.pin));
    gpio_add_callback(enb.port, &enc_b_cb_data);


    ////////////////
    start_time = k_cycle_get_32();

    uart_irq_callback_set(uart_dev, uart_fifo_callback);
    uart_irq_rx_enable(uart_dev);

    struct k_thread console_thread_data;
    k_thread_create(&console_thread_data, console_stack,
                    K_THREAD_STACK_SIZEOF(console_stack),
                    console_thread, NULL, NULL, NULL,
                    5, 0, K_NO_WAIT);


    k_thread_create(&consumer_thread_data, consumer_stack,
                    K_THREAD_STACK_SIZEOF(consumer_stack),
                    consumer_ticks, NULL, NULL, NULL,
                    5, 0, K_NO_WAIT);

    char printBuff[38]; 

    while (1) {
        //  K_FOREVER — blocked here permanently, motor PWM never
        // updated until E-stop pressed. Changed to K_NO_WAIT + sleep.
        if (k_msgq_get(&btn_msgq, printBuff, K_NO_WAIT) == 0) {
            printk("%s\n", printBuff);
            k_mutex_lock(&motor_enabled_mutex, K_FOREVER);
            motor_enabled = false;
            k_mutex_unlock(&motor_enabled_mutex);
        }

        int local_brightness;
        k_mutex_lock(&brightness_mutex, K_FOREVER);
        local_brightness = brightness;
        k_mutex_unlock(&brightness_mutex);

        bool local_enabled;
        k_mutex_lock(&motor_enabled_mutex, K_FOREVER);
        local_enabled = motor_enabled;
        k_mutex_unlock(&motor_enabled_mutex);

        if (local_enabled) {
            gpio_pin_set_dt(&in1, 0);
            gpio_pin_set_dt(&in2, 1);
            pwm_set_pulse_dt(&motor, local_brightness * motor.period / 100);
        } else {
            pwm_set_pulse_dt(&motor, 0);
        }

        // k_msleep(50); 
    }

    return 0;
}