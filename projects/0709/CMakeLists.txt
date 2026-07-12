cmake_minimum_required(VERSION 3.20.0)

# --- 【神級外掛】在編譯前自動將雙系統/同手機放行補丁打入 Zephyr ---
find_program(PATCH_EXECUTABLE patch)
if(PATCH_EXECUTABLE)
    execute_process(
        COMMAND ${PATCH_EXECUTABLE} -p1 -N -i ${CMAKE_CURRENT_SOURCE_DIR}/dual_boot_fix.patch
        WORKING_DIRECTORY $ENV{ZEPHYR_BASE}
        RESULT_VARIABLE patch_result
        ERROR_QUIET
    )
    message(STATUS "Dual-Boot Patch Status: ${patch_result}")
endif()
# -----------------------------------------------------------

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(xiao_mouse)

target_sources(app PRIVATE src/main.c)