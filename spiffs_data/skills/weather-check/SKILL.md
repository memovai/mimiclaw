# Weather Check

Fetch current weather for a city via web search, inside a single script.
Demonstrates a scripted skill that reaches the network through tool.invoke.

## Script
- entry: scripts/main.lua
- args: {"city": {"type": "string", "required": true}}
- run with: lua_run_script path=/spiffs/skills/weather-check/scripts/main.lua args={"city":"Manchester"}

## Origin
packaged
