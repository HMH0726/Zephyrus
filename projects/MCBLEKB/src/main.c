/*
 * Multi-Channel BLE Keyboard - XIAO nRF52840 (Plus) + Zephyr
 *
 * Each "channel" is backed by its own Bluetooth local identity (bt_id).
 * Because bonding/key storage in Zephyr's host stack is partitioned per
 * local identity, the SAME remote device (same IRK) can be paired to
 * MULTIPLE channels at once with no key-database collision -- each
 * channel keeps its own independent bond record for that phone.
 *
 * Button (default D2): press to advance to the next channel.
 *   - If currently connected, we disconnect first, then start
 *     advertising the next channel's identity once the disconnect
 *     callback confirms the link is actually closed.
 *   - If not connected, we switch immediately.
 *
 * LED (default D3): on solid while connected, off while advertising.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "hid_keyboard.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define NUM_CHANNELS 3
#define DEBOUNCE_MS  40
#define FACTORY_RESET_HOLD_MS 3000

static const char * const channel_names[NUM_CHANNELS] = {
	"BTSwitchIRK_1",
	"BTSwitchIRK_2",
	"BTSwitchIRK_3",
};

static uint8_t channel_ids[NUM_CHANNELS];
static int active_channel;
static int pending_channel;
static bool switch_requested;
static struct bt_conn *current_conn;

/* ---------------- GPIO ---------------- */

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec led    = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static struct gpio_callback button_cb_data;
static struct k_work_delayable debounce_work;
static struct k_work_delayable adv_retry_work;
static int adv_retry_channel;

/* ---------------- Advertising data ---------------- */

static uint8_t adv_name_buf[32];

static struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, 0xC1, 0x03), /* 0x03C1 = HID Keyboard */
	BT_DATA(BT_DATA_NAME_COMPLETE, adv_name_buf, 0),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0x12, 0x18), /* 0x1812 = HID Service */
};

/* ---------------- Helpers ---------------- */

static void set_led(bool on)
{
	gpio_pin_set_dt(&led, on ? 1 : 0);
}

static void start_advertising(int idx)
{
	size_t name_len = strlen(channel_names[idx]);

	memcpy(adv_name_buf, channel_names[idx], name_len);
	ad[2].data_len = name_len;

	struct bt_le_adv_param adv_param = {
		.id = channel_ids[idx],
		.options = BT_LE_ADV_OPT_CONN,
		.interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
		.interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
	};

	int err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err == -ENOMEM) {
		/* The connection object from the previous link may not be
		 * fully recycled yet (see bt_conn_cb.recycled). Retry
		 * shortly instead of giving up on this channel.
		 */
		LOG_WRN("No free connection object yet for channel %d, retrying...", idx + 1);
		adv_retry_channel = idx;
		k_work_reschedule(&adv_retry_work, K_MSEC(50));
		return;
	}

	if (err) {
		LOG_ERR("Advertising failed to start for channel %d (err %d)", idx + 1, err);
		return;
	}

	set_led(false);
	printk("\n=================================\n");
	printk("正在廣播頻道 %d (%s)...\n", idx + 1, channel_names[idx]);
	printk("=================================\n");
}

static void do_channel_switch(void)
{
	active_channel = pending_channel;
	start_advertising(active_channel);
}

static void adv_retry_handler(struct k_work *work)
{
	start_advertising(adv_retry_channel);
}

