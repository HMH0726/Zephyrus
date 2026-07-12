@echo off
title Zephyr OS 隔離開發環境 (純血開源版)

:: 取得隨身碟/資料夾當前路徑
set USB_ROOT=%~dp0

:: 1. 掛載免安裝工具
set PATH=%USB_ROOT%tools\python;%USB_ROOT%tools\python\Scripts;%PATH%
set PATH=%USB_ROOT%tools\cmake\bin;%PATH%
set PATH=%USB_ROOT%tools\ninja;%PATH%
set PATH=%USB_ROOT%tools\git\cmd;%PATH%
set PATH=%USB_ROOT%tools\zephyr-sdk\hosttools\bin;%PATH%

:: 2. 設定 Zephyr SDK 與核心路徑
set ZEPHYR_SDK_INSTALL_DIR=%USB_ROOT%tools\zephyr-sdk
set ZEPHYR_TOOLCHAIN_VARIANT=zephyr
set ZEPHYR_BASE=%USB_ROOT%workspace\zephyrproject\zephyr

echo ===================================================
echo    啟動 Zephyr 可攜式開發環境 (Portable Edition)
echo ===================================================
echo [成功] 環境變數已注入！

:: ===================================================
:: 第一步：確保核心是乾淨的原廠狀態 (全域洗淨)
:: ===================================================
echo [狀態] 正在將 Zephyr 核心重置為原廠狀態 - 確保純淨環境...
"%USB_ROOT%tools\git\cmd\git.exe" -C "%ZEPHYR_BASE%" reset --hard >nul 2>&1


:: ===================================================
:: 第二步：詢問要切換到哪個專案或是創建專案
:: ===================================================
:ASK_PROJECT
echo.
echo ===================================================
echo    [ 現有專案列表 ]
dir /b /ad "%USB_ROOT%projects"
echo ===================================================
echo.

:: 清空變數，防止直接按 Enter 導致繼承上次的錯誤值
set "USER_CHOICE="
set /p USER_CHOICE="請輸入要切換的專案名稱 (或輸入 NEWPROJECT 建立新專案): "

:: 防呆 1：如果什麼都沒輸入直接按 Enter，就重新詢問
if "%USER_CHOICE%"=="" goto ASK_PROJECT

:: 防呆 2：自動濾除使用者不小心輸入的空白鍵
set USER_CHOICE=%USER_CHOICE: =%

:: 處理新建專案 (輸入 NEWPROJECT，不分大小寫)
if /I "%USER_CHOICE%"=="NEWPROJECT" (
    echo.
    echo [狀態] 啟動新專案建立精靈...
    python "%USB_ROOT%new_project.py"
    echo.
    echo [狀態] 建立程序結束。請重新選擇您要進入的專案。
    goto ASK_PROJECT
)

:: 處理防呆 3：檢查使用者輸入的專案是否存在
if not exist "%USB_ROOT%projects\%USER_CHOICE%\" (
    echo.
    echo [錯誤] 找不到名為 "%USER_CHOICE%" 的專案，請確認名稱是否正確！
    goto ASK_PROJECT
)


:: ===================================================
:: 第三步：自動連環套用 patches 資料夾內的補丁
:: ===================================================
cd /d "%USB_ROOT%projects\%USER_CHOICE%"
echo.
echo ===================================================
echo [狀態] 已成功切換至專案目錄: %USER_CHOICE%

:: 檢查專案底下是否有 patches 資料夾，且裡面有 .patch 檔案
if exist "patches\*.patch" (
    echo [狀態] 偵測到 patches 資料夾，開始依序套用補丁...
    for %%f in (patches\*.patch) do (
        echo [補丁] 正在套用: %%~nxf
        "%USB_ROOT%tools\git\cmd\git.exe" -C "%ZEPHYR_BASE%" apply --ignore-whitespace "%CD%\%%f"
    )
) else (
    echo [狀態] 專案無 patches 資料夾或無補丁檔案，維持純淨原廠核心。
)
echo ===================================================

:: ===================================================
:: 第四步：載入神級快捷鍵 (避開互動選單防衝突)
:: ===================================================
doskey newproject=python "%USB_ROOT%new_project.py"
doskey west=python -m west $*

:: 任務結束，維持視窗開啟並停留在目標專案
cmd.exe /k