#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "hog_mouse.h"

#define CHANNEL_COUNT 3U
#define INPUT_POLL_MS 12
#define RECONNECT_ADV_DELAY_MS 150

BUILD_ASSERT(CONFIG_BT_ID_MAX >= CHANNEL_COUNT,
	     "CONFIG_BT_ID_MAX must be at least 3 for three mouse channels");

struct mouse_channel {
	uint8_t id;
	const char *name;
};

static struct mouse_channel channels[CHANNEL_COUNT] = {
	{ .id = BT_ID_DEFAULT, .name = "MX Master CH1" },
	{ .id = BT_ID_DEFAULT, .name = "MX Master CH2" },
	{ .id = BT_ID_DEFAULT, .name = "MX Master CH3" },
};

static uint8_t active_channel;
static struct bt_conn *active_conn;
static uint8_t latched_buttons;

static K_MUTEX_DEFINE(state_lock);
static atomic_t bt_ready;

static void advertise_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(advertise_work, advertise_work_handler);

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL)),
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, 0xC2, 0x03), /* HID Mouse: 0x03c2 */
};

#define MX_NEXT_NODE DT_ALIAS(mx_next)
#if DT_NODE_HAS_STATUS_OKAY(MX_NEXT_NODE)
static const struct gpio_dt_spec mx_next_button = GPIO_DT_SPEC_GET(MX_NEXT_NODE, gpios);
#define HAS_MX_NEXT_BUTTON 1
#else
#define HAS_MX_NEXT_BUTTON 0
#endif

#define MX_LEFT_NODE DT_ALIAS(mx_left)
#if DT_NODE_HAS_STATUS_OKAY(MX_LEFT_NODE)
static const struct gpio_dt_spec mx_left_button = GPIO_DT_SPEC_GET(MX_LEFT_NODE, gpios);
#define HAS_MX_LEFT_BUTTON 1
#else
#define HAS_MX_LEFT_BUTTON 0
#endif

#define MX_RIGHT_NODE DT_ALIAS(mx_right)
#if DT_NODE_HAS_STATUS_OKAY(MX_RIGHT_NODE)
static const struct gpio_dt_spec mx_right_button = GPIO_DT_SPEC_GET(MX_RIGHT_NODE, gpios);
#define HAS_MX_RIGHT_BUTTON 1
#else
#define HAS_MX_RIGHT_BUTTON 0
#endif

#define MX_MIDDLE_NODE DT_ALIAS(mx_middle)
#if DT_NODE_HAS_STATUS_OKAY(MX_MIDDLE_NODE)
static const struct gpio_dt_spec mx_middle_button = GPIO_DT_SPEC_GET(MX_MIDDLE_NODE, gpios);
#define HAS_MX_MIDDLE_BUTTON 1
#else
#define HAS_MX_MIDDLE_BUTTON 0
#endif

#define MX_WHEEL_UP_NODE DT_ALIAS(mx_wheel_up)
#if DT_NODE_HAS_STATUS_OKAY(MX_WHEEL_UP_NODE)
static const struct gpio_dt_spec mx_wheel_up_button = GPIO_DT_SPEC_GET(MX_WHEEL_UP_NODE, gpios);
#define HAS_MX_WHEEL_UP_BUTTON 1
#else
#define HAS_MX_WHEEL_UP_BUTTON 0
#endif

#define MX_WHEEL_DOWN_NODE DT_ALIAS(mx_wheel_down)
#if DT_NODE_HAS_STATUS_OKAY(MX_WHEEL_DOWN_NODE)
static const struct gpio_dt_spec mx_wheel_down_button = GPIO_DT_SPEC_GET(MX_WHEEL_DOWN_NODE, gpios);
#define HAS_MX_WHEEL_DOWN_BUTTON 1
#else
#define HAS_MX_WHEEL_DOWN_BUTTON 0
#endif

static void schedule_advertising(k_timeout_t delay)
{
	if (atomic_get(&bt_ready)) {
		(void)k_work_schedule(&advertise_work, delay);
	}
}

static int channel_from_id(uint8_t id)
{
	for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
		if (channels[i].id == id) {
			return i;
		}
	}

	return -ENOENT;
}

static bool parse_channel_arg(const char *arg, uint8_t *channel)
{
	char *end = NULL;
	long value = strtol(arg, &end, 10);

	if (end == arg || *end != '\0' || value < 1 || value > CHANNEL_COUNT) {
		return false;
	}

	*channel = (uint8_t)(value - 1);
	return true;
}

