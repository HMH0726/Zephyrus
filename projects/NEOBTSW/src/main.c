/*
  main.c
  - channel 管理 (3 channels)
  - Bluetooth init, advertising name 切換
  - button 短按: cycle channel
  - button 長按 (>2s): clear channel pairing
  - pairing complete callback: 存下 peer addr 到當前 channel
*/

#include <zephyr.h>
#include <sys/printk.h>
#include <settings/settings.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/conn.h>
#include <bluetooth/addr.h>
#include <bluetooth/gap.h>
#include <drivers/gpio.h>
#include <sys/byteorder.h>
#include <logging/log.h>

LOG_MODULE_REGISTER(mx_mouse, LOG_LEVEL_DBG);

/* 引入 HID 部分（實作在 hid_mouse.c） */
void hid_mouse_init(void);
int hid_send_mouse_report(int8_t dx, int8_t dy, int8_t wheel);

/* Config */
#define CHANNEL_COUNT 3
static const char *channel_names[CHANNEL_COUNT] = {
    "MXMouse-Ch1",
    "MXMouse-Ch2",
    "MXMouse-Ch3",
};

/* Button (sw0) device from devicetree */
#define SW_NODE DT_ALIAS(sw0)
#if DT_NODE_HAS_STATUS(SW_NODE, okay)
#define SW_GPIO_LABEL  DT_GPIO_LABEL(SW_NODE, gpios)
#define SW_GPIO_PIN    DT_GPIO_PIN(SW_NODE, gpios)
#define SW_GPIO_FLAGS  (DT_GPIO_FLAGS(SW_NODE, gpios) | GPIO_INPUT)
#else
#error "No sw0 alias in devicetree; update boards overlay to point to a button pin"
#endif

/* Per-channel stored peer addr */
struct channel_peer {
    bool has;
    bt_addr_le_t addr;
};

static struct channel_peer channels[CHANNEL_COUNT];
static int current_channel = 0;

/* Button handling */
static const struct device *button;
static struct gpio_callback button_cb_data;
static struct k_work_delayable longpress_work;
static struct k_timer press_timer;
static atomic_t pressed_flag = ATOMIC_INIT(0);

#define LONGPRESS_MS 2000

/* Helpers: settings key per channel */
static char settings_key[32];

static int save_channel_peer(int idx)
{
    if (idx < 0 || idx >= CHANNEL_COUNT) {
        return -EINVAL;
    }

    if (!channels[idx].has) {
        /* remove key */
        snprintf(settings_key, sizeof(settings_key), "mxmouse/ch%d", idx);
        settings_save_one(settings_key, NULL, 0); /* removing stored key may vary by backend; we just try to save empty - some backends don't delete; better to call settings_delete if available */
        return 0;
    }

    snprintf(settings_key, sizeof(settings_key), "mxmouse/ch%d", idx);
    return settings_save_one(settings_key, &channels[idx].addr, sizeof(bt_addr_le_t));
}

static int load_channel_peer_cb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
    /* name like "mxmouse/chN" */
    for (int i = 0; i < CHANNEL_COUNT; i++) {
        char key[32];
        snprintf(key, sizeof(key), "mxmouse/ch%d", i);
        if (strcmp(name, key) == 0) {
            bt_addr_le_t addr;
            ssize_t rc = read_cb(cb_arg, &addr, sizeof(addr));
            if (rc == sizeof(addr)) {
                channels[i].has = true;
                memcpy(&channels[i].addr, &addr, sizeof(addr));
                LOG_INF("Loaded channel %d peer", i+1);
            } else {
                channels[i].has = false;
            }
            return 0;
        }
    }
    return 0;
}

/* make advertising with name = channel_names[current_channel] */
static const struct bt_data ad[] = {
    /* BT_DATA bytes will be filled dynamically because name changes.
       We use BT_LE_ADV_CONN_NAME helper which automatically uses device name.
    */
};

static void start_advertising_for_channel(int idx);

/* whitelist helpers (if peer exists) */
static void setup_whitelist_for_channel(int idx)
{
    int err;

    bt_le_whitelist_clear();

    if (channels[idx].has) {
        err = bt_le_whitelist_add(&channels[idx].addr);
        if (err) {
            LOG_WRN("Failed to add peer to whitelist: %d", err);
        } else {
            LOG_INF("Whitelist added for channel %d", idx+1);
        }
    } else {
        LOG_INF("Channel %d has no saved peer; advertising open connect", idx+1);
    }
}

/* Bluetooth callbacks */
static void connected(struct bt_conn *conn, uint8_t err)
{
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_t peer;
    bt_addr_le_copy(&peer, bt_conn_get_dst(conn));
    bt_addr_le_to_str(&peer, addr_str, sizeof(addr_str));

    if (err) {
        LOG_ERR("Connection failed (err %u) from %s", err, addr_str);
        return;
    }

    LOG_INF("Connected: %s", addr_str);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_t peer;
    bt_addr_le_copy(&peer, bt_conn_get_dst(conn));
    bt_addr_le_to_str(&peer, addr_str, sizeof(addr_str));
    LOG_INF("Disconnected: %s (reason 0x%02x)", addr_str, reason);
}

static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
};

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_t peer;
    bt_addr_le_copy(&peer, bt_conn_get_dst(conn));
    bt_addr_le_to_str(&peer, addr_str, sizeof(addr_str));

    if (!err) {
        LOG_INF("Security for %s level %u", addr_str, level);
    } else {
        LOG_WRN("Security for %s failed: %d", addr_str, err);
    }
}