/* ---------------- BLE connection callbacks ---------------- */

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("Connection failed (err %u)", err);
		return;
	}

	current_conn = bt_conn_ref(conn);
	set_led(true);
	printk("裝置已連線 (頻道 %d)\n", active_channel + 1);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("裝置已斷線 (原因: %u)\n", reason);

	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	hid_keyboard_notify_disconnected();
	set_led(false);

	if (switch_requested) {
		switch_requested = false;
		do_channel_switch();
	} else {
		/* Stay on the same channel so the bonded device can
		 * reconnect on its own.
		 */
		start_advertising(active_channel);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

/* ---------------- Button handling ---------------- */

static void request_channel_switch(void)
{
	pending_channel = (active_channel + 1) % NUM_CHANNELS;
	switch_requested = true;

	printk("\n=================================\n");
	printk("按鈕已觸發，切換頻道中...\n");
	printk("=================================\n");

	bt_le_adv_stop();

	if (current_conn) {
		/* do_channel_switch() runs from disconnected() once the
		 * link is actually torn down - don't race it here.
		 */
		bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	} else {
		switch_requested = false;
		do_channel_switch();
	}
}

static void debounce_handler(struct k_work *work)
{
	if (gpio_pin_get_dt(&button)) {
		request_channel_switch();
	}
}

static void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	k_work_reschedule(&debounce_work, K_MSEC(DEBOUNCE_MS));
}

static int setup_gpio(void)
{
	int err;

	if (!gpio_is_ready_dt(&button) || !gpio_is_ready_dt(&led)) {
		LOG_ERR("GPIO device not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (err) {
		return err;
	}

	err = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (err) {
		return err;
	}

	err = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	if (err) {
		return err;
	}

	gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb_data);

	k_work_init_delayable(&debounce_work, debounce_handler);
	k_work_init_delayable(&adv_retry_work, adv_retry_handler);

	return 0;
}

/* ---------------- Identity setup ---------------- */

static void setup_identities(void)
{
	channel_ids[0] = BT_ID_DEFAULT;

	for (int i = 1; i < NUM_CHANNELS; i++) {
		int id = bt_id_create(NULL, NULL);

		if (id < 0) {
			LOG_ERR("bt_id_create failed for channel %d (err %d)", i + 1, id);
			channel_ids[i] = BT_ID_DEFAULT;
		} else {
			channel_ids[i] = (uint8_t)id;
		}
	}
}

/* ---------------- Factory reset (no SWD/nrfjprog needed) ----------------
 *
 * Since flashing via UF2 drag-and-drop only overwrites the application
 * region and never touches the dedicated storage/NVS partition, stale
 * bonding data can survive across reflashes and cause settings_load()
 * failures. This lets you wipe just that partition from within the
 * firmware itself: hold the channel button while powering on / resetting
 * the board (double-tap reset into UF2 mode still works exactly the same
 * as before; this check only runs during normal firmware boot).
 */
static void check_factory_reset(void)
{
	if (!gpio_pin_get_dt(&button)) {
		return; /* button not held at boot, nothing to do */
	}

	printk("偵測到開機時按住按鈕，請繼續按住 %d 秒以清除所有配對資料...\n",
	       FACTORY_RESET_HOLD_MS / 1000);

	int64_t start = k_uptime_get();

	while (gpio_pin_get_dt(&button)) {
		if (k_uptime_get() - start >= FACTORY_RESET_HOLD_MS) {
			break;
		}
		k_msleep(50);
	}

	if (k_uptime_get() - start < FACTORY_RESET_HOLD_MS) {
		printk("按住時間不足，取消清除，正常開機。\n");
		return;
	}

	printk("清除中，請勿斷電...\n");

	const struct flash_area *fa;
	int rc = flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa);

	if (rc) {
		LOG_ERR("flash_area_open failed (%d)", rc);
		return;
	}

	rc = flash_area_erase(fa, 0, fa->fa_size);
	flash_area_close(fa);

	if (rc) {
		LOG_ERR("flash_area_erase failed (%d)", rc);
		return;
	}

	printk("記憶已清除！3 秒後自動重新開機，請放開按鈕...\n");
	k_msleep(3000);
	sys_reboot(SYS_REBOOT_COLD);
	/* unreachable */
}

/* ---------------- Main ---------------- */

int main(void)
{
	int err;

	printk("*** Multi-Channel BLE Keyboard ***\n");

	err = setup_gpio();
	if (err) {
		LOG_ERR("GPIO setup failed (%d)", err);
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return 0;
	}

	check_factory_reset();

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	setup_identities();
	hid_keyboard_init();

	start_advertising(active_channel);

	return 0;
}
