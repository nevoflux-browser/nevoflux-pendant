#!/usr/bin/env python3
"""
Package a standalone Windows 10 flashing & provisioning bundle (`nevoflux-win10-flash-pack.zip`)
so the Windows 10 machine does NOT need Git, source code, or ESP-IDF installed.

Bundle contents:
  - merged-flash.bin    (Complete 8MB ESP32-S3 flash image, flash at offset 0x0)
  - flash_win10.bat     (One-click Windows batch script using esptool)
  - provision_win10.bat (One-click Windows batch script for Wi-Fi & NevoFlux NVS provisioning)
  - provision.py        (Standalone provisioning tool)
  - README-WIN10.txt    (Instructions for Espressif Flash Download Tool GUI & CLI)
"""

from __future__ import annotations

import pathlib
import zipfile

from build_flash_image import REPO_ROOT, build_merged_flash

FLASH_BAT = """@echo off
chcp 65001 >nul
echo ================================================================
echo   NevoFlux Voice Pendant - Windows 10 One-Click Flasher
echo   Target: Waveshare ESP32-S3-Touch-AMOLED-1.43C (8MB Flash)
echo ================================================================
echo.
set /p COM_PORT="请输入开发板串口号 (例如 COM3): "
if "%COM_PORT%"=="" (
    echo [错误] 串口号不能为空！请在设备管理器 -> 端口(COM和LPT) 中查看。
    pause
    exit /b 1
)

python -m esptool version >nul 2>&1
if errorlevel 1 (
    echo [提示] 正在自动安装 esptool...
    pip install esptool pyserial
)

echo.
echo [开始烧录] 正在将 merged-flash.bin 写入 %COM_PORT% (地址 0x000000)...
python -m esptool --chip esp32s3 -p %COM_PORT% -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB 0x000000 merged-flash.bin

if errorlevel 1 (
    echo.
    echo [烧录失败] 请检查：
    echo   1. 串口号 %COM_PORT% 是否正确且未被其他串口助手占用
    echo   2. 按住板子上的 BOOT 键不放 -> 短按一下 RESET 键 -> 松开 BOOT 键，然后重试
    pause
    exit /b 1
)

echo.
echo [成功] 8MB 完整固件已烧录完成！
pause
"""

PROVISION_BAT = """@echo off
chcp 65001 >nul
echo ================================================================
echo   NevoFlux Voice Pendant - Windows 10 Wi-Fi 与配对密钥写入工具
echo ================================================================
echo.
set /p COM_PORT="1. 请输入开发板串口号 (例如 COM3): "
set /p WIFI_SSID="2. 请输入要连接的 Wi-Fi 名称 (SSID): "
set /p WIFI_PASS="3. 请输入 Wi-Fi 密码: "
set /p PAIR_CODE="4. 请输入桌面端 /pair-device 生成的配对码: "

python -c "import serial, argon2" >nul 2>&1
if errorlevel 1 (
    echo [提示] 正在安装 pyserial 与 argon2-cffi...
    pip install pyserial argon2-cffi
)

echo.
python provision.py --port %COM_PORT% --ssid "%WIFI_SSID%" --password "%WIFI_PASS%" --code "%PAIR_CODE%"
pause
"""

README_WIN10 = """========================================================================
NevoFlux Voice Pendant · Windows 10 免源码烧录说明
适用硬件：Waveshare ESP32-S3-Touch-AMOLED-1.43C (8MB Flash / 8MB PSRAM)
========================================================================

本压缩包已包含完整的 8MB 合并固件 (merged-flash.bin)，在 Windows 10 上无需安装
Git、无需拉取源代码、也无需安装庞大的 ESP-IDF。

你可以任选以下两种方式之一进行烧录：

------------------------------------------------------------------------
方法 A：使用乐鑫官方图形化工具烧录（纯鼠标操作，无需安装 Python）
------------------------------------------------------------------------
1. 下载并解压乐鑫官方 Flash Download Tool：
   https://www.espressif.com.cn/zh-hans/support/download/other-tools
2. 双击打开 flash_download_tool.exe，弹窗选择：
   - Chip Type:  ESP32-S3
   - WorkMode:   Develop
   - LoadMode:   UART
3. 在主界面第一行：
   - 勾选左侧复选框 [v]
   - 选择本目录下的 `merged-flash.bin`
   - 右侧地址栏填写：`0x0`（或 `0x000000`）
4. 下方参数设置：
   - SPI SPEED:  80MHz
   - SPI MODE:   DIO
   - COM:        选择开发板对应的 COM 口（如 COM3）
   - BAUD:       460800（或 921600）
5. 点击【START】开始烧录，等待进度条变成绿色【FINISH】，按一下板子上的 RESET 键即可启动！

------------------------------------------------------------------------
方法 B：使用双击脚本烧录（适合已安装 Python 3 的 Windows 10）
------------------------------------------------------------------------
1. 双击运行本目录下的 `flash_win10.bat`
2. 根据提示输入串口号（例如 `COM3`）并回车，脚本会自动将 `merged-flash.bin` 烧录到 0x0。
3. 如需通过串口写入 Wi-Fi 和 NevoFlux 桌面端配对码，双击运行 `provision_win10.bat`，
   并在屏幕提示后 15 秒内按一下板子上的 BOOT 键确认。

------------------------------------------------------------------------
附：或者直接在浏览器里免安装任何软件烧录（ESPLaunchpad / WebSerial）
------------------------------------------------------------------------
1. 在 Windows 10 上用 Chrome 或 Edge 浏览器打开：
   https://espressif.github.io/esp-launchpad/
   （或者 https://esp.huhn.me/ ）
2. 点击 Connect 连接开发板 COM 口，上传 `merged-flash.bin`（地址填 0x0）即可直接在网页里烧录！
"""


def main() -> int:
    build_dir = REPO_ROOT / "build"
    merged_bin = build_dir / "esp-emu" / "merged-flash.bin"
    build_merged_flash(merged_bin, boot_partition="factory")

    zip_path = build_dir / "nevoflux-win10-flash-pack.zip"
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.write(merged_bin, arcname="merged-flash.bin")
        zf.writestr("flash_win10.bat", FLASH_BAT.replace("\n", "\r\n"))
        zf.writestr("provision_win10.bat", PROVISION_BAT.replace("\n", "\r\n"))
        zf.write(REPO_ROOT / "tools" / "provision" / "provision.py", arcname="provision.py")
        zf.writestr("README-WIN10.txt", README_WIN10.replace("\n", "\r\n"))

    print(f"[OK] Generated Windows 10 standalone flash pack -> {zip_path} ({zip_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
