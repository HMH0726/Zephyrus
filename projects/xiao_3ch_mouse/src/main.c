/*
 * XIAO nRF52840 - 3 Channel BLE Mouse (MX Master style profile switching)
 * ------------------------------------------------------------------
 * 核心概念：
 *   - 用 Zephyr 的 multiple Bluetooth identity (bt_id_create) 建立 3 組
 *     完全獨立的藍牙位址 + bonding table，讓手機把它們看成 3 台不同裝置。
 *   - 3 組位址都衍生自晶片出廠燒錄的 factory static address (FICR)，
 *     只把「最後一個 byte」(bt_addr_to_str 印出來的最後兩碼) 分別 +0 / +1 / +2。
 *   - 按下實體按鈕就切換到下一個頻道：停止目前廣播 -> 斷線 -> 用新
 *     identity 重新廣播，讓手機對應的那組配對重新連上。
 *
 * 注意事項（請詳閱）：
 *   1. 這是「BLE 頻道切換機制」的完整骨架，已用標準 Zephyr Host API 撰寫，
 *      邏輯是可放心參考、依循 ZMK 相同做法的。
 *   2. HID 滑鼠 report 的部分（bt_hids_init 的欄位、bt_hids_inp_rep_send
 *      等）因為 Nordic 的 bt_hids library 在不同 NCS / Zephyr 版本間
 *      API 略有出入，這裡給的是「可運作的骨架」，正式編譯前請對照你
 *      本機 SDK 版本裡的
 *          zephyr/include/zephyr/bluetooth/services/hids.h
 *      以及官方 samples/bluetooth/peripheral_hids 範例，確認欄位名稱一致。
 *      我沒有在你的目標環境實際編譯過這份程式碼，如果編譯有落差，
 *      把錯誤訊息貼給我，我可以馬上幫你對應修正。
 *   3. 真正的滑鼠移動資料（來自光學感測器等）不在此範圍內，
 *      請呼叫檔案最下面的 mouse_send_report() 自行餵資料。
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/services/hids.h>

#include <nrf.h>   /* NRF_FICR 暫存器存取 (nRF52840 出廠位址) */

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define NUM_CHANNELS 3

/* ------------------------------------------------------------------ */
/* HID 滑鼠 report descriptor：3 個按鍵 + X/Y + 滾輪                    */
/* ------------------------------------------------------------------ */
static const uint8_t mouse_report_desc[] = {
	0x05, 0x01,       /* Usage Page (Generic Desktop) */
	0x09, 0x02,       /* Usage (Mouse) */
	0xA1, 0x01,       /* Collection (Application) */
	0x09, 0x01,       /*   Usage (Pointer) */
	0xA1, 0x00,       /*   Collection (Physical) */
	0x05, 0x09,       /*     Usage Page (Buttons) */
	0x19, 0x01,       /*     Usage Minimum (1) */
	0x29, 0x03,       /*     Usage Maximum (3) */
	0x15, 0x00,       /*     Logical Minimum (0) */
	0x25, 0x01,       /*     Logical Maximum (1) */
	0x95, 0x03,       /*     Report Count (3) */
	0x75, 0x01,       /*     Report Size (1) */
	0x81, 0x02,       /*     Input (Data,Var,Abs) */
	0x95, 0x01,       /*     Report Count (1) - padding */
	0x75, 0x05,       /*     Report Size (5) */
	0x81, 0x03,       /*     Input (Const,Var,Abs) */
	0x05, 0x01,       /*     Usage Page (Generic Desktop) */
	0x09, 0x30,       /*     Usage (X) */
	0x09, 0x31,       /*     Usage (Y) */
	0x09, 0x38,       /*     Usage (Wheel) */
	0x15, 0x81,       /*     Logical Minimum (-127) */
	0x25, 0x7F,       /*     Logical Maximum (127) */
	0x75, 0x08,       /*     Report Size (8) */
	0x95, 0x03,       /*     Report Count (3) */
	0x81, 0x06,       /*     Input (Data,Var,Rel) */
	0xC0,             /*   End Collection */
	0xC0,             /* End Collection */
};

BT_HIDS_DEF(hids_obj, 4 /* buttons(1) + X(1) + Y(1) + wheel(1) */);

static uint8_t current_channel;
static struct bt_conn *current_conn;
static bt_addr_le_t channel_addr[NUM_CHANNELS];

static const struct gpio_dt_spec sw = GPIO_DT_SPEC_GET(DT_ALIAS(ch_switch), gpios);
static struct gpio_callback sw_cb;

static void start_advertising_on(uint8_t ch);

/* ------------------------------------------------------------------ */
/* 從出廠 factory address 衍生 3 組頻道位址                              */
/* 規則：最後一個 byte（bt_addr_to_str 印出來的最後兩碼）分別 +0/+1/+2    */
/* ------------------------------------------------------------------ */
static void build_channel_addresses(void)
{
	bt_addr_le_t base = { 0 };

	base.type = BT_ADDR_LE_RANDOM;

	base.a.val[0] = (NRF_FICR->DEVICEADDR[0] >> 0) & 0xFF;
	base.a.val[1] = (NRF_FICR->DEVICEADDR[0] >> 8) & 0xFF;
	base.a.val[2] = (NRF_FICR->DEVICEADDR[0] >> 16) & 0xFF;
	base.a.val[3] = (NRF_FICR->DEVICEADDR[0] >> 24) & 0xFF;
	base.a.val[4] = (NRF_FICR->DEVICEADDR[1] >> 0) & 0xFF;
	base.a.val[5] = (NRF_FICR->DEVICEADDR[1] >> 8) & 0xFF;

	/* static random address 規定最高兩個 bit 必須是 11 */
	base.a.val[5] |= 0xC0;

	for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
		channel_addr[ch] = base;
		/* val[0] 是位址的 LSB，也就是印出字串時最後兩碼 */
		channel_addr[ch].a.val[0] = (uint8_t)(base.a.val[0] + ch);

		char s[BT_ADDR_LE_STR_LEN];

		bt_addr_le_to_str(&channel_addr[ch], s, sizeof(s));
		LOG_INF("Channel %d address: %s", ch, s);
	}
}

