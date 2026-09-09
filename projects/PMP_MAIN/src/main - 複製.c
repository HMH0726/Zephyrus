//PMP_MAIN ORESANJO2026

// 1. 標準 C 函式庫
#include <stdint.h>
#include <stdio.h>
#include <string.h>
// 2. Zephyr 核心與系統設定
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/settings/settings.h>
// 3. 網路 (Networking)
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/ethernet_mgmt.h>
// 4. 藍牙 (Bluetooth)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
// 5. USB
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usb_ch9.h>
// 6. 硬體驅動與 HAL
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/sensor.h>
#include <hal/nrf_ficr.h>

/* =========================================================
 * 【全域設定與硬體定義區塊】
 * ========================================================= */
#define ENABLE_CAPTIVE_PORTAL 0

#define CH_COUNT           3   
#define PROFILE_COUNT      3   
#define KEY_COUNT          9   
#define WHEEL_DIR          2   
#define MAX_CH_NAME_LEN    8   
#define MAX_PROF_NAME_LEN  8   

#define ENCODER_STEPS DT_PROP(DT_NODELABEL(qdec), steps)
#define EDGES_PER_CLICK 4
#define DEGREES_PER_CLICK ((EDGES_PER_CLICK * 360) / ENCODER_STEPS)

static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_b = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_NODELABEL(switch_btn), gpios);
static struct gpio_callback button_cb_data;
static const struct gpio_dt_spec clr_button = GPIO_DT_SPEC_GET(DT_NODELABEL(clear_btn), gpios);
static struct gpio_callback clr_button_cb_data;
static const struct gpio_dt_spec editmode_button = GPIO_DT_SPEC_GET(DT_NODELABEL(editmode_btn), gpios);
static struct gpio_callback editmode_button_cb_data;

char channel_names[CH_COUNT][MAX_CH_NAME_LEN] = { "Win", "Mac", "iPad" };

typedef struct {
    char profile_name[MAX_PROF_NAME_LEN];
    uint16_t keys[KEY_COUNT];        
    uint16_t wheel_v[WHEEL_DIR];
    uint16_t wheel_h[WHEEL_DIR];
} keyboard_profile_t;

keyboard_profile_t key_profiles[CH_COUNT][PROFILE_COUNT] = {
    {
        { "Basic", { 0x004B, 0x004E, 0x002C, 0x004A, 0x004D, 0x0028, 0x0052, 0x0051, 0x0029 }, { 0x0052, 0x0051 }, { 0x004F, 0x0050 } },
        { "Code",  { 0x0104, 0x011B, 0x0106, 0x0119, 0x011D, 0x011C, 0x0109, 0x0116, 0x0128 }, { 0x0152, 0x0151 }, { 0x042B, 0x062B } },
        { "Admin", { 0x0329, 0x0A16, 0x094F, 0x0950, 0x0807, 0x080E, 0x0B05, 0x054C, 0x043D }, { 0x014B, 0x014E }, { 0x0852, 0x0851 } }
    },
    {
        { "Base",  { 0x0806, 0x0819, 0x081B, 0x081D, 0x0A1D, 0x0816, 0x0814, 0x0817, 0x082A }, { 0x0852, 0x0851 }, { 0x084F, 0x0850 } },
        { "PS",    { 0x0005, 0x0008, 0x0019, 0x0010, 0x001A, 0x000C, 0x042A, 0x0807, 0x0A11 }, { 0x0030, 0x002F }, { 0x082E, 0x082D } },
        { "PR",    { 0x0019, 0x0006, 0x0004, 0x0014, 0x001A, 0x0008, 0x002C, 0x0028, 0x0029 }, { 0x004F, 0x0050 }, { 0x044F, 0x0450 } }
    },
    {
        { "Read",  { 0x004B, 0x004E, 0x002C, 0x004A, 0x004D, 0x0028, 0x0052, 0x0051, 0x0029 }, { 0x0051, 0x0052 }, { 0x004F, 0x0050 } },
        { "Num",   { 0x0024, 0x0025, 0x0026, 0x0021, 0x0022, 0x0023, 0x001E, 0x001F, 0x0020 }, { 0x0027, 0x002A }, { 0x002E, 0x002D } },
        { "Func",  { 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, 0x0040, 0x0041, 0x0042 }, { 0x0817, 0x081A }, { 0x012B, 0x032B } }
    }
};

