#include "hog_mouse.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

/* One report characteristic, no Report ID in the payload:
 *   byte 0: buttons bit0..bit4 = left/right/middle/back/forward
 *   byte 1: X relative movement
 *   byte 2: Y relative movement
 *   byte 3: vertical wheel
 *   byte 4: horizontal pan
 */
#define HOG_MOUSE_REPORT_SIZE 5
#define HOG_INPUT_REPORT_VALUE_ATTR 6

static atomic_t ccc_enabled;
static uint8_t input_report[HOG_MOUSE_REPORT_SIZE];

static const uint8_t hids_info[] = {
	0x11, 0x01, /* HID Class Specification release 1.11 */
	0x00,       /* Country code: not localized */
	0x03,       /* Remote wake + normally connectable */
};

static const uint8_t report_map[] = {
	0x05, 0x01,       /* Usage Page (Generic Desktop) */
	0x09, 0x02,       /* Usage (Mouse) */
	0xA1, 0x01,       /* Collection (Application) */
	0x09, 0x01,       /*   Usage (Pointer) */
	0xA1, 0x00,       /*   Collection (Physical) */

	0x05, 0x09,       /*     Usage Page (Button) */
	0x19, 0x01,       /*     Usage Minimum (Button 1) */
	0x29, 0x05,       /*     Usage Maximum (Button 5) */
	0x15, 0x00,       /*     Logical Minimum (0) */
	0x25, 0x01,       /*     Logical Maximum (1) */
	0x95, 0x05,       /*     Report Count (5) */
	0x75, 0x01,       /*     Report Size (1) */
	0x81, 0x02,       /*     Input (Data, Variable, Absolute) */
	0x95, 0x01,       /*     Report Count (1) */
	0x75, 0x03,       /*     Report Size (3) */
	0x81, 0x03,       /*     Input (Constant, Variable, Absolute) */

	0x05, 0x01,       /*     Usage Page (Generic Desktop) */
	0x09, 0x30,       /*     Usage (X) */
	0x09, 0x31,       /*     Usage (Y) */
	0x09, 0x38,       /*     Usage (Wheel) */
	0x15, 0x81,       /*     Logical Minimum (-127) */
	0x25, 0x7F,       /*     Logical Maximum (127) */
	0x75, 0x08,       /*     Report Size (8) */
	0x95, 0x03,       /*     Report Count (3) */
	0x81, 0x06,       /*     Input (Data, Variable, Relative) */

	0x05, 0x0C,       /*     Usage Page (Consumer) */
	0x0A, 0x38, 0x02, /*     Usage (AC Pan) */
	0x15, 0x81,       /*     Logical Minimum (-127) */
	0x25, 0x7F,       /*     Logical Maximum (127) */
	0x75, 0x08,       /*     Report Size (8) */
	0x95, 0x01,       /*     Report Count (1) */
	0x81, 0x06,       /*     Input (Data, Variable, Relative) */

	0xC0,             /*   End Collection */
	0xC0,             /* End Collection */
};

static const uint8_t report_ref[] = {
	0x00, /* Report ID: none */
	0x01, /* Report type: input */
};

static ssize_t read_hids_info(struct bt_conn *conn,
			      const struct bt_gatt_attr *attr,
			      void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				     hids_info, sizeof(hids_info));
}

static ssize_t read_report_map(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr,
			       void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				     report_map, sizeof(report_map));
}

static ssize_t read_input_report(struct bt_conn *conn,
				 const struct bt_gatt_attr *attr,
				 void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				     input_report, sizeof(input_report));
}

static ssize_t read_report_ref(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr,
			       void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				     report_ref, sizeof(report_ref));
}

static ssize_t write_ctrl_point(struct bt_conn *conn,
				const struct bt_gatt_attr *attr,
				const void *buf, uint16_t len,
				uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(buf);
	ARG_UNUSED(flags);

	if (offset != 0U || len != 1U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	return len;
}

static void input_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	atomic_set(&ccc_enabled, value == BT_GATT_CCC_NOTIFY);
}

BT_GATT_SERVICE_DEFINE(hog_mouse_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO,
			       BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ_ENCRYPT,
			       read_hids_info, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP,
			       BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ_ENCRYPT,
			       read_report_map, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ_ENCRYPT,
			       read_input_report, NULL, input_report),
	BT_GATT_CCC(input_ccc_changed,
		    BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
	BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF,
			   BT_GATT_PERM_READ_ENCRYPT,
			   read_report_ref, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CTRL_POINT,
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE_ENCRYPT,
			       NULL, write_ctrl_point, NULL)
);

int hog_mouse_init(void)
{
	memset(input_report, 0, sizeof(input_report));
	atomic_clear(&ccc_enabled);
	return 0;
}

int hog_mouse_send(struct bt_conn *conn, uint8_t buttons,
		   int8_t x, int8_t y, int8_t wheel, int8_t pan)
{
	if (conn == NULL) {
		return -ENOTCONN;
	}

	input_report[0] = buttons & 0x1f;
	input_report[1] = (uint8_t)x;
	input_report[2] = (uint8_t)y;
	input_report[3] = (uint8_t)wheel;
	input_report[4] = (uint8_t)pan;

	return bt_gatt_notify(conn, &hog_mouse_svc.attrs[HOG_INPUT_REPORT_VALUE_ATTR],
			      input_report, sizeof(input_report));
}

void hog_mouse_release_local_state(void)
{
	memset(input_report, 0, sizeof(input_report));
}
