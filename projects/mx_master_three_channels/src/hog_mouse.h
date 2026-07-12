#ifndef HOG_MOUSE_H_
#define HOG_MOUSE_H_

#include <stdint.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/sys/util.h>

#define HOG_MOUSE_BTN_LEFT    BIT(0)
#define HOG_MOUSE_BTN_RIGHT   BIT(1)
#define HOG_MOUSE_BTN_MIDDLE  BIT(2)
#define HOG_MOUSE_BTN_BACK    BIT(3)
#define HOG_MOUSE_BTN_FORWARD BIT(4)

int hog_mouse_init(void);
int hog_mouse_send(struct bt_conn *conn, uint8_t buttons,
		   int8_t x, int8_t y, int8_t wheel, int8_t pan);
void hog_mouse_release_local_state(void);

#endif /* HOG_MOUSE_H_ */