uint8_t current_profile = 0;
static uint8_t channel_ids[CH_COUNT] = {0, 1, 2};
static uint8_t current_channel = 0;             
static struct bt_conn *current_conn = NULL;     
static int64_t last_button_time = 0;            
static int64_t last_clr_button_time = 0;        
static int64_t last_editmode_button_time = 0;   
static bool volatile is_switching = false;      

K_EVENT_DEFINE(edit_mode_event);
volatile bool is_edit_mode = false;

static void switch_channel_work_handler(struct k_work *work);
K_WORK_DEFINE(switch_channel_work, switch_channel_work_handler); 
static void complete_switch_work_handler(struct k_work *work);
K_WORK_DEFINE(complete_switch_work, complete_switch_work_handler); 
static void factory_reset_work_handler(struct k_work *work);
K_WORK_DEFINE(factory_reset_work, factory_reset_work_handler); 
static void restart_adv_work_handler(struct k_work *work);
K_WORK_DEFINE(restart_adv_work, restart_adv_work_handler); 

//------------------------------------------------------- BTKeyboard
static int app_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (strcmp(name, "ch") == 0) { read_cb(cb_arg, &current_channel, sizeof(current_channel)); return 0; }
    if (strcmp(name, "ids") == 0) { read_cb(cb_arg, channel_ids, sizeof(channel_ids)); return 0; }
    if (strcmp(name, "cnames") == 0) { read_cb(cb_arg, channel_names, sizeof(channel_names)); return 0; }
    if (strcmp(name, "profiles") == 0) { read_cb(cb_arg, key_profiles, sizeof(key_profiles)); return 0; }
    return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(app_settings, "app", NULL, app_settings_set, NULL, NULL);

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

static void factory_reset_work_handler(struct k_work *work) {
    printk("\n================ 執行物理層核彈抹除 ================\n");
    bt_le_adv_stop();
    if (current_conn) bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    printk(">>> 正在啟動物理 Flash 抹除 (繞過檔案系統)...\n");
    const struct device *flash_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));
    if (device_is_ready(flash_dev)) {
        off_t storage_offset = DT_REG_ADDR(DT_NODELABEL(storage_partition));
        size_t storage_size = DT_REG_SIZE(DT_NODELABEL(storage_partition));
        int rc = flash_erase(flash_dev, storage_offset, storage_size);
        if (rc == 0) printk(">>> [成功] 實體磁區已全部填入 0xFF！\n");
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

void editmode_button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    int64_t now = k_uptime_get();
    if (now - last_editmode_button_time > 1000) { 
        last_editmode_button_time = now; 
        k_event_post(&edit_mode_event, 0x01);
    }
}

static void init_bluetooth_identities(void) {
    bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
    size_t count = CONFIG_BT_ID_MAX;
    bt_id_get(addrs, &count); 
    
    if (count != 3 || channel_ids[1] == 0xFF || channel_ids[2] == 0xFF) {
        printk("\n>>> [系統自癒] 偵測到 NVS 身分異常 (目前 %d 個)。\n", count);
        for (int i = 1; i < CONFIG_BT_ID_MAX; i++) bt_id_delete(i);
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

// ------------------------------------------------------ BTKeyboard 結束

/* ========================================================
 * 【QDEC 編碼器背景採樣執行緒】
 * ======================================================== */
static void encoder_thread(void *p1, void *p2, void *p3) {
    const struct device *const qdec_dev = DEVICE_DT_GET(DT_NODELABEL(qdec));
    if (!device_is_ready(qdec_dev)) {
        printk("QDEC 設備尚未準備好！\n");
        return;
    }

    struct sensor_value val;
    int accumulated_degrees = 0;

    while (1) {
        sensor_sample_fetch(qdec_dev);
        sensor_channel_get(qdec_dev, SENSOR_CHAN_ROTATION, &val);

        int delta_degrees = val.val1;
        if (delta_degrees != 0) {
            accumulated_degrees += delta_degrees;
            int clicks_moved = accumulated_degrees / DEGREES_PER_CLICK;
            
            if (clicks_moved != 0) {
                /* 💡 後續將在此處加入藍牙 HID 出鍵發送邏輯 */
                accumulated_degrees %= DEGREES_PER_CLICK;
            }
        }
        k_msleep(20);
    }
}
K_THREAD_DEFINE(encoder_thread_id, 2048, encoder_thread, NULL, NULL, NULL, 6, 0, 0);

/* =========================================================
 * 【USB CDC-NCM 虛擬網卡設定】
 * ========================================================= */
USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(sample_fs_config, USB_SCD_SELF_POWERED, 250, &fs_cfg_desc); 
USBD_DESC_LANG_DEFINE(sample_lang); 
USBD_DESC_MANUFACTURER_DEFINE(sample_mfr, "XIAO"); 
USBD_DESC_PRODUCT_DEFINE(sample_product, "XIAO CDC-NCM ZFlip"); 
USBD_DESC_SERIAL_NUMBER_DEFINE(sample_sn); 
USBD_DEVICE_DEFINE(sample_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), 0x2fe3, 0x0090);

static void usbd_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg) {}

