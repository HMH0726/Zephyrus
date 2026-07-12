#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/reboot.h>
#include <stdio.h>

#define CH_COUNT 3

// --- 硬體按鈕設定 ---
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_NODELABEL(switch_btn), gpios);
static struct gpio_callback button_cb_data;
static const struct gpio_dt_spec clr_button = GPIO_DT_SPEC_GET(DT_NODELABEL(clear_btn), gpios);
static struct gpio_callback clr_button_cb_data;

// 儲存 Zephyr Identity ID
static uint8_t channel_ids[CH_COUNT] = {0, 1, 2};
static uint8_t current_channel = 0;
static struct bt_conn *current_conn = NULL;
static int64_t last_button_time = 0;
static int64_t last_clr_button_time = 0;

static bool volatile is_switching = false;

// =========================================================
//  【關鍵防護】背景排程器完整宣告區塊
// =========================================================
static void switch_channel_work_handler(struct k_work *work);
K_WORK_DEFINE(switch_channel_work, switch_channel_work_handler);

static void complete_switch_work_handler(struct k_work *work);
K_WORK_DEFINE(complete_switch_work, complete_switch_work_handler);

static void factory_reset_work_handler(struct k_work *work);
K_WORK_DEFINE(factory_reset_work, factory_reset_work_handler);

static void restart_adv_work_handler(struct k_work *work);
K_WORK_DEFINE(restart_adv_work, restart_adv_work_handler);
// =========================================================

// 記憶目前頻道
static int app_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (strcmp(name, "ch") == 0) { read_cb(cb_arg, &current_channel, sizeof(current_channel)); return 0; }
    if (strcmp(name, "ids") == 0) { read_cb(cb_arg, channel_ids, sizeof(channel_ids)); return 0; }
    return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(app_settings, "app", NULL, app_settings_set, NULL, NULL);

// GATT HID 滑鼠服務表
static const uint8_t hid_report_map[] = { 0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0xc0 };
struct hids_info { uint16_t version; uint8_t code; uint8_t flags; } __packed;
static struct hids_info info = { .version = 0x0111, .code = 0x00, .flags = BIT(1) };

static ssize_t read_info(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset) { return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data, sizeof(struct hids_info)); }
static ssize_t read_report_map(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset) { return bt_gatt_attr_read(conn, attr, buf, len, offset, hid_report_map, sizeof(hid_report_map)); }
static ssize_t read_input_report(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset) { return bt_gatt_attr_read(conn, attr, buf, len, offset, NULL, 0); }

BT_GATT_SERVICE_DEFINE(hog_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_info, NULL, &info),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_report_map, NULL, NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ_ENCRYPT, read_input_report, NULL, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT)
);

// 三個頻道的獨立廣播資料與名稱 (附帶 UUID 以避免手機隱藏)
static const struct bt_data ad_ch0[] = { 
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)), 
    BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0x12, 0x18), 
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, 0xC2, 0x03), 
    BT_DATA(BT_DATA_NAME_COMPLETE, "XIAO_CH_1", 9) 
};
static const struct bt_data ad_ch1[] = { 
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)), 
    BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0x12, 0x18), 
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, 0xC2, 0x03), 
    BT_DATA(BT_DATA_NAME_COMPLETE, "XIAO_CH_2", 9) 
};
static const struct bt_data ad_ch2[] = { 
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)), 
    BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0x12, 0x18), 
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, 0xC2, 0x03), 
    BT_DATA(BT_DATA_NAME_COMPLETE, "XIAO_CH_3", 9) 
};
static const struct bt_data *ad_lists[CH_COUNT] = {ad_ch0, ad_ch1, ad_ch2};
static const size_t ad_sizes[CH_COUNT] = {ARRAY_SIZE(ad_ch0), ARRAY_SIZE(ad_ch1), ARRAY_SIZE(ad_ch2)};

static void start_adv_for_current_channel(void) {
    bt_le_adv_stop(); 

    struct bt_le_adv_param adv_param = {
        .id = channel_ids[current_channel], 
        .options = BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY,
        .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
        .interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
    };
    
    int err = bt_le_adv_start(&adv_param, ad_lists[current_channel], ad_sizes[current_channel], NULL, 0);
    if (!err) printk("\n[ 頻道 %d ] 正在廣播滑鼠訊號... (底層ID: %d)\n", current_channel + 1, channel_ids[current_channel]);
    else printk("\n[ 錯誤 ] 廣播失敗 (err %d)\n", err);
}

// 安全重啟廣播處理器 (修復 err -12)
static void restart_adv_work_handler(struct k_work *work) {
    k_sleep(K_MSEC(200)); 
    start_adv_for_current_channel();
}

