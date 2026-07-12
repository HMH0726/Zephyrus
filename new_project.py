import os
import sys

def create_project():
    print("===================================================")
    print("  🚀 Zephyr 專案快速產生器 (Portable Edition)")
    print("===================================================")
    
    # 自動抓取腳本所在位置(根目錄)，並定位到 projects 資料夾
    root_dir = os.path.dirname(os.path.abspath(__file__))
    projects_dir = os.path.join(root_dir, "projects")
    
    # 確保 projects 資料夾存在
    if not os.path.exists(projects_dir):
        os.makedirs(projects_dir)

    # 1. 詢問專案名稱
    proj_name = input("👉 請輸入新專案名稱 (例如 MyNewKeyboard): ").strip()

    if not proj_name:
        print("❌ 錯誤：專案名稱不能為空！")
        return

    # 計算新專案的完整路徑
    target_dir = os.path.join(projects_dir, proj_name)

    if os.path.exists(target_dir):
        print(f"❌ 錯誤：資料夾 '{proj_name}' 已經存在！")
        return

    # 2. 建立資料夾結構
    os.makedirs(os.path.join(target_dir, "src"))

    # 3. 產生 CMakeLists.txt
    with open(os.path.join(target_dir, "CMakeLists.txt"), "w", encoding="utf-8") as f:
        f.write(f"""cmake_minimum_required(VERSION 3.20.0)

# 尋找 Zephyr 核心
find_package(Zephyr REQUIRED HINTS $ENV{{ZEPHYR_BASE}})

project({proj_name})

# 指定主程式原始碼位置
target_sources(app PRIVATE src/main.c)
""")

    # 4. 產生 prj.conf (給一個乾淨的基礎藍牙設定)
    with open(os.path.join(target_dir, "prj.conf"), "w", encoding="utf-8") as f:
        f.write(f"""# ==========================================
# {proj_name} 基礎設定
# ==========================================
CONFIG_BT=y
CONFIG_BT_PERIPHERAL=y
CONFIG_BT_DEVICE_NAME="{proj_name}"

# 開啟 GPIO 驅動
CONFIG_GPIO=y
""")

    # 5. 產生 app.overlay (基礎按鈕範本)
    with open(os.path.join(target_dir, "app.overlay"), "w", encoding="utf-8") as f:
        f.write("""/ {
    aliases {
        sw0 = &button0;
    };

    buttons {
        compatible = "gpio-keys";
        button0: button_0 {
            /* 預設綁定到 XIAO 的 D1 腳位 (P0.03) */
            gpios = <&gpio0 3 (GPIO_PULL_UP | GPIO_ACTIVE_LOW)>;
            label = "User Button";
        };
    };
};
""")

    # 6. 產生 src/main.c
    with open(os.path.join(target_dir, "src/main.c"), "w", encoding="utf-8") as f:
        f.write(f"""#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void) {{
    printk("Hello World! 歡迎來到 {proj_name} 的世界！\\n");

    /* RTOS 主迴圈：每秒醒來一次，其他時間進入深度睡眠省電 */
    while (1) {{
        k_sleep(K_MSEC(1000));
    }}
    return 0;
}}
""")

    print(f"\n✅ 專案 [{proj_name}] 建立成功！")
    print("===================================================")
    print(f"下一步：")
    print(f"1. 輸入 'cd {proj_name}' 進入專案資料夾")
    print(f"2. 輸入 'west build -b xiao_ble/nrf52840' 直接開始編譯")
    print("===================================================")

if __name__ == "__main__":
    create_project()