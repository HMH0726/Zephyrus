# MX Master-style 3-channel BLE HID mouse for XIAO nRF52840 Plus

這個 Zephyr app 讓一片 XIAO nRF52840 Plus 以 BLE HID mouse 的方式運作，並模擬 MX Master 的 3 個 Easy-Switch 頻道：

- CH1 廣播名稱：`MX Master CH1`
- CH2 廣播名稱：`MX Master CH2`
- CH3 廣播名稱：`MX Master CH3`

三個頻道使用三個不同的 Zephyr Bluetooth identity，因此手機會把它們當成三個獨立的 BLE 滑鼠裝置。你可以把同一支手機分別配對到 CH1/CH2/CH3，之後按 D0 或 shell 指令切換，韌體會斷開目前連線並用下一個 identity 重新廣播。

## 目錄

```text
mx_master_three_channels/
├─ CMakeLists.txt
├─ prj.conf
├─ app.overlay
├─ src/
│  ├─ main.c
│  ├─ hog_mouse.c
│  └─ hog_mouse.h
└─ README.md
```

## 接線

`app.overlay` 預設以 XIAO nRF52840 常見 D pin 對應設定 GPIO。所有按鍵都是「按下接 GND」，內部 pull-up。

| 功能 | XIAO pin | nRF52840 GPIO |
| --- | --- | --- |
| 下一個頻道 | D0 | P0.02 |
| 左鍵 | D1 | P0.03 |
| 右鍵 | D2 | P0.28 |
| 中鍵 | D3 | P0.29 |
| 滾輪上 | D4 | P0.04 |
| 滾輪下 | D5 | P0.05 |

如果你的 XIAO nRF52840 Plus board package 腳位命名不同，只要改 `app.overlay` 裡面的 `gpios = <&gpio0 ...>` 即可。

## Build / Flash

在你的 portable Zephyr v4.4.0 開發環境中：

```powershell
cd D:\Users\ORESANJO\Downloads\Zephyrus\projects\mx_master_three_channels
west build -b xiao_ble/nrf52840 .
west flash
```

如果你的 Plus 板子在環境裡有自己的 board target，請把 `xiao_ble/nrf52840` 換成你的 target，例如：

```powershell
west build -b <your_xiao_nrf52840_plus_board> .
```

## 配對流程

1. 燒錄後預設在 CH1，手機搜尋並配對 `MX Master CH1`。
2. 按 D0，或在 shell 輸入 `mx channel 2`，手機搜尋並配對 `MX Master CH2`。
3. 再按 D0，或輸入 `mx channel 3`，手機搜尋並配對 `MX Master CH3`。
4. 之後按 D0 會依序切換 CH1 -> CH2 -> CH3 -> CH1。

手機端會看到三個名字不同的 HID mouse。切換頻道時目前連線會被斷開，新的頻道會開始廣播並讓手機重新連上。

## Shell 指令

如果你的 console/shell 可用，可以用：

```text
mx status
mx channel 1
mx channel 2
mx channel 3
mx next
mx move 20 0
mx move 0 0 1
mx click left
mx unpair 1
mx unpair all
```

`mx unpair all` 會清掉三個 identity 的 bond。手機端也建議同步「忘記裝置」，避免手機保存舊的 bond/key。

## 注意

- 這份專案重點是三頻道 BLE HID identity、bond 與切換邏輯。
- 目前內建的是 HID mouse report：5 個按鍵、X/Y 相對移動、垂直滾輪、水平 pan。若要接光學感測器或更完整的 MX Master 按鍵，可以在 `send_mouse_report()` 前面接入你的 sensor/按鍵資料。
- 同一時間只會有一個頻道 active，這和 MX Master 的使用模式相同。
