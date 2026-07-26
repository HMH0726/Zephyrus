# PMP_MAIN
cmake_minimum_required(VERSION 3.20.0)

# --- Find patch (Dual Boot) ---
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
# ------------------------------------

# 尋找 Zephyr 核心
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(PMP)

# 指定主程式原始碼位置
target_sources(app PRIVATE src/main.c)
generate_inc_file_for_target(app src/index.html ${ZEPHYR_BINARY_DIR}/include/generated/index.html.inc)