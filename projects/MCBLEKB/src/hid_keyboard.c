/*
 * Minimal HID-over-GATT (HOGP) keyboard service.
 *
 * Vanilla Zephyr does not ship a ready-made "bt_hids" library (that only
 * exists in Nordic's nRF Connect SDK). This file implements the small
 * subset of the HID service needed for a standard boot-protocol keyboard:
 *   - HID Information
 *   - Report Map (the HID report descriptor)
 *   - Input Report (notify) + CCC + Report Reference descriptor
 *   - Output Report (write) + Report Reference descriptor  (LED state)
 *   - HID Control Point (write)
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <string.h>
#include <errno.h>

#include "hid_keyboard.h"

enum {
	HIDS_INPUT   = 0x01,
	HIDS_OUTPUT  = 0x02,
	HIDS_FEATURE = 0x03,
};

enum {
	HIDS_NORMALLY_CONNECTABLE = BIT(1),
};

struct hids_info {
	uint16_t version;
	uint8_t  country_code;
	uint8_t  flags;
} __packed;

struct hids_report_ref {
	uint8_t id;
	uint8_t type;
} __packed;

/* Standard USB HID boot-keyboard report descriptor:
 * 1 modifier byte + 1 reserved byte + 6 keycode bytes = 8-byte report.
 */
static const uint8_t hid_report_map_data[] = {
	0x05, 0x01, /* Usage Page (Generic Desktop) */
	0x09, 0x06, /* Usage (Keyboard) */
	0xA1, 0x01, /* Collection (Application) */
	0x85, 0x01, /*   Report Id (1) */
	0x05, 0x07, /*   Usage Page (Key Codes) */
	0x19, 0xE0, /*   Usage Minimum (224) */
	0x29, 0xE7, /*   Usage Maximum (231) */
	0x15, 0x00, /*   Logical Minimum (0) */
	0x25, 0x01, /*   Logical Maximum (1) */
	0x75, 0x01, /*   Report Size (1) */
	0x95, 0x08, /*   Report Count (8) */
	0x81, 0x02, /*   Input (Data, Variable, Absolute) - modifier byte */
	0x95, 0x01, /*   Report Count (1) */
	0x75, 0x08, /*   Report Size (8) */
	0x81, 0x01, /*   Input (Constant) - reserved byte */
	0x95, 0x05, /*   Report Count (5) */
	0x75, 0x01, /*   Report Size (1) */
	0x05, 0x08, /*   Usage Page (LEDs) */
	0x19, 0x01, /*   Usage Minimum (1) */
	0x29, 0x05, /*   Usage Maximum (5) */
	0x91, 0x02, /*   Output (Data, Variable, Absolute) - LED report */
	0x95, 0x01, /*   Report Count (1) */
	0x75, 0x03, /*   Report Size (3) */
	0x91, 0x01, /*   Output (Constant) - LED report padding */
	0x95, 0x06, /*   Report Count (6) */
	0x75, 0x08, /*   Report Size (8) */
	0x15, 0x00, /*   Logical Minimum (0) */
	0x25, 0x65, /*   Logical Maximum (101) */
	0x05, 0x07, /*   Usage Page (Key Codes) */
	0x19, 0x00, /*   Usage Minimum (0) */
	0x29, 0x65, /*   Usage Maximum (101) */
	0x81, 0x00, /*   Input (Data, Array) - up to 6 simultaneous keys */
	0xC0        /* End Collection */
};

static const struct hids_info info = {
	.version = 0x0000,
	.country_code = 0x00,
	.flags = HIDS_NORMALLY_CONNECTABLE,
};

static const struct hids_report_ref input_report_ref  = { .id = 0x01, .type = HIDS_INPUT };
static const struct hids_report_ref output_report_ref = { .id = 0x01, .type = HIDS_OUTPUT };

static uint8_t input_report[8];
static uint8_t output_report_value[1];
static uint8_t ctrl_point_value;
static bool notifications_enabled;

/* --- GATT read/write callbacks --- */