void debug_halt(int step, int err_code) {
    if (err_code < 0) err_code = -err_code; 
    int hundreds = err_code / 100;
    int tens = (err_code % 100) / 10;
    int units = err_code % 10;

    gpio_pin_set_dt(&led_r, 0); gpio_pin_set_dt(&led_g, 0); gpio_pin_set_dt(&led_b, 0);

    while (1) {
        for (int i = 0; i < step; i++) {
            gpio_pin_set_dt(&led_r, 1); k_sleep(K_MSEC(300)); gpio_pin_set_dt(&led_r, 0); k_sleep(K_MSEC(300));
        }
        k_sleep(K_MSEC(2000)); 

        for (int i = 0; i < hundreds; i++) {
            gpio_pin_set_dt(&led_r, 1); k_sleep(K_MSEC(400)); gpio_pin_set_dt(&led_r, 0); k_sleep(K_MSEC(400));
        }
        k_sleep(K_MSEC(1000));
        
        if (tens > 0) {
            for (int i = 0; i < tens; i++) {
                gpio_pin_set_dt(&led_g, 1); k_sleep(K_MSEC(400)); gpio_pin_set_dt(&led_g, 0); k_sleep(K_MSEC(400));
            }
        } else if (hundreds > 0) { 
            gpio_pin_set_dt(&led_g, 1); k_sleep(K_MSEC(100)); gpio_pin_set_dt(&led_g, 0); k_sleep(K_MSEC(400)); 
        }
        k_sleep(K_MSEC(1000));

        if (units > 0) {
            for (int i = 0; i < units; i++) {
                gpio_pin_set_dt(&led_b, 1); k_sleep(K_MSEC(400)); gpio_pin_set_dt(&led_b, 0); k_sleep(K_MSEC(400));
            }
        } else { 
            gpio_pin_set_dt(&led_b, 1); k_sleep(K_MSEC(100)); gpio_pin_set_dt(&led_b, 0); k_sleep(K_MSEC(400)); 
        }
        k_sleep(K_MSEC(4000)); 
    }
}

/* =========================================================
 * 【Web Server 資料區塊】
 * ========================================================= */
const char *html_header = 
"HTTP/1.1 200 OK\r\n"
"Content-Type: text/html\r\n"
"Connection: close\r\n"
"\r\n";

const unsigned char html_body[] = {
    #include "index.html.inc"
};

const char *ok_response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
const char *redirect_response = "HTTP/1.1 302 Found\r\nLocation: http://192.168.4.1/\r\nConnection: close\r\n\r\n";

/* ========================================================
 * 【微型 DHCP 伺服器 (極簡穩定版)】
 * ======================================================== */
