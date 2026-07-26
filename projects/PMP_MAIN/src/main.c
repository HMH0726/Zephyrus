//PMP_MAIN ORESANJO2026
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/ethernet.h>
#include <string.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/ethernet_mgmt.h>
#include <hal/nrf_ficr.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usb_ch9.h>
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

#define ENABLE_CAPTIVE_PORTAL 0	//Mode Switch (0: 穩定 ETH_MODE / 1: CAPTIVE_PORTAL)
#define CH_COUNT 3

static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_b = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
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


//------------------------------------------------------- BTKeyboard

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

// ------------------------------------------------------ BTKeyboard


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

/* 🛡️ 網頁的 HTTP 標頭 (Header) */
const char *html_header = 
"HTTP/1.1 200 OK\r\n"
"Content-Type: text/html\r\n"
"Connection: close\r\n"
"\r\n";

/* 🛡️ 網頁的本體 (Body)，由 CMake 自動從 index.html 轉換進來 */
const unsigned char html_body[] = {
    #include "index.html.inc"
};

const char *ok_response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
const char *redirect_response = "HTTP/1.1 302 Found\r\nLocation: http://192.168.4.1/\r\nConnection: close\r\n\r\n";

/* ========================================================
 * 微型 DHCP 伺服器
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
                /* 在強制彈窗模式下，將自己設定為 DNS 伺服器 (Option 6) */
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
 * 🕸️ DNS 攔截器 (Captive Portal 核心) 完整保留
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
        
        if (len >= 12) {
            /* 檢查是否為標準的 DNS 查詢 (QR=0, OPCODE=0) */
            if ((buf[2] & 0x80) == 0 && (buf[2] & 0x78) == 0) {
                uint8_t rep[512];
                memcpy(rep, buf, len); /* 複製原始查詢請求 */
                
                /* 修改 DNS 標記為「標準回覆 (QR=1)」且沒有錯誤 */
                rep[2] |= 0x80; 
                /* 設定 Answer Count = 1 */
                rep[6] = 0x00; rep[7] = 0x01; 

                int opt = len;
                /* Name Pointer (指向封包偏移量 12 的查詢名稱) */
                rep[opt++] = 0xC0; rep[opt++] = 0x0C;
                /* Type A (IPv4 位址) */
                rep[opt++] = 0x00; rep[opt++] = 0x01;
                /* Class IN (網際網路) */
                rep[opt++] = 0x00; rep[opt++] = 0x01;
                /* TTL (60 秒) */
                rep[opt++] = 0x00; rep[opt++] = 0x00; rep[opt++] = 0x00; rep[opt++] = 0x3C;
                /* 資料長度 (4 bytes，也就是 IP) */
                rep[opt++] = 0x00; rep[opt++] = 0x04;
                /* 強制綁架導向我們的 IP: 192.168.4.1 */
                rep[opt++] = 192; rep[opt++] = 168; rep[opt++] = 4; rep[opt++] = 1;

                zsock_sendto(sock, rep, opt, 0, (struct sockaddr *)&client_addr, client_len);
            }
        }
    }
}
K_THREAD_DEFINE(dns_thread_id, 2048, captive_portal_dns_thread, NULL, NULL, NULL, 5, 0, 0);
#endif

