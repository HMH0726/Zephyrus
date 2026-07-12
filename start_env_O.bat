@echo off
title Zephyr OS 隔離開發環境 (純血開源版)
echo ===================================================
echo    啟動 Zephyr 可攜式開發環境 (Portable Edition)
echo ===================================================

:: 取得隨身碟/資料夾當前路徑
set USB_ROOT=%~dp0

:: 1. 掛載免安裝工具
set PATH=%USB_ROOT%tools\python;%USB_ROOT%tools\python\Scripts;%PATH%
set PATH=%USB_ROOT%tools\cmake\bin;%PATH%
set PATH=%USB_ROOT%tools\ninja;%PATH%
set PATH=%USB_ROOT%tools\git\cmd;%PATH%
:: 喚醒 DTC 裝置樹編譯器
set PATH=%USB_ROOT%tools\zephyr-sdk\hosttools\bin;%PATH%

:: 2. 設定 Zephyr SDK 路徑
set ZEPHYR_SDK_INSTALL_DIR=%USB_ROOT%tools\zephyr-sdk
set ZEPHYR_TOOLCHAIN_VARIANT=zephyr

:: 3. 關鍵導航：明確告訴編譯器 Zephyr 作業系統的核心在哪裡
set ZEPHYR_BASE=%USB_ROOT%workspace\zephyrproject\zephyr

echo [成功] 環境變數已注入！目前掛載於: %USB_ROOT%
echo [狀態] 目前啟動引擎：純血 Zephyr
echo ===================================================

:: 繞過寫死的 exe 路徑，讓 west 變成完全可攜！
doskey west=python -m west $*

echo ===================================================

:: 恢復你的神級快捷鍵！
doskey newproject=python "%USB_ROOT%new_project.py"

:: 繞過寫死的 exe 路徑，讓 west 變成完全可攜！
doskey west=python -m west $*

:: ===================================================
:: 【終極補丁引擎】自動套用雙系統補丁 (具備網頁空白/Tab免疫抗體)
:: ===================================================
if exist "%USB_ROOT%projects\0709\dual_boot_fix.patch" (
    echo [補丁] 正在透過 Git 自動套用雙系統補丁 - 忽略排版差異...
    "%USB_ROOT%tools\git\cmd\git.exe" -C "%ZEPHYR_BASE%" apply --ignore-whitespace "%USB_ROOT%projects\0709\dual_boot_fix.patch"
)
:: ===================================================

:: 開啟終端機並直接停留在 projects 資料夾
cmd.exe /k "cd /d %USB_ROOT%projects"