static void mini_dhcp_thread(void *p1, void *p2, void *p3) {
    int sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); 
    if (sock < 0) return;
    
    int bcast_en = 1;
    zsock_setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast_en, sizeof(bcast_en)); 
    
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(67); 
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (zsock_bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) return;
    
    uint8_t buf[512];
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        ssize_t len = zsock_recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client_addr, &client_len);
        
        if (len < 0) {
            k_sleep(K_MSEC(500));
            continue; 
        }

        if (!is_edit_mode) continue;

        if (len >= 240 && buf[0] == 1) {
            uint8_t msg_type = 0;
            int i = 240;
            while (i < len && buf[i] != 255) {
                if (buf[i] == 53 && i + 2 < len && buf[i+1] >= 1) { msg_type = buf[i+2]; break; }
                if (buf[i] == 0) { i++; } else { if (i + 1 >= len) break; i += 2 + buf[i+1]; }
            }
            
            if (msg_type == 1 || msg_type == 3) {
                uint8_t rep[300];
                memset(rep, 0, sizeof(rep));
                rep[0] = 2; rep[1] = 1; rep[2] = 6; 
                memcpy(&rep[4], &buf[4], 4); 
                
                rep[16] = 192; rep[17] = 168; rep[18] = 4; rep[19] = 2; 
                rep[20] = 192; rep[21] = 168; rep[22] = 4; rep[23] = 1; 
                memcpy(&rep[28], &buf[28], 16); 
                rep[236] = 0x63; rep[237] = 0x82; rep[238] = 0x53; rep[239] = 0x63; 
                
                int opt = 240;
                rep[opt++] = 53; rep[opt++] = 1; rep[opt++] = (msg_type == 1) ? 2 : 5; 
                rep[opt++] = 54; rep[opt++] = 4; rep[opt++] = 192; rep[opt++] = 168; rep[opt++] = 4; rep[opt++] = 1; 
                rep[opt++] = 1;  rep[opt++] = 4; rep[opt++] = 255; rep[opt++] = 255; rep[opt++] = 255; rep[opt++] = 0; 
                
#if ENABLE_CAPTIVE_PORTAL == 1
                rep[opt++] = 3;  rep[opt++] = 4; rep[opt++] = 192; rep[opt++] = 168; rep[opt++] = 4; rep[opt++] = 1; 
                rep[opt++] = 6;  rep[opt++] = 4; rep[opt++] = 192; rep[opt++] = 168; rep[opt++] = 4; rep[opt++] = 1; 
#endif
                rep[opt++] = 51; rep[opt++] = 4; rep[opt++] = 0x00; rep[opt++] = 0x01; rep[opt++] = 0x51; rep[opt++] = 0x80; 
                
                const char *net_name = "XIAO-Panel";
                int name_len = strlen(net_name);
                rep[opt++] = 15; rep[opt++] = name_len;
                memcpy(&rep[opt], net_name, name_len);
                opt += name_len;
                
                rep[opt++] = 255; 

                struct sockaddr_in bcast_addr;
                memset(&bcast_addr, 0, sizeof(bcast_addr));
                bcast_addr.sin_family = AF_INET;
                bcast_addr.sin_port = htons(68);
                bcast_addr.sin_addr.s_addr = htonl(0xFFFFFFFF); 

                zsock_sendto(sock, rep, opt, 0, (struct sockaddr *)&bcast_addr, sizeof(bcast_addr));
            }
        }
    }
}
K_THREAD_DEFINE(dhcp_thread_id, 2048, mini_dhcp_thread, NULL, NULL, NULL, 5, 0, 0);

/* ========================================================
 * 【🕸️ DNS 攔截器執行緒 (極簡穩定版)】
 * ======================================================== */
#if ENABLE_CAPTIVE_PORTAL == 1
static void captive_portal_dns_thread(void *p1, void *p2, void *p3) {
    int sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(53); 
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (zsock_bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) return;

    uint8_t buf[512];
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        ssize_t len = zsock_recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client_addr, &client_len);
        
        if (len < 0) {
            k_sleep(K_MSEC(500));
            continue;
        }

        if (!is_edit_mode) continue;

        if (len >= 12) {
            if ((buf[2] & 0x80) == 0 && (buf[2] & 0x78) == 0) {
                uint8_t rep[512];
                memcpy(rep, buf, len); 
                
                rep[2] |= 0x80; 
                rep[6] = 0x00; rep[7] = 0x01; 

                int opt = len;
                rep[opt++] = 0xC0; rep[opt++] = 0x0C;
                rep[opt++] = 0x00; rep[opt++] = 0x01;
                rep[opt++] = 0x00; rep[opt++] = 0x01;
                rep[opt++] = 0x00; rep[opt++] = 0x00; rep[opt++] = 0x00; rep[opt++] = 0x3C;
                rep[opt++] = 0x00; rep[opt++] = 0x04;
                rep[opt++] = 192; rep[opt++] = 168; rep[opt++] = 4; rep[opt++] = 1;

                zsock_sendto(sock, rep, opt, 0, (struct sockaddr *)&client_addr, client_len);
            }
        }
    }
}
K_THREAD_DEFINE(dns_thread_id, 2048, captive_portal_dns_thread, NULL, NULL, NULL, 5, 0, 0);
#endif

/* =========================================================
 * 【模組化初始化函式群】
 * ========================================================= */
