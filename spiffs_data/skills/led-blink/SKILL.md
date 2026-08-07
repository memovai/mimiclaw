# LED Blink

Blink an LED on a policy-allowed GPIO pin a chosen number of times.

## Script
- entry: scripts/main.lua
- args: {"pin": {"type": "integer", "default": 38},
         "times": {"type": "integer", "default": 3, "min": 1, "max": 20},
         "interval_ms": {"type": "integer", "default": 300, "min": 20, "max": 2000}}
- run with: lua_run_script path=/spiffs/skills/led-blink/scripts/main.lua args={"pin":38,"times":3}

## Origin
packaged
