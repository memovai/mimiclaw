# tool module

Call a registered firmware tool from a script and get its text result. This
is how a script reaches capabilities that live in C — including the network
(`web_search`) — without holding a socket itself.

## Functions
- `tool.invoke(name, args)` — run tool `name` with the table `args` (encoded
  to JSON internally). Returns the tool's result as a string. Raises an error
  if the tool fails.

## Example
```lua
local res = tool.invoke("web_search", { query = "weather Manchester today" })
-- res is the tool's text output; parse what you need out of it
```

## Notes
- `args` is a Lua table; it is converted to JSON for the tool.
- Available tools include: web_search, get_current_time, read_file,
  write_file, list_dir, cron_add/cron_list/cron_remove, gpio_* .
- The result is text; use string operations or ask for structured fields.