static void init_leds(void) {
    if (gpio_is_ready_dt(&led_r)) gpio_pin_configure_dt(&led_r, GPIO_OUTPUT_INACTIVE);
    if (gpio_is_ready_dt(&led_g)) gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);
    if (gpio_is_ready_dt(&led_b)) gpio_pin_configure_dt(&led_b, GPIO_OUTPUT_INACTIVE);
}

static void init_buttons(void) {
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
    if (device_is_ready(editmode_button.port)) {
        gpio_pin_configure_dt(&editmode_button, GPIO_INPUT);
        gpio_pin_interrupt_configure_dt(&editmode_button, GPIO_INT_EDGE_TO_ACTIVE);
        gpio_init_callback(&editmode_button_cb_data, editmode_button_pressed, BIT(editmode_button.pin));
        gpio_add_callback(editmode_button.port, &editmode_button_cb_data);
    }
}

static void init_usb_cdc_ncm(void) {
    int err = usbd_add_descriptor(&sample_usbd, &sample_lang); if (err) debug_halt(1, err);
    err = usbd_add_descriptor(&sample_usbd, &sample_mfr); if (err) debug_halt(1, err);
    err = usbd_add_descriptor(&sample_usbd, &sample_product); if (err) debug_halt(1, err);
    err = usbd_add_descriptor(&sample_usbd, &sample_sn); if (err) debug_halt(1, err);

    err = usbd_add_configuration(&sample_usbd, USBD_SPEED_FS, &sample_fs_config); if (err) debug_halt(3, err);
    err = usbd_register_class(&sample_usbd, "cdc_ncm_0", USBD_SPEED_FS, 1); if (err) debug_halt(4, err);
    err = usbd_msg_register_cb(&sample_usbd, usbd_msg_cb); if (err) debug_halt(5, err);
    
    err = usbd_init(&sample_usbd); if (err) debug_halt(6, err); 
}

static struct net_if *init_network_interface(void) {
    struct net_if *iface = net_if_get_default();
    if (!iface) debug_halt(8, 1);

    uint32_t ficr_deviceaddr0 = nrf_ficr_deviceaddr_get(NRF_FICR, 0);
    uint32_t ficr_deviceaddr1 = nrf_ficr_deviceaddr_get(NRF_FICR, 1);
    
    uint8_t mac_addr[6];
    mac_addr[0] = (ficr_deviceaddr1 >> 8) & 0xFF;
    mac_addr[1] = (ficr_deviceaddr1 >> 0) & 0xFF;
    mac_addr[2] = (ficr_deviceaddr0 >> 24) & 0xFF;
    mac_addr[3] = (ficr_deviceaddr0 >> 16) & 0xFF;
    mac_addr[4] = (ficr_deviceaddr0 >> 8) & 0xFF;
    mac_addr[5] = (ficr_deviceaddr0 >> 0) & 0xFF;

    mac_addr[0] |= 0xC0; 
    
    struct ethernet_req_params eth_params;
    memcpy(eth_params.mac_address.addr, mac_addr, 6);
    
    net_if_down(iface);
    int mac_err = net_mgmt(NET_REQUEST_ETHERNET_SET_MAC_ADDRESS, iface, &eth_params, sizeof(eth_params));
    if (mac_err < 0) debug_halt(10, mac_err); 

    struct in_addr my_addr, my_netmask;
    net_addr_pton(AF_INET, "192.168.4.1", &my_addr);
    if (!net_if_ipv4_addr_add(iface, &my_addr, NET_ADDR_MANUAL, 0)) debug_halt(9, 1);
    net_addr_pton(AF_INET, "255.255.255.0", &my_netmask);
    net_if_ipv4_set_netmask_by_addr(iface, &my_addr, &my_netmask);

    return iface;
}

