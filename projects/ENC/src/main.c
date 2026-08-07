#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/usb/usb_device.h>

/* 1 個物理刻度 (Click) = 60 度 */
#define DEGREES_PER_CLICK 60

int main(void)
{
    /* 啟動 USB 設備 */
    if (usb_enable(NULL)) {
        return 0;
    }
    k_msleep(3000); 

    printk("USB 序列埠初始化完成！\n");

    const struct device *const qdec_dev = DEVICE_DT_GET(DT_NODELABEL(qdec));

    if (!device_is_ready(qdec_dev)) {
        printk("QDEC 設備尚未準備好！\n");
        return 0;
    }

    printk("編碼器方向偵測開始！請轉動旋鈕...\n");

    struct sensor_value val;
    int absolute_degrees = 0;
    int last_clicks = 0;

    while (1) {
        sensor_sample_fetch(qdec_dev);
        sensor_channel_get(qdec_dev, SENSOR_CHAN_ROTATION, &val);

        int delta_degrees = val.val1;

        if (delta_degrees != 0) {
            absolute_degrees += delta_degrees;
            
            int current_clicks = absolute_degrees / DEGREES_PER_CLICK;
            int clicks_moved = current_clicks - last_clicks;
            
            /* 往右轉：動幾格就印幾次「右」 */
            if (clicks_moved > 0) {
                for (int i = 0; i < clicks_moved; i++) {
                    printk("右\n");
                }
            } 
            /* 往左轉：動幾格就印幾次「左」 */
            else if (clicks_moved < 0) {
                for (int i = 0; i < -clicks_moved; i++) {
                    printk("左\n");
                }
            }
            
            last_clicks = current_clicks;
        }

        /* 輪詢間隔，可依需求調整回應速度 */
        k_msleep(100);
    }
    return 0;
}