static int8_t clamp_hid_axis(long value)
{
	if (value > INT8_MAX) {
		return INT8_MAX;
	}

	if (value < INT8_MIN) {
		return INT8_MIN;
	}

	return (int8_t)value;
}

static struct bt_conn *get_active_conn_ref(void)
{
	struct bt_conn *conn = NULL;

	k_mutex_lock(&state_lock, K_FOREVER);
	if (active_conn != NULL) {
		conn = bt_conn_ref(active_conn);
	}
	k_mutex_unlock(&state_lock);

	return conn;
}

static void send_mouse_report(uint8_t buttons, int8_t x, int8_t y,
			      int8_t wheel, int8_t pan)
{
	struct bt_conn *conn = get_active_conn_ref();

	if (conn == NULL) {
		return;
	}

	int err = hog_mouse_send(conn, buttons, x, y, wheel, pan);

	if (err != 0 && err != -ENOTCONN && err != -EACCES && err != -EINVAL) {
		printk("HID notify failed: %d\n", err);
	}

	bt_conn_unref(conn);
}

static void disconnect_active_conn(uint8_t reason)
{
	struct bt_conn *conn = get_active_conn_ref();

	if (conn == NULL) {
		return;
	}

	int err = bt_conn_disconnect(conn, reason);

	if (err != 0 && err != -ENOTCONN) {
		printk("Disconnect failed: %d\n", err);
	}

	bt_conn_unref(conn);
}

static int select_channel(uint8_t channel)
{
	if (channel >= CHANNEL_COUNT) {
		return -EINVAL;
	}

	uint8_t old_channel;
	bool changed;
	bool had_connection;

	k_mutex_lock(&state_lock, K_FOREVER);
	old_channel = active_channel;
	changed = (active_channel != channel);
	active_channel = channel;
	had_connection = (active_conn != NULL);
	k_mutex_unlock(&state_lock);

	if (!changed) {
		printk("Already on channel %u (%s)\n", channel + 1,
		       channels[channel].name);
		return 0;
	}

	printk("Switch channel %u -> %u (%s)\n", old_channel + 1,
	       channel + 1, channels[channel].name);

	latched_buttons = 0;
	hog_mouse_release_local_state();
	(void)bt_le_adv_stop();

	if (had_connection) {
		disconnect_active_conn(BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	} else {
		schedule_advertising(K_NO_WAIT);
	}

	return 0;
}

static int select_next_channel(void)
{
	uint8_t next;

	k_mutex_lock(&state_lock, K_FOREVER);
	next = (active_channel + 1U) % CHANNEL_COUNT;
	k_mutex_unlock(&state_lock);

	return select_channel(next);
}

static void advertise_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!atomic_get(&bt_ready)) {
		return;
	}

	uint8_t channel;
	uint8_t id;
	const char *name;
	bool connected;

	k_mutex_lock(&state_lock, K_FOREVER);
	channel = active_channel;
	id = channels[channel].id;
	name = channels[channel].name;
	connected = (active_conn != NULL);
	k_mutex_unlock(&state_lock);

	if (connected) {
		return;
	}

	(void)bt_le_adv_stop();

	int err = bt_set_name(name);

	if (err != 0) {
		printk("bt_set_name(%s) failed: %d\n", name, err);
	}

	struct bt_le_adv_param adv_param = BT_LE_ADV_PARAM_INIT(
		BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY,
		BT_GAP_ADV_FAST_INT_MIN_2,
		BT_GAP_ADV_FAST_INT_MAX_2,
		NULL);
	adv_param.id = id;

	struct bt_data sd[] = {
		BT_DATA(BT_DATA_NAME_COMPLETE, name, strlen(name)),
	};

	err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err != 0) {
		printk("Advertising channel %u failed: %d\n", channel + 1, err);
		schedule_advertising(K_MSEC(1000));
		return;
	}

	printk("Advertising %s on BLE identity %u\n", name, id);
}

