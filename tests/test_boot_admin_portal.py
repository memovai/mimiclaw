#!/usr/bin/env python3
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MIMI = (ROOT / "main" / "mimi.c").read_text()
CONFIG = (ROOT / "main" / "mimi_config.h").read_text()
ONBOARD = (ROOT / "main" / "onboard" / "wifi_onboard.c").read_text()


def macro_value(name):
    match = re.search(rf"^#define\s+{name}\s+(.+)$", CONFIG, re.M)
    assert match, f"missing {name}"
    return match.group(1).strip()


def main():
    app_main = MIMI.split("void app_main(void)", 1)[1]

    assert "wifi_onboard_start(WIFI_ONBOARD_MODE_CAPTIVE)" in app_main
    assert "wifi_onboard_start(WIFI_ONBOARD_MODE_ADMIN)" not in app_main

    assert macro_value("MIMI_BOOT_BUTTON_LONG_PRESS_MS") == "3000"
    assert macro_value("MIMI_ONBOARD_ADMIN_TIMEOUT_MS") == "(10 * 60 * 1000)"
    assert "level == 0 && press_active && !long_press_handled" in MIMI
    assert "last == 0 && level == 1 && press_active" in MIMI
    assert "wifi_manager_is_connected()" in MIMI

    assert "ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MIMI_ONBOARD_ADMIN_TIMEOUT_MS))" in ONBOARD
    assert "xTaskNotifyGive(s_admin_timeout_task)" in ONBOARD
    assert "httpd_stop(s_server)" in ONBOARD
    assert "esp_wifi_set_mode(WIFI_MODE_STA)" in ONBOARD
    assert ONBOARD.count("esp_restart();") >= 3
    assert "if (!captive && s_admin_active)" in ONBOARD


if __name__ == "__main__":
    main()
