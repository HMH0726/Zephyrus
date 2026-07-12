@echo off
title nRF Connect SDK (NCS) 隔離開發環境
echo ===================================================
echo   啟動 Nordic NCS 可攜式開發環境 (Portable Edition)
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

:: 3. ?? 靈魂切換：將 Zephyr Base 指向新蓋好的 nRFConnectSDK
set ZEPHYR_BASE=%USB_ROOT%workspace\nRFConnectSDK\zephyr

echo [成功] 環境變數已注入！目前掛載於: %USB_ROOT%
echo [狀態] 目前啟動引擎：Nordic NCS v3.4.0
echo ===================================================

:: 繞過寫死的 exe 路徑，讓 west 變成完全可攜！
doskey west=python -m west $*

echo ===================================================

:: 恢復你的神級快捷鍵！
doskey newproject=python "%USB_ROOT%new_project.py"

:: 繞過寫死的 exe 路徑，讓 west 變成完全可攜！
doskey west=python -m west $*

:: 開啟終端機並直接停留在 projects 資料夾
cmd.exe /k "cd /d %USB_ROOT%projects"

:: 開啟終端機並直接停留在 projects 資料夾
cmd.exe /k "cd /d %USB_ROOT%projects"