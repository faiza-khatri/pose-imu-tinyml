#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
printk("------------------------------------------\n");
printk("STM32F746G Discovery Console Initialized\n");
printk("Device Booted Successfully\n");
printk("------------------------------------------\n");

int counter = 0;

while (1) {
printk("Heartbeat count: %d\n", counter++);
k_msleep(1000); // 1 second delay
}
return 0;
}