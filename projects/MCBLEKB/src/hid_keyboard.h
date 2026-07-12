#ifndef HID_KEYBOARD_H_
#define HID_KEYBOARD_H_

#include <zephyr/bluetooth/conn.h>
#include <stdint.h>

/* Call once at boot, after bt_enable(). Currently just resets internal state,
 * but kept as a hook so you can extend it later (e.g. load a saved LED
 * output state).
 */
void hid_keyboard_init(void);

/* Call from your BT disconnected callback so the service stops trying to
 * notify a report on a link that no longer exists.
 */
void hid_keyboard_notify_disconnected(void);

/* Send one 8-byte HID boot-keyboard report:
 *   modifier : bitfield, bit0=LCtrl bit1=LShift bit2=LAlt bit3=LGUI
 *              bit4=RCtrl bit5=RShift bit6=RAlt bit7=RGUI
 *   keycodes : up to 6 simultaneous USB HID keycodes (0 = no key)
 *
 * Returns 0 on success, -ENOTCONN if not connected / notifications not
 * enabled (host hasn't written CCC yet), or a negative bt_gatt_notify() error.
 */
int hid_keyboard_send_report(struct bt_conn *conn, uint8_t modifier,
			      const uint8_t keycodes[6]);

/* Convenience: send a single keycode, then immediately send an all-zero
 * "key released" report. Useful for a simple button-triggers-one-keystroke
 * setup.
 */
int hid_keyboard_send_key(struct bt_conn *conn, uint8_t modifier, uint8_t keycode);

#endif /* HID_KEYBOARD_H_ */
