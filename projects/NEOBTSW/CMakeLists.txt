cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mx_mouse_multi_channel)

target_sources(app PRIVATE src/main.c src/hid_mouse.c)