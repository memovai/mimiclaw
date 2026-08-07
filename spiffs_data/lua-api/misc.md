# json, timer, log, args — quick reference

## json
- `json.encode(value)` — Lua value to JSON string.
- `json.decode(str)` — JSON string to Lua value.

## timer
- `timer.sleep_ms(ms)` — block the script for `ms` (max 5000 per call);
  counts against the script timeout.
- `timer.now_ms()` — milliseconds since boot.

## log
- `log.info(msg)` / `log.warn(msg)` / `log.error(msg)` — write to the device
  log (tagged `lua.script`). `msg` must be a string.

## args (argument validation)
`args` is the global table of arguments passed to the script. Validate it:
```lua
local ctx = arg_schema.parse(args, {
  city  = arg_schema.str{ required = true },
  count = arg_schema.int{ default = 3, min = 1, max = 10 },
})
-- ctx.city, ctx.count are now validated with defaults applied
```
Builders: `arg_schema.int`, `arg_schema.num`, `arg_schema.bool`,
`arg_schema.str`, each taking `{ default=, min=, max=, required= }`.

## return value and output
- `return <value>` from the top level; it is sent back as JSON.
- `print(...)` output is captured and appended after the return value.
