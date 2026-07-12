/*
  hid_mouse.c
  - 基本 HIDS / HID over GATT 的初始化與一個簡單的 mouse report 送出函式
  - 這個實作是基於 Zephyr samples/bluetooth/hids_mouse 的精簡整合，用於示範與直接使用
*/

#include <zephyr.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/gatt.h>
#include <bluetooth/services/hids.h>
#include <logging/log.h>

LOG_MODULE_DECLARE(mx_mouse);

static struct bt_hids hids_obj;

/* HID input report map for a mouse: (buttons, x, y, wheel)
   3 bytes or 4 bytes report depending descriptor. 這裡使用 4-byte report:
   [buttons (1)] [x (1)] [y (1)] [wheel (1)]
   注意：如果你的 descriptor 不一致，需要對應調整。
*/

/* A minimal report map (binary) copied / simplified from typical mouse descriptor */
static const uint8_t report_map[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x02,       /* Usage (Mouse) */
    0xA1, 0x01,       /* Collection (Application) */
      0x09, 0x01,     /*   Usage (Pointer) */
      0xA1, 0x00,     /*   Collection (Physical) */
        0x05, 0x09,   /*     Usage Page (Buttons) */
        0x19, 0x01,   /*     Usage Minimum (01) */
        0x29, 0x03,   /*     Usage Maximum (03) */
        0x15, 0x00,   /*     Logical Minimum (0) */
        0x25, 0x01,   /*     Logical Maximum (1) */
        0x95, 0x03,   /*     Report Count (3) */
        0x75, 0x01,   /*     Report Size (1) */
        0x81, 0x02,   /*     Input (Data,Var,Abs) */
        0x95, 0x01,   /*     Report Count (1) */
        0x75, 0x05,   /*     Report Size (5) */
        0x81, 0x03,   /*     Input (Const,Var,Abs) -- padding */
        0x05, 0x01,   /*     Usage Page (Generic Desktop) */
        0x09, 0x30,   /*     Usage (X) */
        0x09, 0x31,   /*     Usage (Y) */
        0x09, 0x38,   /*     Usage (Wheel) */
        0x15, 0x81,   /*     Logical Minimum (-127) */
        0x25, 0x7F,   /*     Logical Maximum (127) */
        0x75, 0x08,   /*     Report Size (8) */
        0x95, 0x03,   /*     Report Count (3) */
        0x81, 0x06,   /*     Input (Data,Var,Rel) */
      0xC0,           /*   End Collection */
    0xC0              /* End Collection */
};

static struct bt_hids_init_param hids_param;

/* HID report characteristics setup: one input report for mouse */
static struct bt_hids_inp_rep mouse_input_rep = {
    .size = 4, /* 4 bytes: buttons, x, y, wheel */
    .id = 0x01,
};

void hid_mouse_init(void)
{
    int err;

    memset(&hids_param, 0, sizeof(hids_param));

    hids_param.rep_map = report_map;
    hids_param.rep_map_len = sizeof(report_map);

    /* Input report */
    hids_param.inp_rep_cnt = 1;
    hids_param.inp_rep = &mouse_input_rep;

    /* Device info (optional) */
    struct bt_hids_init_param device_param = {0};

    err = bt_hids_init(&hids_obj, &hids_param);
    if (err) {
        LOG_ERR("bt_hids_init failed: %d", err);
    } else {
        LOG_INF("HID Service initialized");
    }
}

/* Send mouse report: small wrapper */
/* dx, dy, wheel: -127..127
   buttons ignored for now; you can extend to set button bits.
*/
int hid_send_mouse_report(int8_t dx, int8_t dy, int8_t wheel)
{
    uint8_t report[4] = {0, 0, 0, 0};
    report[0] = 0; /* buttons */
    report[1] = (uint8_t)dx;
    report[2] = (uint8_t)dy;
    report[3] = (uint8_t)wheel;

    int err = bt_hids_inp_rep_send(&hids_obj, &mouse_input_rep, report, sizeof(report), 0);
    if (err) {
        LOG_DBG("hid_send error: %d", err);
    }
    return err;
}