static int ensure_three_identities(void)
{
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);
	int err;

	channels[0].id = BT_ID_DEFAULT;
	bt_id_get(addrs, &count);

	for (uint8_t i = 1; i < CHANNEL_COUNT; i++) {
		if (i < count) {
			channels[i].id = i;
			continue;
		}

		err = bt_id_create(NULL, NULL);
		if (err < 0) {
			printk("bt_id_create for channel %u failed: %d\n", i + 1, err);
			return err;
		}

		channels[i].id = (uint8_t)err;
	}

	count = ARRAY_SIZE(addrs);
	bt_id_get(addrs, &count);
	printk("Bluetooth identities: %u\n", (unsigned int)count);

	for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
		char addr[BT_ADDR_LE_STR_LEN];

		if (channels[i].id < count) {
			bt_addr_le_to_str(&addrs[channels[i].id], addr, sizeof(addr));
		} else {
			strcpy(addr, "unknown");
		}

		printk("  CH%u name='%s' id=%u addr=%s\n", i + 1,
		       channels[i].name, channels[i].id, addr);
	}

	return 0;
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err != 0) {
		printk("Connection failed: %u\n", err);
		schedule_advertising(K_MSEC(500));
		return;
	}

	struct bt_conn_info info;
	uint8_t conn_id = BT_ID_DEFAULT;
	int channel = -1;

	if (bt_conn_get_info(conn, &info) == 0) {
		conn_id = info.id;
		channel = channel_from_id(conn_id);
	}

	k_mutex_lock(&state_lock, K_FOREVER);
	bool accept = (channel >= 0 && (uint8_t)channel == active_channel && active_conn == NULL);
	if (accept) {
		active_conn = bt_conn_ref(conn);
	}
	k_mutex_unlock(&state_lock);

	if (!accept) {
		printk("Reject connection on identity %u while active channel is %u\n",
		       conn_id, active_channel + 1);
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	printk("Connected on CH%u (%s)\n", channel + 1, channels[channel].name);

	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err != 0 && err != -EALREADY) {
		printk("Security request failed: %d\n", err);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	bool was_active = false;

	k_mutex_lock(&state_lock, K_FOREVER);
	if (active_conn == conn) {
		bt_conn_unref(active_conn);
		active_conn = NULL;
		was_active = true;
	}
	k_mutex_unlock(&state_lock);

	if (was_active) {
		latched_buttons = 0;
		hog_mouse_release_local_state();
		printk("Disconnected, reason 0x%02x\n", reason);
	}
}

static void recycled(void)
{
	schedule_advertising(K_MSEC(RECONNECT_ADV_DELAY_MS));
}

