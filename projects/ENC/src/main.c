#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/usb/usb_device.h>

/* 自動讀取與計算物理刻度 */
#define ENCODER_STEPS DT_PROP(DT_NODELABEL(qdec), steps)
#define EDGES_PER_CLICK 4
#define DEGREES_PER_CLICK ((EDGES_PER_CLICK * 360) / ENCODER_STEPS)

int main(void)
{
    /* 初始化 USB */
    if (usb_enable(NULL)) {
        return 0;
    }
    k_msleep(3000); 
    printk("USB 序列埠初始化完成！\n");
    printk("目前編碼器設定: 一圈 %d 格，每格 %d 度\n", ENCODER_STEPS, DEGREES_PER_CLICK);

    /* 初始化 QDEC */
    const struct device *const qdec_dev = DEVICE_DT_GET(DT_NODELABEL(qdec));
    if (!device_is_ready(qdec_dev)) {
        printk("QDEC 設備尚未準備好！\n");
        return 0;
    }
    printk("編碼器方向偵測開始！請轉動旋鈕...\n");

    struct sensor_value val;
    
    /* 【優化】只需要一個變數來儲存「還沒換算成格數的剩餘度數」 */
    int accumulated_degrees = 0; 

    while (1) {
        sensor_sample_fetch(qdec_dev);
        sensor_channel_get(qdec_dev, SENSOR_CHAN_ROTATION, &val);

        if (val.val1 != 0) {
            /* 1. 把新轉動的變化量加進暫存區 */
            accumulated_degrees += val.val1;
            
            /* 2. 算算看這些度數可以換成幾「實體格」 */
            int clicks_moved = accumulated_degrees / DEGREES_PER_CLICK;
            
            /* 如果有產生完整的實體格數 */
            if (clicks_moved != 0) {
                
                /* 3. 觸發輸出 */
                if (clicks_moved > 0) {
                    for (int i = 0; i < clicks_moved; i++) printk("右\n");
                } else {
                    for (int i = 0; i < -clicks_moved; i++) printk("左\n");
                }
                
                /* 4. 【精簡核心】把已經印出去的格數扣掉，只保留除不盡的「餘數」 */
                accumulated_degrees %= DEGREES_PER_CLICK; 
            }
        }

        k_msleep(25);
    }
    return 0;
}