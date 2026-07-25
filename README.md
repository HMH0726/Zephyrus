# Zephyrus

# Notes

<hr/>
<h3>CMD</h3>

west build -b xiao_ble/nrf52840 -p always
rmdir /s /q build && west build -b xiao_ble/nrf52840

git -C "%ZEPHYR_BASE%" reset --hard
git -C "%ZEPHYR_BASE%" clean -fd

west build -p always

<hr/>
<h3>QQQ</h3>

Zephyr 和nRF Connect SDK (NCS)能夠實現像MX Master滑鼠那樣3個頻道都配對到同一個手機的情況嗎?

但我這邊的情況比較特別，我會拿我的手機去配對多個頻道，當然也會有每個頻道不同設備的情況，另外需要手機端顯示不同的識別名稱

我目前使用XIAO nRF52840 Plus搭配Zephyr
我有辦法複製出mx master 滑鼠3個頻道都配對到我的手機上並且都可以反覆切換一樣的功能嗎？直接產出完整專案，另外不同頻道要不同名稱



我目前使用XIAO nRF52840 Plus，有辦法複製出mx master 滑鼠3個頻道都配對到我的手機上並且都可以反覆切換一樣的功能嗎？直接產出完整專案，另外不同頻道要不同名稱


頻道1使用板子原本的MAC、頻道2使用板子原本的MAC但是最後一位+1、頻道3使用板子原本的MAC但是最後一位+2


Seeed Studio XIAO nRF52840 Plus搭配Zephyr 或是nRF Connect SDK (NCS)能夠實現像MX Master滑鼠那樣3個頻道都配對到同一個手機的情況嗎?用法有差別嗎?



<hr/>