static ssize_t read_info(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &info, sizeof(info));
}

static ssize_t read_report_map(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				  hid_report_map_data, sizeof(hid_report_map_data));
}

static ssize_t read_input_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				  void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				  input_report, sizeof(input_report));
}

static ssize_t read_report_ref(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				void *buf, uint16_t len, uint16_t offset)
{
	const struct hids_report_ref *ref = attr->user_data;

	return bt_gatt_attr_read(conn, attr, buf, len, offset, ref, sizeof(*ref));
}

static void input_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notifications_enabled = (value == BT_GATT_CCC_NOTIFY);
}

static ssize_t write_output_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				    const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	uint8_t *value = attr->user_data;

	if (offset + len > sizeof(output_report_value)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	memcpy(value + offset, buf, len);
	/* value[0] bit0 = Num Lock, bit1 = Caps Lock, bit2 = Scroll Lock, etc. */
	return len;
}

static ssize_t write_ctrl_point(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				 const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	uint8_t *value = attr->user_data;

	if (offset + len > sizeof(ctrl_point_value)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	memcpy(value + offset, buf, len);
	return len;
}

/*
 * Service layout (attribute indices, for reference - see hid_keyboard_send_report):
 *  0  Primary Service
 *  1  Characteristic Decl - HID Information
 *  2  Value               - HID Information
 *  3  Characteristic Decl - Report Map
 *  4  Value               - Report Map
 *  5  Characteristic Decl - Report (Input)
 *  6  Value               - Input Report   <-- notify target
 *  7  CCC
 *  8  Descriptor           - Report Reference (Input)
 *  9  Characteristic Decl - Report (Output)
 * 10  Value               - Output Report
 * 11  Descriptor           - Report Reference (Output)
 * 12  Characteristic Decl - HID Control Point
 * 13  Value               - HID Control Point
 */
BT_GATT_SERVICE_DEFINE(hids_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),

	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO, BT_GATT_CHRC_READ,
				BT_GATT_PERM_READ, read_info, NULL, NULL),

	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP, BT_GATT_CHRC_READ,
				BT_GATT_PERM_READ, read_report_map, NULL, NULL),

	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
				BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
				BT_GATT_PERM_READ, read_input_report, NULL, NULL),
	BT_GATT_CCC(input_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ,
			   read_report_ref, NULL, (void *)&input_report_ref),

	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
				BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
				BT_GATT_PERM_WRITE, NULL, write_output_report,
				output_report_value),
	BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ,
			   read_report_ref, NULL, (void *)&output_report_ref),

	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CTRL_POINT,
				BT_GATT_CHRC_WRITE_WITHOUT_RESP,
				BT_GATT_PERM_WRITE, NULL, write_ctrl_point,
				&ctrl_point_value),
);

#define INPUT_REPORT_VALUE_ATTR (&hids_svc.attrs[6])

void hid_keyboard_init(void)
{
	memset(input_report, 0, sizeof(input_report));
	notifications_enabled = false;
}

void hid_keyboard_notify_disconnected(void)
{
	notifications_enabled = false;
}

int hid_keyboard_send_report(struct bt_conn *conn, uint8_t modifier,
			      const uint8_t keycodes[6])
{
	if (!conn || !notifications_enabled) {
		return -ENOTCONN;
	}

	input_report[0] = modifier;
	input_report[1] = 0x00; /* reserved */
	memcpy(&input_report[2], keycodes, 6);

	return bt_gatt_notify(conn, INPUT_REPORT_VALUE_ATTR, input_report, sizeof(input_report));
}

int hid_keyboard_send_key(struct bt_conn *conn, uint8_t modifier, uint8_t keycode)
{
	uint8_t keys_down[6] = { keycode, 0, 0, 0, 0, 0 };
	uint8_t keys_up[6]   = { 0 };
	int err;

	err = hid_keyboard_send_report(conn, modifier, keys_down);
	if (err) {
		return err;
	}

	k_sleep(K_MSEC(10));

	return hid_keyboard_send_report(conn, 0, keys_up);
}