static const char *security_err_name(enum bt_security_err err)
{
	const char *name = bt_security_err_to_str(err);

	return (name != NULL && name[0] != '\0') ? name : "unknown";
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	ARG_UNUSED(conn);

	if (err == BT_SECURITY_ERR_SUCCESS) {
		printk("Security level %u\n", level);
	} else {
		printk("Security failed: level %u err %u (%s)\n", level, err,
		       security_err_name(err));
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
	.security_changed = security_changed,
};

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	ARG_UNUSED(conn);
	printk("Pairing complete, bonded=%u\n", bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	ARG_UNUSED(conn);
	printk("Pairing failed, reason=%u (%s)\n", reason,
	       security_err_name(reason));
}

#if IS_ENABLED(CONFIG_BT_SMP_APP_PAIRING_ACCEPT)
static enum bt_security_err pairing_accept(struct bt_conn *conn,
					   const struct bt_conn_pairing_feat *const feat)
{
	struct bt_conn_info info;
	uint8_t conn_id = BT_ID_DEFAULT;
	int channel = -1;

	if (bt_conn_get_info(conn, &info) == 0) {
		conn_id = info.id;
		channel = channel_from_id(conn_id);
	}

	printk("Pairing request on CH%d id=%u: io=%u oob=%u auth=0x%02x key_size=%u init_key=0x%02x resp_key=0x%02x\n",
	       channel >= 0 ? channel + 1 : 0, conn_id,
	       feat->io_capability, feat->oob_data_flag, feat->auth_req,
	       feat->max_enc_key_size, feat->init_key_dist, feat->resp_key_dist);

	return BT_SECURITY_ERR_SUCCESS;
}
#endif

static struct bt_conn_auth_cb auth_callbacks = {
#if IS_ENABLED(CONFIG_BT_SMP_APP_PAIRING_ACCEPT)
	.pairing_accept = pairing_accept,
#endif
};

static struct bt_conn_auth_info_cb auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

static int configure_gpio_button(const struct gpio_dt_spec *button,
				 const char *name)
{
	if (!gpio_is_ready_dt(button)) {
		printk("GPIO for %s is not ready\n", name);
		return -ENODEV;
	}

	int err = gpio_pin_configure_dt(button, GPIO_INPUT);
	if (err != 0) {
		printk("GPIO configure failed for %s: %d\n", name, err);
		return err;
	}

	return 0;
}

static bool read_gpio_button(const struct gpio_dt_spec *button)
{
	int value = gpio_pin_get_dt(button);

	return value > 0;
}

static void configure_inputs(void)
{
#if HAS_MX_NEXT_BUTTON
	(void)configure_gpio_button(&mx_next_button, "mx-next");
#endif
#if HAS_MX_LEFT_BUTTON
	(void)configure_gpio_button(&mx_left_button, "mx-left");
#endif
#if HAS_MX_RIGHT_BUTTON
	(void)configure_gpio_button(&mx_right_button, "mx-right");
#endif
#if HAS_MX_MIDDLE_BUTTON
	(void)configure_gpio_button(&mx_middle_button, "mx-middle");
#endif
#if HAS_MX_WHEEL_UP_BUTTON
	(void)configure_gpio_button(&mx_wheel_up_button, "mx-wheel-up");
#endif
#if HAS_MX_WHEEL_DOWN_BUTTON
	(void)configure_gpio_button(&mx_wheel_down_button, "mx-wheel-down");
#endif
}

static void poll_inputs_forever(void)
{
	bool next_was_pressed = false;
	bool wheel_up_was_pressed = false;
	bool wheel_down_was_pressed = false;

	while (true) {
		uint8_t buttons = 0;
		bool pressed;

#if HAS_MX_NEXT_BUTTON
		pressed = read_gpio_button(&mx_next_button);
		if (pressed && !next_was_pressed) {
			(void)select_next_channel();
		}
		next_was_pressed = pressed;
#endif

#if HAS_MX_LEFT_BUTTON
		if (read_gpio_button(&mx_left_button)) {
			buttons |= HOG_MOUSE_BTN_LEFT;
		}
#endif
#if HAS_MX_RIGHT_BUTTON
		if (read_gpio_button(&mx_right_button)) {
			buttons |= HOG_MOUSE_BTN_RIGHT;
		}
#endif
#if HAS_MX_MIDDLE_BUTTON
		if (read_gpio_button(&mx_middle_button)) {
			buttons |= HOG_MOUSE_BTN_MIDDLE;
		}
#endif

		if (buttons != latched_buttons) {
			latched_buttons = buttons;
			send_mouse_report(latched_buttons, 0, 0, 0, 0);
		}

#if HAS_MX_WHEEL_UP_BUTTON
		pressed = read_gpio_button(&mx_wheel_up_button);
		if (pressed && !wheel_up_was_pressed) {
			send_mouse_report(latched_buttons, 0, 0, 1, 0);
		}
		wheel_up_was_pressed = pressed;
#endif
#if HAS_MX_WHEEL_DOWN_BUTTON
		pressed = read_gpio_button(&mx_wheel_down_button);
		if (pressed && !wheel_down_was_pressed) {
			send_mouse_report(latched_buttons, 0, 0, -1, 0);
		}
		wheel_down_was_pressed = pressed;
#endif

		k_sleep(K_MSEC(INPUT_POLL_MS));
	}
}

#if IS_ENABLED(CONFIG_SHELL)
static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	k_mutex_lock(&state_lock, K_FOREVER);
	uint8_t channel = active_channel;
	bool connected = (active_conn != NULL);
	k_mutex_unlock(&state_lock);

	shell_print(sh, "active: CH%u %s", channel + 1, channels[channel].name);
	shell_print(sh, "connected: %s", connected ? "yes" : "no");
	for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
		shell_print(sh, "CH%u: id=%u name=%s", i + 1,
			    channels[i].id, channels[i].name);
	}

	return 0;
}

static int cmd_channel(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	uint8_t channel;
	if (!parse_channel_arg(argv[1], &channel)) {
		shell_error(sh, "usage: mx channel <1|2|3>");
		return -EINVAL;
	}

	return select_channel(channel);
}

static int cmd_next(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return select_next_channel();
}

static int cmd_move(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 3) {
		shell_error(sh, "usage: mx move <dx> <dy> [wheel] [pan] [buttons]");
		return -EINVAL;
	}

	long dx = strtol(argv[1], NULL, 0);
	long dy = strtol(argv[2], NULL, 0);
	long wheel = (argc > 3) ? strtol(argv[3], NULL, 0) : 0;
	long pan = (argc > 4) ? strtol(argv[4], NULL, 0) : 0;
	long buttons = (argc > 5) ? strtol(argv[5], NULL, 0) : latched_buttons;

	latched_buttons = (uint8_t)buttons & 0x1f;
	send_mouse_report(latched_buttons, clamp_hid_axis(dx), clamp_hid_axis(dy),
			  clamp_hid_axis(wheel), clamp_hid_axis(pan));

	return 0;
}

