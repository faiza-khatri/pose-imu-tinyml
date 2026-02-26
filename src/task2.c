#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/console/console.h>


int main(void)
{

    console_init();
    uint8_t var;



while (1) {
printk("Enter char\n");
var = console_getchar();
if(var=='q'){
    printk("exit");
    k_sleep(K_FOREVER);
}
printk("Recieved: %c", var);
printk(" | Hex: %x\n", var);
printk("------------------------------------------\n");



k_msleep(1000); // 1 second delay
}
return 0;
}