static int init_tcp_server(void) {
    int serv_sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serv_sock < 0) return -1;
    
    int reuse = 1;
    zsock_setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(80);
    zsock_bind(serv_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
    
    zsock_listen(serv_sock, 10);

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    zsock_setsockopt(serv_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return serv_sock;
}

/* =========================================================
 * 【主程式 Main 進入點】
 * ========================================================= */
int main(void) {
    k_sleep(K_SECONDS(3));
    printk("\n========================================\n");
    printk("XIAO nRF52840 Plus - 完美顯形商用版\n");
    printk("========================================\n");

    init_leds();
    init_buttons();

    bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
    bt_enable(NULL);
    settings_load(); 
    init_bluetooth_identities();
    start_adv_for_current_channel();

    init_usb_cdc_ncm();
    struct net_if *iface = init_network_interface();
    if (!iface) debug_halt(8, 1);
    
    int serv_sock = init_tcp_server();
    if (serv_sock < 0) return -1;

    while (1) {
        /* ----- 進入【純藍牙工作模式】 ----- */
        is_edit_mode = false; 

        gpio_pin_set_dt(&led_b, 0);
        gpio_pin_set_dt(&led_g, 0);

        printk("\n>>> 系統目前處於 [純藍牙工作模式]\n");
        printk(">>> 若需修改設定，請按下實體 D7 鍵以觸發 [編輯模式]...\n\n");

        k_event_wait(&edit_mode_event, 0x01, false, K_FOREVER);
        k_event_set(&edit_mode_event, 0x00); 

        /* ----- 進入【編輯模式】 ----- */
        printk("========================================\n");
        printk(">>> 🚀 [編輯模式] 已觸發！正在啟動 USB 網卡與網路伺服器...\n");
        printk("========================================\n");

        usbd_enable(&sample_usbd);
        net_if_up(iface);

        gpio_pin_set_dt(&led_b, 1);
        while (!net_if_is_up(iface)) { k_sleep(K_MSEC(100)); } 
        gpio_pin_set_dt(&led_b, 0);

        is_edit_mode = true; 
        gpio_pin_set_dt(&led_g, 1); 

        printk(">>> 網頁伺服器已就緒！請連接電腦並瀏覽 http://192.168.4.1\n");

        bool exit_requested = false;
        
        while (!exit_requested) {
            struct sockaddr_in client_addr;
            socklen_t client_addr_len = sizeof(client_addr);
            
            int client_sock = zsock_accept(serv_sock, (struct sockaddr *)&client_addr, &client_addr_len);
            
            if (k_event_test(&edit_mode_event, 0x01)) {
                k_event_set(&edit_mode_event, 0x00); 
                exit_requested = true;
                if (client_sock >= 0) zsock_close(client_sock);
                continue; 
            }
            
            if (client_sock < 0) {
                continue; 
            }
            
            gpio_pin_set_dt(&led_b, 1);
            
            char rx_buf[1024] = {0};
            int total_len = 0;
            int wait_ms = 500; 
            
            while (total_len < sizeof(rx_buf) - 1 && wait_ms > 0) {
                ssize_t received = zsock_recv(client_sock, rx_buf + total_len, sizeof(rx_buf) - 1 - total_len, ZSOCK_MSG_DONTWAIT);
                
                if (received > 0) {
                    total_len += received;
                    if (strstr(rx_buf, "\r\n\r\n") != NULL) break; 
                    wait_ms = 500; 
                } else if (received == 0) {
                    break; 
                } else {
                    k_sleep(K_MSEC(10));
                    wait_ms -= 10;
                }
            }
            
			if (total_len > 0) {
							if (strstr(rx_buf, "GET /exit") != NULL) {
								zsock_send(client_sock, ok_response, strlen(ok_response), 0);
								exit_requested = true; 
							}
							else if (strstr(rx_buf, "GET /led/on") != NULL) {
								gpio_pin_set_dt(&led_g, 1);
								zsock_send(client_sock, ok_response, strlen(ok_response), 0);
							} 
							else if (strstr(rx_buf, "GET /led/off") != NULL) {
								gpio_pin_set_dt(&led_g, 0);
								zsock_send(client_sock, ok_response, strlen(ok_response), 0);
							} 
							else if (strstr(rx_buf, "GET /favicon.ico") != NULL) {
								const char *not_found = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
								zsock_send(client_sock, not_found, strlen(not_found), 0);
							}
							else if (strstr(rx_buf, "GET /api/channels") != NULL) {
								char json_resp[256];
								snprintf(json_resp, sizeof(json_resp),
										 "HTTP/1.1 200 OK\r\n"
										 "Content-Type: application/json\r\n"
										 "Connection: close\r\n\r\n"
										 "[\"%s\",\"%s\",\"%s\"]",
										 channel_names[0], channel_names[1], channel_names[2]);
								zsock_send(client_sock, json_resp, strlen(json_resp), 0);
							}
							else if (strncmp(rx_buf, "GET /api/profiles?ch=", 21) == 0) {
								int ch = rx_buf[21] - '0';
								char json_resp[256];
								if (ch >= 0 && ch < CH_COUNT) {
									snprintf(json_resp, sizeof(json_resp),
											 "HTTP/1.1 200 OK\r\n"
											 "Content-Type: application/json\r\n"
											 "Connection: close\r\n\r\n"
											 "[\"%s\",\"%s\",\"%s\"]",
											 key_profiles[ch][0].profile_name,
											 key_profiles[ch][1].profile_name,
											 key_profiles[ch][2].profile_name);
								} else {
									snprintf(json_resp, sizeof(json_resp),
											 "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n[]");
								}
								zsock_send(client_sock, json_resp, strlen(json_resp), 0);
							}
                            else if (strncmp(rx_buf, "GET /api/rename?", 16) == 0) {
                                char *ch_ptr = strstr(rx_buf, "ch=");
                                char *pf_ptr = strstr(rx_buf, "pf=");
                                char *cname_ptr = strstr(rx_buf, "cname=");
                                char *pname_ptr = strstr(rx_buf, "pname=");
                                
                                if (ch_ptr && pf_ptr) {
                                    int ch = ch_ptr[3] - '0';
                                    int pf = pf_ptr[3] - '0';
                                    
                                    if (ch >= 0 && ch < CH_COUNT && pf >= 0 && pf < PROFILE_COUNT) {
                                        if (cname_ptr) {
                                            char *src = cname_ptr + 6;
                                            char new_cname[MAX_CH_NAME_LEN] = {0};
                                            int i = 0, j = 0;
                                            while (src[i] != ' ' && src[i] != '&' && j < MAX_CH_NAME_LEN - 1) {
                                                if (src[i] == '%' && src[i+1] && src[i+2]) {
                                                    int val; sscanf(&src[i+1], "%2x", &val);
                                                    new_cname[j++] = (char)val; i += 3;
                                                } else if (src[i] == '+') {
                                                    new_cname[j++] = ' '; i++;
                                                } else {
                                                    new_cname[j++] = src[i++];
                                                }
                                            }
                                            new_cname[j] = '\0';
                                            strncpy(channel_names[ch], new_cname, MAX_CH_NAME_LEN);
                                            channel_names[ch][MAX_CH_NAME_LEN - 1] = '\0';
                                        }

                                        if (pname_ptr) {
                                            char *src = pname_ptr + 6;
                                            char new_pname[MAX_PROF_NAME_LEN] = {0};
                                            int i = 0, j = 0;
                                            while (src[i] != ' ' && src[i] != '&' && j < MAX_PROF_NAME_LEN - 1) {
                                                if (src[i] == '%' && src[i+1] && src[i+2]) {
                                                    int val; sscanf(&src[i+1], "%2x", &val);
                                                    new_pname[j++] = (char)val; i += 3;
                                                } else if (src[i] == '+') {
                                                    new_pname[j++] = ' '; i++;
                                                } else {
                                                    new_pname[j++] = src[i++];
                                                }
                                            }
                                            new_pname[j] = '\0';
                                            strncpy(key_profiles[ch][pf].profile_name, new_pname, MAX_PROF_NAME_LEN);
                                            key_profiles[ch][pf].profile_name[MAX_PROF_NAME_LEN - 1] = '\0';
                                        }

                                        settings_save_one("app/cnames", channel_names, sizeof(channel_names));
                                        settings_save_one("app/profiles", key_profiles, sizeof(key_profiles));
                                    }
                                }
                                zsock_send(client_sock, ok_response, strlen(ok_response), 0);
                            }
                            else if (strncmp(rx_buf, "GET /api/keys?", 14) == 0) {
                                char *ch_ptr = strstr(rx_buf, "ch=");
                                char *pf_ptr = strstr(rx_buf, "pf=");
                                char json_resp[384];

                                if (ch_ptr && pf_ptr) {
                                    int ch = ch_ptr[3] - '0';
                                    int pf = pf_ptr[3] - '0';

                                    if (ch >= 0 && ch < CH_COUNT && pf >= 0 && pf < PROFILE_COUNT) {
                                        snprintf(json_resp, sizeof(json_resp),
                                                 "HTTP/1.1 200 OK\r\n"
                                                 "Content-Type: application/json\r\n"
                                                 "Connection: close\r\n\r\n"
                                                 "{\"1\":%u,\"2\":%u,\"3\":%u,\"4\":%u,\"5\":%u,\"6\":%u,\"7\":%u,\"8\":%u,\"9\":%u,"
                                                 "\"A_FWD\":%u,\"A_REV\":%u,\"B_R\":%u,\"B_L\":%u}",
                                                 key_profiles[ch][pf].keys[0], key_profiles[ch][pf].keys[1],
                                                 key_profiles[ch][pf].keys[2], key_profiles[ch][pf].keys[3],
                                                 key_profiles[ch][pf].keys[4], key_profiles[ch][pf].keys[5],
                                                 key_profiles[ch][pf].keys[6], key_profiles[ch][pf].keys[7],
                                                 key_profiles[ch][pf].keys[8],
                                                 key_profiles[ch][pf].wheel_v[0], key_profiles[ch][pf].wheel_v[1],
                                                 key_profiles[ch][pf].wheel_h[0], key_profiles[ch][pf].wheel_h[1]);
                                    } else {
                                        snprintf(json_resp, sizeof(json_resp), "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n{}");
                                    }
                                } else {
                                    snprintf(json_resp, sizeof(json_resp), "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n{}");
                                }
                                zsock_send(client_sock, json_resp, strlen(json_resp), 0);
                            }
                            else if (strncmp(rx_buf, "GET /api/setkey?", 16) == 0) {
                                char *ch_ptr = strstr(rx_buf, "ch=");
                                char *pf_ptr = strstr(rx_buf, "pf=");
                                char *id_ptr = strstr(rx_buf, "id=");
                                char *code_ptr = strstr(rx_buf, "code=");

                                if (ch_ptr && pf_ptr && id_ptr && code_ptr) {
                                    int ch = ch_ptr[3] - '0';
                                    int pf = pf_ptr[3] - '0';
                                    uint16_t code = (uint16_t)strtoul(code_ptr + 5, NULL, 10);

                                    char id_str[8] = {0};
                                    int idx = 0;
                                    char *src = id_ptr + 3;
                                    while (src[idx] != '&' && src[idx] != ' ' && idx < sizeof(id_str) - 1) {
                                        id_str[idx] = src[idx];
                                        idx++;
                                    }
                                    id_str[idx] = '\0';

                                    if (ch >= 0 && ch < CH_COUNT && pf >= 0 && pf < PROFILE_COUNT) {
                                        if (id_str[0] >= '1' && id_str[0] <= '9' && id_str[1] == '\0') {
                                            key_profiles[ch][pf].keys[id_str[0] - '1'] = code;
                                        } else if (strcmp(id_str, "A_FWD") == 0) {
                                            key_profiles[ch][pf].wheel_v[0] = code;
                                        } else if (strcmp(id_str, "A_REV") == 0) {
                                            key_profiles[ch][pf].wheel_v[1] = code;
                                        } else if (strcmp(id_str, "B_R") == 0) {
                                            key_profiles[ch][pf].wheel_h[0] = code;
                                        } else if (strcmp(id_str, "B_L") == 0) {
                                            key_profiles[ch][pf].wheel_h[1] = code;
                                        }

                                        settings_save_one("app/profiles", key_profiles, sizeof(key_profiles));
                                    }
                                }
                                zsock_send(client_sock, ok_response, strlen(ok_response), 0);
                            }
							else if (strstr(rx_buf, "GET / ") != NULL || strstr(rx_buf, "GET /index.html") != NULL) {
								zsock_send(client_sock, html_header, strlen(html_header), 0);
								
								int total_sent = 0;
								int html_size = sizeof(html_body);
								
								while (total_sent < html_size) {
									ssize_t sent = zsock_send(client_sock, html_body + total_sent, html_size - total_sent, 0);
									if (sent <= 0) break;
									total_sent += sent;
								}
							}
							else {
			#if ENABLE_CAPTIVE_PORTAL == 1
								zsock_send(client_sock, redirect_response, strlen(redirect_response), 0);
			#else
								const char *not_found = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
								zsock_send(client_sock, not_found, strlen(not_found), 0);
			#endif
							}
						}
            
            k_sleep(K_MSEC(30)); 
            zsock_close(client_sock);
            gpio_pin_set_dt(&led_b, 0);
        }
        
        printk("\n>>> 收到 EXIT 請求，系統即將重新啟動以安全退出編輯模式...\n");
        k_sleep(K_MSEC(1000)); 
		usbd_disable(&sample_usbd); 
        k_sleep(K_MSEC(100));
        sys_reboot(SYS_REBOOT_WARM);
    }
    
    return 0;
}