static uint8_t button_mask_from_name(const char *name)
{
	if (strcmp(name, "left") == 0) {
		return HOG_MOUSE_BTN_LEFT;
	}
	if (strcmp(name, "right") == 0) {
		return HOG_MOUSE_BTN_RIGHT;
	}
	if (strcmp(name, "middle") == 0) {
		return HOG_MOUSE_BTN_MIDDLE;
	}
	if (strcmp(name, "back") == 0) {
		return HOG_MOUSE_BTN_BACK;
	}
	if (strcmp(name, "forward") == 0) {
		return HOG_MOUSE_BTN_FORWARD;
	}

	return 0;
}

static int cmd_click(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	uint8_t mask = button_mask_from_name(argv[1]);
	if (mask == 0) {
		shell_error(sh, "usage: mx click <left|right|middle|back|forward>");
		return -EINVAL;
	}

	latched_buttons |= mask;
	send_mouse_report(latched_buttons, 0, 0, 0, 0);
	k_sleep(K_MSEC(35));
	latched_buttons &= ~mask;
	send_mouse_report(latched_buttons, 0, 0, 0, 0);

	return 0;
}

static int unpair_one(uint8_t channel)
{
	int err = bt_unpair(channels[channel].id, NULL);
	if (err != 0) {
		printk("Unpair CH%u failed: %d\n", channel + 1, err);
		return err;
	}

	printk("Unpaired CH%u (%s)\n", channel + 1, channels[channel].name);
	return 0;
}

static int cmd_unpair(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (strcmp(argv[1], "all") == 0) {
		int first_err = 0;

		disconnect_active_conn(BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
			int err = unpair_one(i);
			if (err != 0 && first_err == 0) {
				first_err = err;
			}
		}
		schedule_advertising(K_MSEC(RECONNECT_ADV_DELAY_MS));
		return first_err;
	}

	uint8_t channel;
	if (!parse_channel_arg(argv[1], &channel)) {
		shell_error(sh, "usage: mx unpair <1|2|3|all>");
		return -EINVAL;
	}

	disconnect_active_conn(BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	int err = unpair_one(channel);
	schedule_advertising(K_MSEC(RECONNECT_ADV_DELAY_MS));
	return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(mx_subcmds,
	SHELL_CMD(status, NULL, "Show active channel and connection state.", cmd_status),
	SHELL_CMD_ARG(channel, NULL, "Select channel: mx channel <1|2|3>.", cmd_channel, 2, 0),
	SHELL_CMD(next, NULL, "Switch to the next channel.", cmd_next),
	SHELL_CMD_ARG(move, NULL, "Send movement: mx move <dx> <dy> [wheel] [pan] [buttons].", cmd_move, 3, 3),
	SHELL_CMD_ARG(click, NULL, "Click: mx click <left|right|middle|back|forward>.", cmd_click, 2, 0),
	SHELL_CMD_ARG(unpair, NULL, "Clear bonds: mx unpair <1|2|3|all>.", cmd_unpair, 2, 0),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(mx, &mx_subcmds, "Three-channel BLE HID mouse controls.", NULL);
#endif /* CONFIG_SHELL */

int main(void)
{
	int err;

	printk("MX Master-style three-channel BLE HID mouse\n");

	hog_mouse_init();
	configure_inputs();

	err = bt_conn_auth_cb_register(&auth_callbacks);
	if (err != 0) {
		printk("Auth callback register failed: %d\n", err);
	}

	err = bt_conn_auth_info_cb_register(&auth_info_callbacks);
	if (err != 0) {
		printk("Auth info callback register failed: %d\n", err);
	}

	err = bt_enable(NULL);
	if (err != 0) {
		printk("Bluetooth init failed: %d\n", err);
		return 0;
	}

	printk("Bluetooth initialized\n");

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		err = settings_load();
		if (err != 0) {
			printk("settings_load failed: %d\n", err);
		}
	}

	err = ensure_three_identities();
	if (err != 0) {
		printk("Cannot create three BLE identities: %d\n", err);
		return 0;
	}

	atomic_set(&bt_ready, 1);
	schedule_advertising(K_NO_WAIT);

	poll_inputs_forever();
	return 0;
}

