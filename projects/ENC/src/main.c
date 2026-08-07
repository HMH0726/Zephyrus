#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/usb/usb_device.h>

/* 定義編碼器刻度與度數轉換 */
#define ENCODER_STEPS DT_PROP(DT_NODELABEL(qdec), steps)
#define EDGES_PER_CLICK 4
#define DEGREES_PER_CLICK ((EDGES_PER_CLICK * 360) / ENCODER_STEPS) /* 算出來會是 60 */

int main(void)
{
    /* 初始化 USB (忽略黃色警告) */
    if (usb_enable(NULL)) return 0;
    k_msleep(3000); 

    printk("USB 序列埠初始化完成！\n");
    printk("設定: 一圈 %d 格，每格 %d 度\n", ENCODER_STEPS, DEGREES_PER_CLICK);

    const struct device *const qdec_dev = DEVICE_DT_GET(DT_NODELABEL(qdec));
    if (!device_is_ready(qdec_dev)) {
        printk("QDEC 設備尚未準備好！\n");
        return 0;
    }

    struct sensor_value val;
    int accumulated_degrees = 0; /* 用來收集碎片的撲滿 */

    printk("【正式模式】開始！準備好享受絲般滑順的手感了嗎...\n");

    while (1) {
        sensor_sample_fetch(qdec_dev);
        sensor_channel_get(qdec_dev, SENSOR_CHAN_ROTATION, &val);

        int delta_degrees = val.val1;

        /* 只要有變化，就把度數丟進撲滿 */
        if (delta_degrees != 0) {
            accumulated_degrees += delta_degrees;
            
            /* 檢查撲滿裡面的度數，夠不夠換算成「實體格數」 */
            int clicks_moved = accumulated_degrees / DEGREES_PER_CLICK;
            
            if (clicks_moved != 0) {
                
                /* 觸發鍵盤/巨集輸出 */
                if (clicks_moved > 0) {
                    for (int i = 0; i < clicks_moved; i++) {
                        printk("➡️ 右轉 1 格\n");
                    }
                } else {
                    for (int i = 0; i < -clicks_moved; i++) {
                        printk("⬅️ 左轉 1 格\n");
                    }
                }
                
                /* 把已經印出去的度數扣掉，保留剩下的零頭 */
                accumulated_degrees %= DEGREES_PER_CLICK; 
            }
        }

        /* 保持 20ms 的超高頻率刷新 */
        k_msleep(20);
    }
    return 0;
}