static void connected(struct bt_conn *conn, uint8_t err) {
    if (err) return;
    current_conn = bt_conn_ref(conn);
    printk(">>> 已連線至頻道 %d\n", current_channel + 1);
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    if (current_conn == conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }
    printk(">>> 已斷線 (原因: 0x%02x)\n", reason);

    if (is_switching) {
        is_switching = false;
        k_work_submit(&complete_switch_work); 
    } else {
        k_work_submit(&restart_adv_work);
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = { 
    .connected = connected, 
    .disconnected = disconnected,
};

static void auth_pairing_complete(struct bt_conn *conn, bool bonded) {
    printk(">>> 頻道 %d 配對成功並綁定！\n", current_channel + 1);
}

static void auth_pairing_failed(struct bt_conn *conn, enum bt_security_err reason) {
    printk(">>> 頻道 %d 配對失敗 (原因: %d)\n", current_channel + 1, reason);
}
static struct bt_conn_auth_info_cb conn_auth_info_callbacks = { .pairing_complete = auth_pairing_complete, .pairing_failed = auth_pairing_failed };

// 切換頻道邏輯
static void switch_channel_work_handler(struct k_work *work) {
    printk("\n================ 切換頻道 ================\n");
    bt_le_adv_stop(); 
    if (current_conn) {
        is_switching = true;
        bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    } else {
        k_work_submit(&complete_switch_work);
    }
}

static void complete_switch_work_handler(struct k_work *work) {
    current_channel = (current_channel + 1) % CH_COUNT;
    settings_save_one("app/ch", &current_channel, sizeof(current_channel));
    start_adv_for_current_channel();
}

// 物理層核彈抹除
static void factory_reset_work_handler(struct k_work *work) {
    printk("\n================ 執行物理層核彈抹除 ================\n");
    bt_le_adv_stop();
    if (current_conn) {
        bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
    
    printk(">>> 正在啟動物理 Flash 抹除 (繞過檔案系統)...\n");
    const struct device *flash_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));
    if (device_is_ready(flash_dev)) {
        off_t storage_offset = DT_REG_ADDR(DT_NODELABEL(storage_partition));
        size_t storage_size = DT_REG_SIZE(DT_NODELABEL(storage_partition));
        int rc = flash_erase(flash_dev, storage_offset, storage_size);
        if (rc == 0) {
            printk(">>> [成功] 實體磁區已全部填入 0xFF！\n");
        }
    }
    
    printk(">>> 系統將於 1 秒後重新啟動...\n");
    k_sleep(K_MSEC(1000));
    sys_reboot(SYS_REBOOT_COLD); 
}

void switch_button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    int64_t now = k_uptime_get();
    if (now - last_button_time > 1000) { 
        last_button_time = now; 
        k_work_submit(&switch_channel_work); 
    }
}

void clear_button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    int64_t now = k_uptime_get();
    if (now - last_clr_button_time > 1500) { 
        last_clr_button_time = now; 
        k_work_submit(&factory_reset_work); 
    }
}

// =========================================================
//  固定初始化 MAC、MAC+1、MAC+2 (防幽靈自癒機制)
// =========================================================
static void init_bluetooth_identities(void) {
    bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
    size_t count = CONFIG_BT_ID_MAX;
    bt_id_get(addrs, &count);
    
    if (count != 3 || channel_ids[1] == 0xFF || channel_ids[2] == 0xFF) {
        printk("\n>>> [系統自癒] 偵測到 NVS 身分異常 (目前 %d 個)。\n", count);
        
        for (int i = 1; i < CONFIG_BT_ID_MAX; i++) {
            bt_id_delete(i);
        }

        bt_addr_le_t base_addr = addrs[0]; 

        bt_addr_le_t mac1 = base_addr;
        mac1.a.val[0] += 1;
        mac1.type = BT_ADDR_LE_RANDOM;
        mac1.a.val[5] |= 0xC0; 
        int id1 = bt_id_create(&mac1, NULL);
        channel_ids[1] = (id1 >= 0) ? id1 : 1;

        bt_addr_le_t mac2 = base_addr;
        mac2.a.val[0] += 2;
        mac2.type = BT_ADDR_LE_RANDOM;
        mac2.a.val[5] |= 0xC0;
        int id2 = bt_id_create(&mac2, NULL);
        channel_ids[2] = (id2 >= 0) ? id2 : 2;

        settings_save_one("app/ids", channel_ids, sizeof(channel_ids));
        printk("-> 身分重建完成！CH1 ID: %d, CH2 ID: %d, CH3 ID: %d\n", channel_ids[0], channel_ids[1], channel_ids[2]);
    } else {
        printk("-> 從 NVS 成功載入既有身分！CH1 ID: %d, CH2 ID: %d, CH3 ID: %d\n", channel_ids[0], channel_ids[1], channel_ids[2]);
    }
}

int main(void) {
    k_sleep(K_SECONDS(3));
    printk("\n========================================\n");
    printk("XIAO nRF52840 Plus - 完美顯形商用版\n");
    printk("========================================\n");

    if (device_is_ready(button.port)) {
        gpio_pin_configure_dt(&button, GPIO_INPUT);
        gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
        gpio_init_callback(&button_cb_data, switch_button_pressed, BIT(button.pin));
        gpio_add_callback(button.port, &button_cb_data);
    }
    if (device_is_ready(clr_button.port)) {
        gpio_pin_configure_dt(&clr_button, GPIO_INPUT);
        gpio_pin_interrupt_configure_dt(&clr_button, GPIO_INT_EDGE_TO_ACTIVE);
        gpio_init_callback(&clr_button_cb_data, clear_button_pressed, BIT(clr_button.pin));
        gpio_add_callback(clr_button.port, &clr_button_cb_data);
    }

    bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
    bt_enable(NULL);
    
    settings_load(); 
    init_bluetooth_identities();
    
    start_adv_for_current_channel();

    return 0;
}