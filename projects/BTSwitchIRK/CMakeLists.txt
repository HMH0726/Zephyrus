cmake_minimum_required(VERSION 3.20.0)

# 尋找 Zephyr 作業系統核心
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

# 專案名稱
project(BTSwitchIRK)

# 指定主程式原始碼位置
target_sources(app PRIVATE src/main.c)