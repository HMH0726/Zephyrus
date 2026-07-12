#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void) {
    printk("Hello World! 歡迎來到 CLRFLASH 的世界！\n");

    /* RTOS 主迴圈：每秒醒來一次，其他時間進入深度睡眠省電 */
    while (1) {
        k_sleep(K_MSEC(1000));
    }
    return 0;
}
