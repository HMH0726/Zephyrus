#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/settings/settings.h>
#include <stdio.h>

// ==========================================
// 硬體按鈕綁定 (D1 切換, D2 打字/重置)
// ==========================================
#define SW0_NODE DT_ALIAS(sw0)
#define SW1_NODE DT_ALIAS(sw1)
static const struct gpio_dt_spec btn_switch = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
static const struct gpio_dt_spec btn_type = GPIO_DT_SPEC_GET(SW1_NODE, gpios);

static struct gpio_callback btn_switch_cb_data;
static struct gpio_callback btn_type_cb_data;

// 藍牙狀態變數
static struct bt_conn *current_conn;
static uint8_t current_id = 0; 
static int64_t last_switch_time = 0;
static int64_t last_type_time = 0;
static char dynamic_device_name[30];

// ==========================================
// 1. 手刻 Device Information Service (DIS)
// ==========================================
#define MY_UUID_DIS       BT_UUID_DECLARE_16(0x180A)
#define MY_UUID_PNP_ID    BT_UUID_DECLARE_16(0x2a50)
static const uint8_t pnp_id[] = { 0x02, 0x6D, 0x04, 0x2B, 0xC5, 0x11, 0x01 };

static ssize_t read_pnp_id(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, pnp_id, sizeof(pnp_id));
}
BT_GATT_SERVICE_DEFINE(dis_svc,
    BT_GATT_PRIMARY_SERVICE(MY_UUID_DIS),
    BT_GATT_CHARACTERISTIC(MY_UUID_PNP_ID, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_pnp_id, NULL, NULL)
);

// ==========================================
// 2. 手刻 HID over GATT (HOGP) 鍵盤服務
// ==========================================
#define MY_UUID_HIDS             BT_UUID_DECLARE_16(0x1812)
#define MY_UUID_HIDS_INFO        BT_UUID_DECLARE_16(0x2a4a)
#define MY_UUID_HIDS_REPORT_MAP  BT_UUID_DECLARE_16(0x2a4b)
#define MY_UUID_HIDS_CTRL_POINT  BT_UUID_DECLARE_16(0x2a4c)
#define MY_UUID_HIDS_REPORT      BT_UUID_DECLARE_16(0x2a4d)
#define MY_UUID_REPORT_REF       BT_UUID_DECLARE_16(0x2908)

static const uint8_t hid_info[] = { 0x11, 0x01, 0x00, 0x03 };
static const uint8_t hid_report_desc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 
    0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0
};

static ssize_t read_info(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, hid_info, sizeof(hid_info));
}
static ssize_t read_report_map(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, hid_report_desc, sizeof(hid_report_desc));
}

static uint8_t input_report_data[8] = {0};
static ssize_t read_report(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, input_report_data, sizeof(input_report_data));
}

static void cccd_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    printk("💻 電腦/手機已準備好接收鍵盤訊號 (CCC: %d)\n", value);
}

static uint8_t ctrl_point = 0;
static ssize_t write_ctrl_point(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (len == 1) ctrl_point = *((uint8_t *)buf);
    return len;
}

static const uint8_t report_ref_1[] = { 0x01, 0x01 };
static ssize_t read_report_ref(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, report_ref_1, sizeof(report_ref_1));
}

BT_GATT_SERVICE_DEFINE(hid_svc,
    BT_GATT_PRIMARY_SERVICE(MY_UUID_HIDS),
    BT_GATT_CHARACTERISTIC(MY_UUID_HIDS_INFO, BT_GATT_CHRC_READ, BT_GATT_PERM_READ_ENCRYPT, read_info, NULL, NULL),
    BT_GATT_CHARACTERISTIC(MY_UUID_HIDS_REPORT_MAP, BT_GATT_CHRC_READ, BT_GATT_PERM_READ_ENCRYPT, read_report_map, NULL, NULL),
    BT_GATT_CHARACTERISTIC(MY_UUID_HIDS_CTRL_POINT, BT_GATT_CHRC_WRITE_WITHOUT_RESP, BT_GATT_PERM_WRITE_ENCRYPT, NULL, write_ctrl_point, &ctrl_point),
    BT_GATT_CHARACTERISTIC(MY_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ_ENCRYPT, read_report, NULL, input_report_data),
    BT_GATT_CCC(cccd_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_DESCRIPTOR(MY_UUID_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_report_ref, NULL, NULL)
);

// ==========================================
// 🌟 專家級除錯：SMP 安全配對攔截器 🌟
// ==========================================
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey) {
    printk("🔑 系統請求配對碼: %06u\n", passkey);
}

static void auth_cancel(struct bt_conn *conn) {
    printk("🛑 配對被取消！\n");
}

