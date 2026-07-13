# Light Control

Control the device light.

## When to use
When the user asks to set, change, test, dim, brighten, or turn off the light. Treat `灯`, `小灯`, `彩灯`, `light`, and `lamp` as this device light.

## How to use
For status/notification intent, prefer `light_signal`.
For direct color control, use `rgb_led_set`, not `gpio_write`.
When the user speaks Chinese, translate colors and effects to English constants in the tool call.

## Semantic signals
Use `light_signal {"signal":"..."}` when the user describes what the light should mean, not the raw animation.

| Signal | Meaning | Light behavior |
| --- | --- | --- |
| `idle` / `ready` / `online` / `空闲` / `就绪` / `在线` / `默认` | Device is running normally; this is the boot default | Warm-white breathing, 5% brightness |
| `thinking` / `processing` / `思考` / `处理中` | The agent is thinking or waiting for an LLM/tool result | Cool-white breathing |
| `tool` / `working` / `工具` / `工具执行` / `工作中` | A local tool or hardware action is running | Cyan pulse |
| `success` / `done` / `ok` / `成功` / `完成` | Operation completed successfully | Green pulse |
| `warning` / `warn` / `警告` / `注意` | Recoverable problem or user attention needed | Yellow blink |
| `error` / `failed` / `fail` / `错误` / `失败` | Failed operation or serious error | Red fast blink |
| `urgent` / `紧急` | High-priority alert | Red heartbeat |
| `message` / `notification` / `消息` / `通知` | Incoming message or ordinary notification | Blue pulse |
| `important` / `重要` | Important but not urgent notification | Purple breathing |
| `offline` / `离线` / `断网` / `网络异常` | Wi-Fi/network/API path is unhealthy | Yellow fast blink |
| `telegram_offline` / `telegram异常` / `telegram断开` | Telegram channel is unhealthy | Blue fast blink |
| `find_me` / `找我` / `你在哪` / `定位` | Make the board easy to find | Rainbow |
| `sleep` / `night` / `睡觉` / `夜间` | Low-disturbance night state | Dim warm white, 5% brightness |
| `off` / `关灯` / `关闭` | Turn the controllable RGB light off | Off |

Examples:
- Default/idle: `light_signal {"signal":"idle"}`
- Thinking: `light_signal {"signal":"thinking"}`
- Success: `light_signal {"signal":"success"}`
- Error: `light_signal {"signal":"error"}`
- Find me: `light_signal {"signal":"find_me"}`
- Override brightness: `light_signal {"signal":"urgent","brightness_percent":25}`

Examples:
- Red: `rgb_led_set {"color":"red"}`
- Off: `rgb_led_set {"color":"off"}`
- On: `rgb_led_set {"color":"on"}`
- Custom color: `rgb_led_set {"r":32,"g":0,"b":255}`
- Hex color: `rgb_led_set {"hex":"#ff00aa"}`
- Dim blue: `rgb_led_set {"color":"blue","brightness":32}`
- Percent brightness: `rgb_led_set {"brightness_percent":30}`
- Brighter/dimmer: `rgb_led_set {"delta_brightness":32}` or `rgb_led_set {"delta_brightness":-32}`
- Blink: `rgb_led_effect {"effect":"blink","color":"red","speed_ms":500}`
- Red-blue alternating: `rgb_led_effect {"effect":"alternate","color":"red","color2":"blue","speed_ms":350}`
- Breathe: `rgb_led_effect {"effect":"breathe","color":"warm_white","speed_ms":80}`
- Fade between two colors: `rgb_led_effect {"effect":"fade","color":"red","color2":"blue","speed_ms":80}`
- Rainbow: `rgb_led_effect {"effect":"rainbow","brightness_percent":20,"speed_ms":80}`
- Pulse: `rgb_led_effect {"effect":"pulse","color":"cyan","speed_ms":100}`
- Heartbeat: `rgb_led_effect {"effect":"heartbeat","color":"red","speed_ms":120}`
- Sparkle: `rgb_led_effect {"effect":"sparkle","color":"white","speed_ms":80}`
- Confetti: `rgb_led_effect {"effect":"confetti","brightness_percent":20,"speed_ms":120}`
- Confetti Chinese names: 彩纸, 彩色纸屑, 纸屑, 彩点, 随机彩点. Use `effect:"confetti"` in the tool call.
- Police light: `rgb_led_effect {"effect":"police","brightness_percent":30,"speed_ms":180}`
- Stop effect: `rgb_led_effect {"effect":"stop"}`
- Status: `rgb_led_status {}`

- Common colors: `red`, `green`, `blue`, `white`, `cool_white`, `warm_white`, `yellow`, `cyan`, `purple`, `violet`, `orange`, `pink`, `rose`, `teal`, `off`, `on`
- Chinese colors: `红/红色`, `绿/绿色`, `蓝/蓝色`, `白/白色`, `冷白`, `暖白`, `黄/黄色`, `青/青色`, `紫/紫色`, `紫罗兰`, `橙/橙色`, `粉/粉色`, `玫红`, `蓝绿`, `关/关灯`, `开/开灯`

## Board note
This physical board's light uses GPIO48. Do not change it to GPIO38 based only on generic ESP32-S3-DevKitC-1 docs or pinout images; GPIO48 was verified by visible onboard LED behavior.