/* ------------------------------------------------------------------ */
/* 建立 (僅第一次執行時) 3 組 identity                                  */
/* ------------------------------------------------------------------ */
static void setup_identities(void)
{
	bt_addr_le_t existing[NUM_CHANNELS];
	size_t count = NUM_CHANNELS;

	bt_id_get(existing, &count);

	if (count >= NUM_CHANNELS) {
		LOG_INF("Identities already exist (%d), skip creation", (int)count);
		return;
	}

	for (uint8_t ch = count; ch < NUM_CHANNELS; ch++) {
		int id = bt_id_create(&channel_addr[ch], NULL);

		if (id < 0) {
			LOG_ERR("bt_id_create failed for channel %d (err %d)", ch, id);
		} else {
			LOG_INF("Created identity %d for channel %d", id, ch);
		}
	}
}

/* ------------------------------------------------------------------ */
/* 廣播                                                                */
/* ------------------------------------------------------------------ */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void start_advertising_on(uint8_t ch)
{
	struct bt_le_adv_param param = *BT_LE_ADV_CONN;

	param.id = ch;

	int err = bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), NULL, 0);

	if (err) {
		LOG_ERR("Advertising failed to start on channel %d (err %d)", ch, err);
	} else {
		LOG_INF("Advertising on channel %d", ch);
	}
}

static void switch_channel(uint8_t new_ch)
{
	if (new_ch == current_channel && current_conn) {
		return;
	}

	LOG_INF("Switching channel %d -> %d", current_channel, new_ch);

	bt_le_adv_stop();

	if (current_conn) {
		bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		/* 實際的 conn 釋放在 disconnected() callback 裡處理 */
	}

	current_channel = new_ch;

	if (!current_conn) {
		start_advertising_on(current_channel);
	}
	/* 如果目前還有連線，等 disconnected() 觸發後才會重新廣播 */
}

static void sw_pressed(const struct device *dev, struct gpio_callback *cb,
			uint32_t pins)
{
	uint8_t next = (current_channel + 1) % NUM_CHANNELS;

	switch_channel(next);
}

/* ------------------------------------------------------------------ */
/* 連線 callback                                                       */
/* ------------------------------------------------------------------ */
static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("Connection failed on channel %d (err %d)", current_channel, err);
		start_advertising_on(current_channel);
		return;
	}

	LOG_INF("Connected on channel %d", current_channel);
	current_conn = bt_conn_ref(conn);

	/* 滑鼠務必要求加密連線 */
	bt_conn_set_security(conn, BT_SECURITY_L2);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected (reason 0x%02x)", reason);

	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	/* 斷線後，回到「目前選定頻道」重新廣播 */
	start_advertising_on(current_channel);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

/* ------------------------------------------------------------------ */
/* HIDS 初始化                                                         */
/* ⚠ 請對照你本機 SDK 的 hids.h 與 peripheral_hids 範例確認欄位名稱      */
/* ------------------------------------------------------------------ */
static void hids_init(void)
{
	struct bt_hids_init_param init = { 0 };
	struct bt_hids_inp_rep *inp_rep;

	init.rep_map.data = mouse_report_desc;
	init.rep_map.size = sizeof(mouse_report_desc);

	init.info.bcd_hid = 0x0111;
	init.info.b_country_code = 0x00;
	init.info.flags = BT_HIDS_REMOTE_WAKE | BT_HIDS_NORMALLY_CONNECTABLE;

	inp_rep = &init.inp_rep_group_init.reports[0];
	inp_rep->size = 4; /* buttons + X + Y + wheel */
	inp_rep->id = 1;
	init.inp_rep_group_init.cnt++;

	bt_hids_init(&hids_obj, &init);
}

/* 給你自己的感測器/輸入程式呼叫，送出一筆滑鼠 report */
void mouse_send_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
	uint8_t report[4] = { buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel };

	if (current_conn) {
		bt_hids_inp_rep_send(&hids_obj, current_conn, report, sizeof(report), NULL);
	}
}

/* ------------------------------------------------------------------ */
int main(void)
{
	int err;

	build_channel_addresses();

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return 0;
	}

	settings_load();

	setup_identities();
	hids_init();

	if (gpio_is_ready_dt(&sw)) {
		gpio_pin_configure_dt(&sw, GPIO_INPUT);
		gpio_pin_interrupt_configure_dt(&sw, GPIO_INT_EDGE_TO_ACTIVE);
		gpio_init_callback(&sw_cb, sw_pressed, BIT(sw.pin));
		gpio_add_callback(sw.port, &sw_cb);
	} else {
		LOG_WRN("Switch GPIO not ready - 請檢查 app.overlay 的腳位設定");
	}

	current_channel = 0;
	start_advertising_on(current_channel);

	return 0;
}