static void pairing_complete(struct bt_conn *conn, bool bonded) {
    printk("✅ 配對與綁定完成！(Bonded: %d)\n", bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason) {
    printk("💥 配對失敗！Zephyr 底層 SMP 錯誤碼: %d\n", reason);
}

static struct bt_conn_auth_cb auth_cb_display = {
    .passkey_display = auth_passkey_display,
    .cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb auth_info_cb = {
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed,
};

// ==========================================
// 打字、廣播與連線邏輯
// ==========================================
static void send_keystroke_work_handler(struct k_work *work) {
    if (!current_conn) {
        printk("尚未連線，無法送出按鍵！\n");
        return;
    }
    printk("⌨️ 送出按鍵 'A' ...\n");
    input_report_data[2] = 0x04; 
    bt_gatt_notify(current_conn, &hid_svc.attrs[8], input_report_data, sizeof(input_report_data));
    k_sleep(K_MSEC(20));
    input_report_data[2] = 0x00; 
    bt_gatt_notify(current_conn, &hid_svc.attrs[8], input_report_data, sizeof(input_report_data));
}
K_WORK_DEFINE(type_work, send_keystroke_work_handler);

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0x12, 0x18),     
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, 0xC1, 0x03), 
};

static struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, dynamic_device_name, 0),
};

static void start_adv(uint8_t id) {
    snprintf(dynamic_device_name, sizeof(dynamic_device_name), "%s_%d", CONFIG_BT_DEVICE_NAME, id + 1);
    sd[0].data = (const uint8_t *)dynamic_device_name;
    sd[0].data_len = strlen(dynamic_device_name);

    struct bt_le_adv_param adv_param = {
        .options = (BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY), 
        .interval_min = 0x0030, 
        .interval_max = 0x0060, 
        .id = id,               
    };

    if (bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd))) {
        printk("廣播啟動失敗\n");
    } else {
        printk("🚀 頻道 %d (%s) 正在廣播中...\n", id + 1, dynamic_device_name);
    }
}

static void adv_work_handler(struct k_work *work) {
    start_adv(current_id);
}
K_WORK_DEFINE(adv_work, adv_work_handler);

static void switch_profile_work_handler(struct k_work *work) {
    printk("\n=================================\n");
    printk("切換頻道按鈕已觸發！\n");
    current_id = (current_id + 1) % CONFIG_BT_ID_MAX;

    if (current_conn) {
        // 斷開現有連線，這會觸發 disconnected 回呼，在那邊重啟廣播
        bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    } else {
        bt_le_adv_stop();
        k_work_submit(&adv_work);
    }
    printk("=================================\n");
}
K_WORK_DEFINE(switch_work, switch_profile_work_handler);

// ==========================================
// 按鈕中斷觸發器
// ==========================================
void button_switch_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    int64_t now = k_uptime_get();
    if (now - last_switch_time < 500) return; 
    last_switch_time = now;
    k_work_submit(&switch_work);
}

void button_type_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    int64_t now = k_uptime_get();
    if (now - last_type_time < 300) return; 
    last_type_time = now;
    k_work_submit(&type_work);
}

static void connected(struct bt_conn *conn, uint8_t err) {
    if (err) return;
    current_conn = bt_conn_ref(conn);
    printk("✅ 裝置已連線！(頻道 %d)\n", current_id + 1);
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    printk("❌ 裝置已斷線 (原因: %d)\n", reason);
    if (current_conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }
    k_work_submit(&adv_work);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

// ==========================================
// 主程式 Setup
// ==========================================
int main(void) {
    printk("初始化 Zephyr 系統...\n");

    gpio_pin_configure_dt(&btn_switch, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&btn_switch, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(&btn_switch_cb_data, button_switch_pressed, BIT(btn_switch.pin));
    gpio_add_callback(btn_switch.port, &btn_switch_cb_data);

    gpio_pin_configure_dt(&btn_type, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&btn_type, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(&btn_type_cb_data, button_type_pressed, BIT(btn_type.pin));
    gpio_add_callback(btn_type.port, &btn_type_cb_data);

    bt_enable(NULL);

    // 👉 註冊配對攔截器 (放在 bt_enable 之後)
    //bt_conn_auth_cb_register(&auth_cb_display);
    bt_conn_auth_info_cb_register(&auth_info_cb);

    settings_load();

    // 隱藏秘技：開機時按住 D2 鍵，執行「原廠重置」清除所有藍牙記憶
    if (gpio_pin_get_dt(&btn_type) == 1) {
        printk("⚠️ 偵測到重置指令！正在清除 Flash 中的所有配對記憶...\n");
        
        // 👉 修正這裡：用迴圈把每個頻道的記憶都清空
        for (uint8_t id = 0; id < CONFIG_BT_ID_MAX; id++) {
            bt_unpair(id, NULL); 
        }
        
        printk("✅ 記憶已徹底清除！\n");
        k_sleep(K_MSEC(1000)); 
    }

    bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
    size_t id_count = CONFIG_BT_ID_MAX;
    bt_id_get(addrs, &id_count);
    
    while (id_count < CONFIG_BT_ID_MAX) {
        bt_id_create(NULL, NULL);
        id_count++;
    }
    
    k_work_submit(&adv_work);

    while (1) {
        k_sleep(K_FOREVER); 
    }
    return 0;
}