static void pair_complete(struct bt_conn *conn, bool bonded)
{
    /* On pairing complete, store the remote address into current channel */
    if (!conn) return;

    const bt_addr_le_t *dst = bt_conn_get_dst(conn);
    if (!dst) return;

    channels[current_channel].has = true;
    memcpy(&channels[current_channel].addr, dst, sizeof(bt_addr_le_t));
    save_channel_peer(current_channel);

    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(dst, addr_str, sizeof(addr_str));
    LOG_INF("Paired with %s on channel %d", addr_str, current_channel+1);

    /* After storing, set whitelist to only this peer for current channel */
    setup_whitelist_for_channel(current_channel);
}

static struct bt_conn_auth_info_cb auth_cb = {
    .pairing_complete = pair_complete,
    .pairing_failed = NULL,
};

/* Button work / timer handlers */
static void longpress_timeout(struct k_work *work)
{
    /* long press detected: unpair current channel */
    if (channels[current_channel].has) {
        int err = bt_unpair(BT_ID_DEFAULT, &channels[current_channel].addr);
        if (err) {
            LOG_ERR("bt_unpair failed: %d", err);
        } else {
            LOG_INF("Unpaired channel %d", current_channel+1);
            channels[current_channel].has = false;
            save_channel_peer(current_channel);
            /* Remove whitelist entry */
            bt_le_whitelist_clear();
        }
    } else {
        LOG_INF("Channel %d had no peer to unpair", current_channel+1);
    }
}

/* handle short press (cycle channel) */
static void switch_channel(void)
{
    current_channel = (current_channel + 1) % CHANNEL_COUNT;
    LOG_INF("Switched to channel %d (%s)", current_channel+1, channel_names[current_channel]);

    /* change local name and restart advertising */
    bt_set_name(channel_names[current_channel]);

    /* update whitelist if peer exists */
    setup_whitelist_for_channel(current_channel);

    start_advertising_for_channel(current_channel);
}

/* GPIO callback */
static void button_pressed_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    /* Debounce / start timer for long press */
    /* mark pressed and start delayed work for long press */
    atomic_set(&pressed_flag, 1);
    k_work_schedule(&longpress_work, K_MSEC(LONGPRESS_MS));
}

static void button_released(void)
{
    /* cancel longpress work if scheduled */
    k_work_cancel_delayable(&longpress_work);

    if (atomic_get(&pressed_flag)) {
        /* short press */
        switch_channel();
    }
    atomic_set(&pressed_flag, 0);
}

/* Because some boards report both press & release as edge, we implement a poll to check pin state.
   Simpler: use an interrupt on both edges and check gpio_pin_get. */
static void button_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    int val = gpio_pin_get(dev, SW_GPIO_PIN);
    if (val == 0) { /* active low: pressed */
        button_pressed_isr(dev, cb, pins);
    } else {
        /* released */
        button_released();
    }
}

/* Advertising control */
static void start_advertising_for_channel(int idx)
{
    int err;

    /* stop current advertising first */
    bt_le_adv_stop();

    /* set device name to channel name */
    bt_set_name(channel_names[idx]);

    /* Setup whitelist if have peer */
    setup_whitelist_for_channel(idx);

    /* Use connectable advertising with name */
    struct bt_le_adv_param adv_param = {
        .options = BT_LE_ADV_OPT_CONNECTABLE,
        .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
        .interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
        .id = BT_ID_DEFAULT,
    };

    const struct bt_data ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
        /* Name will be taken from bt_set_name if using bt_le_adv_start with conn name */
    };

    err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        LOG_ERR("Advertising failed to start (err %d)", err);
    } else {
        LOG_INF("Advertising started for channel %d (%s)", idx+1, channel_names[idx]);
    }
}

/* settings handler for our keys */
static int settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
    return load_channel_peer_cb(name, len, read_cb, cb_arg);
}

static int settings_init_cb(void)
{
    int rc = settings_subsys_init();
    if (rc) {
        LOG_ERR("settings_subsys_init failed: %d", rc);
        return rc;
    }
    settings_register(&((struct settings_handler){
        .name = "mxmouse",
        .h_set = settings_set,
        .h_commit = NULL,
        .h_export = NULL,
    }));
    settings_load();
    return 0;
}

void main(void)
{
    int err;

    LOG_INF("MX Multi-Channel Mouse starting...");

    /* initialize channels as empty */
    for (int i = 0; i < CHANNEL_COUNT; i++) {
        channels[i].has = false;
    }

    /* init settings so we can persist channel peers */
    settings_init_cb();

    /* initialize hid subsystem (HID service, reports) */
    hid_mouse_init();

    /* init bluetooth */
    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("Bluetooth init failed (err %d)", err);
        return;
    }
    LOG_INF("Bluetooth initialized");

    bt_conn_cb_register(&conn_callbacks);
    bt_conn_auth_info_cb_register(&auth_cb);

    /* initialize button device */
    button = device_get_binding(SW_GPIO_LABEL);
    if (!button) {
        LOG_ERR("Button device %s not found", SW_GPIO_LABEL);
    } else {
        gpio_pin_configure(button, SW_GPIO_PIN, GPIO_INPUT | GPIO_PULL_UP);
        gpio_pin_interrupt_configure(button, SW_GPIO_PIN, GPIO_INT_EDGE_BOTH);
        gpio_init_callback(&button_cb_data, button_handler, BIT(SW_GPIO_PIN));
        gpio_add_callback(button, &button_cb_data);
        LOG_INF("Button initialized on %s pin %d", SW_GPIO_LABEL, SW_GPIO_PIN);
    }

    k_work_init_delayable(&longpress_work, longpress_timeout);

    /* start advertising for initial channel */
    start_advertising_for_channel(current_channel);

    /* Simple demo: send small mouse jitter every 5s (optional) */
    while (1) {
        /* sleep 5s */
        k_sleep(K_SECONDS(5));
        /* send small move to show HID path works if connected */
        hid_send_mouse_report(5, 0, 0);
    }
}