int main(void) {
    /* =========================================================
     * 1. 系統啟動與基本設定 (來自專案二)
     * ========================================================= */
    k_sleep(K_SECONDS(3));
    printk("\n========================================\n");
    printk("XIAO nRF52840 Plus - 完美顯形商用版\n");
    printk("========================================\n");

    /* =========================================================
     * 2. 初始化按鍵與 GPIO 中斷 (來自專案二)
     * ========================================================= */
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

    /* =========================================================
     * 3. 初始化藍牙服務 (來自專案二)
     * ========================================================= */
    bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
    bt_enable(NULL);
    
    settings_load(); 
    init_bluetooth_identities();
    
    start_adv_for_current_channel();

    /* =========================================================
     * 4. 初始化 LED (來自專案一)
     * ========================================================= */
    if (gpio_is_ready_dt(&led_r)) gpio_pin_configure_dt(&led_r, GPIO_OUTPUT_INACTIVE);
    if (gpio_is_ready_dt(&led_g)) gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);
    if (gpio_is_ready_dt(&led_b)) gpio_pin_configure_dt(&led_b, GPIO_OUTPUT_INACTIVE);

    /* =========================================================
     * 5. 初始化 USB 與虛擬網卡 CDC NCM (來自專案一)
     * ========================================================= */
    int err = usbd_add_descriptor(&sample_usbd, &sample_lang); if (err) debug_halt(1, err);
    err = usbd_add_descriptor(&sample_usbd, &sample_mfr); if (err) debug_halt(1, err);
    err = usbd_add_descriptor(&sample_usbd, &sample_product); if (err) debug_halt(1, err);
    err = usbd_add_descriptor(&sample_usbd, &sample_sn); if (err) debug_halt(1, err);

    err = usbd_add_configuration(&sample_usbd, USBD_SPEED_FS, &sample_fs_config); if (err) debug_halt(3, err);
    err = usbd_register_class(&sample_usbd, "cdc_ncm_0", USBD_SPEED_FS, 1); if (err) debug_halt(4, err);
    err = usbd_msg_register_cb(&sample_usbd, usbd_msg_cb); if (err) debug_halt(5, err);
    
    err = usbd_init(&sample_usbd); if (err) debug_halt(6, err); 
    err = usbd_enable(&sample_usbd); if (err) debug_halt(7, err); 

    gpio_pin_set_dt(&led_b, 1);
    struct net_if *iface = net_if_get_default();
    if (!iface) debug_halt(8, 1);

    /* =========================================================
     * 6. 動態植入出廠 MAC (從 NRF_FICR 讀取全球唯一碼) (來自專案一)
     * ========================================================= */
    uint32_t ficr_deviceaddr0 = nrf_ficr_deviceaddr_get(NRF_FICR, 0);
    uint32_t ficr_deviceaddr1 = nrf_ficr_deviceaddr_get(NRF_FICR, 1);
    
    uint8_t mac_addr[6];
    mac_addr[0] = (ficr_deviceaddr1 >> 8) & 0xFF;
    mac_addr[1] = (ficr_deviceaddr1 >> 0) & 0xFF;
    mac_addr[2] = (ficr_deviceaddr0 >> 24) & 0xFF;
    mac_addr[3] = (ficr_deviceaddr0 >> 16) & 0xFF;
    mac_addr[4] = (ficr_deviceaddr0 >> 8) & 0xFF;
    mac_addr[5] = (ficr_deviceaddr0 >> 0) & 0xFF;

    /* 藍牙/網路 MAC 規範：Static Random 位址的最高兩位元必須為 1 */
    mac_addr[0] |= 0xC0; 
    
    struct ethernet_req_params eth_params;
    memcpy(eth_params.mac_address.addr, mac_addr, 6);
    
    /* 必須先 down 介面才能修改 MAC */
    net_if_down(iface);
    int mac_err = net_mgmt(NET_REQUEST_ETHERNET_SET_MAC_ADDRESS, iface, &eth_params, sizeof(eth_params));
    if (mac_err < 0) debug_halt(10, mac_err); 
    net_if_up(iface);

    /* =========================================================
     * 7. 等待網路啟動與設定固定 IP (來自專案一)
     * ========================================================= */
    /* 等待網卡啟動 */
    while (!net_if_is_up(iface)) { k_sleep(K_MSEC(100)); }
    gpio_pin_set_dt(&led_b, 0);

    /* 設定固定 IP：192.168.4.1 */
    struct in_addr my_addr, my_netmask;
    net_addr_pton(AF_INET, "192.168.4.1", &my_addr);
    if (!net_if_ipv4_addr_add(iface, &my_addr, NET_ADDR_MANUAL, 0)) debug_halt(9, 1);
    net_addr_pton(AF_INET, "255.255.255.0", &my_netmask);
    net_if_ipv4_set_netmask_by_addr(iface, &my_addr, &my_netmask);

    k_sleep(K_MSEC(500));
    gpio_pin_set_dt(&led_g, 1);

    /* =========================================================
     * 8. 啟動 TCP Socket 伺服器 (來自專案一)
     * ========================================================= */
    int serv_sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serv_sock < 0) return -1;
    
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(80);
    zsock_bind(serv_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
    
    zsock_listen(serv_sock, 10);

    /* =========================================================
     * 9. 進入 HTTP Server 無窮迴圈監聽 (來自專案一)
     * ========================================================= */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_sock = zsock_accept(serv_sock, (struct sockaddr *)&client_addr, &client_addr_len);
        
        if (client_sock < 0) {
            k_sleep(K_MSEC(10));
            continue;
        }
        
        gpio_pin_set_dt(&led_b, 1);
        
        /* 🛡️ 無情掛電話機制：0.5秒內不給資料就直接切斷 */
        char rx_buf[1024] = {0};
        int total_len = 0;
        int wait_ms = 500; 
        
        while (total_len < sizeof(rx_buf) - 1 && wait_ms > 0) {
            ssize_t received = zsock_recv(client_sock, rx_buf + total_len, sizeof(rx_buf) - 1 - total_len, ZSOCK_MSG_DONTWAIT);
            
            if (received > 0) {
                total_len += received;
                if (strstr(rx_buf, "\r\n\r\n") != NULL) break; 
                wait_ms = 500; /* 對方有傳資料，重置計時器 */
            } else if (received == 0) {
                break; /* 對方主動關閉連線 */
            } else {
                /* 還沒收到，小睡 50ms 再看一次 */
                k_sleep(K_MSEC(50));
                wait_ms -= 50;
            }
        }
        
        if (total_len > 0) {
            if (strstr(rx_buf, "GET /led/on") != NULL) {
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
            else if (strstr(rx_buf, "GET / ") != NULL || strstr(rx_buf, "GET /index.html") != NULL) {
                /* 1. 先發送 HTTP 標頭 */
                zsock_send(client_sock, html_header, strlen(html_header), 0);
                
                /* 2. 🛡️ 升級：大檔案分塊發送機制 (TCP Chunking) */
                int total_sent = 0;
                int html_size = sizeof(html_body);
                
                while (total_sent < html_size) {
                    /* 每次嘗試把剩下的資料送出去 */
                    ssize_t sent = zsock_send(client_sock, html_body + total_sent, html_size - total_sent, 0);
                    
                    if (sent <= 0) {
                        break; /* 如果網路錯誤或對方斷線，就停止發送 */
                    }
                    total_sent += sent; /* 累加已經成功發送的位元組數量 */
                }
            }
            else {
#if ENABLE_CAPTIVE_PORTAL == 1
                /* 如果有開啟 Captive Portal，就把不認識的網址都導向首頁 */
                zsock_send(client_sock, redirect_response, strlen(redirect_response), 0);
#else
                const char *not_found = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
                zsock_send(client_sock, not_found, strlen(not_found), 0);
#endif
            }
        }
        
        zsock_shutdown(client_sock, ZSOCK_SHUT_WR);
        
        char drain_buf[128];
        while (zsock_recv(client_sock, drain_buf, sizeof(drain_buf), ZSOCK_MSG_DONTWAIT) > 0) {}
        
        k_sleep(K_MSEC(50)); 
        zsock_close(client_sock);
        gpio_pin_set_dt(&led_b, 0);
    }
    
    return 0; /* 正常情況下不會執行到這裡 */
}