# Multi-Channel BLE Keyboard (XIAO nRF52840 + Zephyr)

Supports **N independent channels**, each backed by its own Bluetooth
local identity (`bt_id`). This is what lets the **same phone** be paired
to more than one channel simultaneously without the key-database
collision / `reason 22` disconnect you were hitting before — each
channel's bond is stored under its own identity, so there's no 1-IRK-to-
1-identity conflict.

## Files

- `CMakeLists.txt` - standard Zephyr app build file
- `prj.conf` - Kconfig options (BT, multi-identity, bonding persistence, HID)
- `app.overlay` / `boards/xiao_ble.overlay` - button (D2) + LED (D3) devicetree
- `src/main.c` - identity setup, advertising, button-triggered channel switch
- `src/hid_keyboard.c` / `.h` - minimal hand-rolled HID-over-GATT keyboard service

## Build

```bash
west build -b xiao_ble . --pristine
west flash
```

If your Zephyr/board version uses a different board target name (e.g.
`xiao_ble/nrf52840`, or you're on Seeed's own board-support fork), just
swap the `-b` argument accordingly.

## Key config knobs

- `NUM_CHANNELS` in `src/main.c` — how many channels/identities you want.
- `CONFIG_BT_ID_MAX` in `prj.conf` — must be **≥ NUM_CHANNELS** (identity 0
  is the default one).
- `CONFIG_BT_MAX_PAIRED` — total bond slots across *all* channels and *all*
  remote devices combined. If you're pairing 1 phone to 3 channels, that's
  already 3 bonds; pad this generously.
- Button/LED pins are in `app.overlay` — currently D2 (button) / D3 (LED),
  change the pin numbers there if your wiring differs.

## Hardware limit to keep in mind

The nRF52840 BLE controller's **resolving list** (for resolving remote
private addresses) has a small fixed hardware size — commonly on the
order of 8–16 entries depending on SoftDevice/controller build
(`CONFIG_BT_CTLR_RL_SIZE`). Each bonded device the controller needs to
resolve consumes one entry. Pairing the same phone to many channels is
fine in the tens, but don't scale `NUM_CHANNELS` into the dozens without
checking this.

## Factory reset without SWD tools (button-hold at boot)

If you're flashing via UF2 drag-and-drop (double-tap reset), you don't have
`nrfjprog`/`pyocd` available to fully erase flash. UF2 flashing only
overwrites the application region, never the dedicated storage/NVS
partition — so stale bonding data from a previous firmware can survive
across reflashes and cause `settings: set-value failure` errors on boot.

This firmware includes a built-in factory reset for exactly that case:

1. Hold the channel button (D2) and power on / reset the board.
2. Keep holding for ~3 seconds — the console will prompt you to keep holding.
3. Once erased, the board reboots automatically on its own.

This calls `flash_area_erase()` on the `storage_partition` and then does a
clean `sys_reboot()` immediately after, rather than continuing the same
boot cycle — NVS mounts and scans its write pointers very early (before
`main()` even runs), so erasing the partition mid-boot without rebooting
would leave those pointers pointing at now-blank flash instead of
resyncing. A fresh boot re-mounts NVS against the freshly erased sectors
cleanly.

## Things worth double-checking against your exact SDK version

A few Zephyr API names/macros can shift slightly between mainline Zephyr
and Nordic's nRF Connect SDK (which is Zephyr-based) release to release.
If the build errors on any of these, it's almost always a rename, not a
logic problem — check your installed version's `bluetooth/gap.h`,
`bluetooth/conn.h` and `bluetooth/uuid.h`:

- `BT_CONN_CB_DEFINE(...)` vs. older `bt_conn_cb_register(&conn_callbacks)`
- `BT_GAP_ADV_FAST_INT_MIN_2` / `_MAX_2`
- `bt_id_create(NULL, NULL)` signature (should be stable, but confirm)

## Extending it

- To actually send keystrokes, call `hid_keyboard_send_key(current_conn,
  modifier, keycode)` from wherever your real key-scanning logic lives
  (a matrix scan, another button, etc.) — it's already wired up in
  `hid_keyboard.c`.
- Add `CONFIG_BT_DIS_MODEL` / `CONFIG_BT_DIS_MANUF` in `prj.conf` if you
  want custom Device Information Service strings.
- If a picky host requires it, you can add a Protocol Mode characteristic
  to `hids_svc` — most hosts work fine without it for a basic keyboard.
