# XIAO nRF52840 - 3 頻道 BLE 滑鼠骨架 (MX Master 風格切換)

用 Zephyr / nRF Connect SDK (NCS) 的 **multiple Bluetooth identity** 機制，
在同一顆晶片上模擬 Logitech MX Master 的「3 個頻道各自配對、按鍵切換」行為。

## 核心邏輯

1. 讀取 nRF52840 出廠燒錄的 factory static address (FICR->DEVICEADDR)。
2. 衍生出 3 組位址：**只把「最後一個 byte」分別 +0 / +1 / +2**（不用隨機值），
   對應頻道 0 / 1 / 2。
3. 用 `bt_id_create()` 把這 3 組位址各自建立成獨立的 Bluetooth identity，
   讓手機端把它們視為 3 個不同裝置，各自完成配對與 bonding。
   → 這正好對應「同一支手機要佔用多個頻道」的情境：因為裝置端位址不同，
   手機藍牙清單裡會看到 3 個項目，各自獨立 bond，互不覆蓋。
4. 按下實體按鈕 → 停止目前廣播 → 若有連線就斷線 → 切到下一個 identity
   重新廣播 → 手機依 bond 記錄自動重新連上該頻道對應的裝置。

## 檔案結構

```
xiao_3ch_mouse/
├── CMakeLists.txt
├── prj.conf          # Kconfig：identity 數量、bonding、HIDS 等設定
├── app.overlay        # 切換按鈕的 GPIO 腳位（請依你實際接線修改）
└── src/
    └── main.c         # 主程式
```

## 建置方式

需要先裝好 west + nRF Connect SDK 或 upstream Zephyr 的開發環境。

```bash
# 在你的 NCS / Zephyr workspace 裡
west build -b xiao_ble path/to/xiao_3ch_mouse

# 若是 XIAO nRF52840 Sense 版本
west build -b xiao_ble/nrf52840/sense path/to/xiao_3ch_mouse
```

`app.overlay` 放在應用程式資料夾根目錄，Zephyr 建置系統會自動套用，
不需要額外參數。

## 燒錄

XIAO BLE 內建 UF2 bootloader，通常可以直接：

```bash
west flash
```

若你的環境沒有對應的 flash runner，也可以把 `build/zephyr/zephyr.uf2`
用雙擊 reset 進入 bootloader 模式後直接拖曳到出現的隨身碟裡。

## 接線

`app.overlay` 目前假設按鈕接在 `P0.02`，另一端接 GND。
**請依照你實際的接線腳位修改 `app.overlay` 裡的 `gpio0 2` 這兩個數字**。

## 重要提醒 / 已知待驗證項目

- **BLE identity / 位址衍生 / 廣播切換** 這部分是標準 Zephyr Bluetooth Host
  API（`bt_id_create`、`bt_le_adv_start` 搭配 `.id` 欄位切換 identity），
  這個做法與 ZMK 韌體處理多頻道切換的機制相同，邏輯上可以放心採用。
- **HIDS（HID over GATT）的欄位細節**（`bt_hids_init_param` 的成員名稱、
  `bt_hids_inp_rep_send` 用法等）在不同版本的 NCS / Zephyr 之間曾經調整過。
  我沒有在實機環境編譯驗證這部分，正式建置前，請對照你本機 SDK 版本裡的：
  - `zephyr/include/zephyr/bluetooth/services/hids.h`
  - `nrf/samples/bluetooth/peripheral_hids`（NCS 官方範例）

  如果編譯出現欄位名稱不符的錯誤，把錯誤訊息貼給我，我可以馬上幫你對應修正。
- 真正的滑鼠移動資料（來自光學感測器、滾輪等）不在此骨架範圍內，
  請在你自己的輸入處理程式碼裡呼叫 `main.c` 最下面的
  `mouse_send_report(buttons, dx, dy, wheel)` 即可送出一筆 HID report。
- 記得 `settings_load()` 之後 identity 只會建立一次（程式已處理），
  之後開機會沿用 flash 裡existing 的 3 組 bonding 資料，不會重複建立。
