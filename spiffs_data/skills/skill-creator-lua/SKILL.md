# Lua Skill Creator

Author a new skill for this device when the user asks for a capability that
does not yet exist. Use this whenever the request implies repeatable behavior,
computation, hardware sequencing, or scheduled/stateful logic.

## Origin
packaged

## When to use which tier
- **Prompt-only skill (L0)**: the task is pure instructions the model can
  follow with existing tools (e.g. "summarize the news each morning"). Write a
  single Markdown file `skills/<name>.md` with a title, a one-line description,
  and steps. No Lua.
- **Scripted skill (L2)**: the task needs a loop, computation, hardware
  timing, persistent state, or combining several tool calls deterministically
  (e.g. "blink an LED", "log temperature every hour and alert on a threshold").
  Write a Lua script.

## Procedure for a scripted skill
1. Decide the modules you need. Read only the API docs you will use:
   - `read_file /spiffs/lua-api/gpio.md` (hardware)
   - `read_file /spiffs/lua-api/storage.md` (persistent state)
   - `read_file /spiffs/lua-api/tool.md` (call other tools, incl. web_search)
   - `read_file /spiffs/lua-api/misc.md` (json, timer, log, args validation)
2. Write the script to `skills/<name>/scripts/main.lua`. Structure it as:
   - `arg_schema.parse(args, {...})` to validate inputs with defaults,
   - the body,
   - an explicit `return { ... }` of a small result table.
3. `lua_check` the script. If it reports an error with a line number, fix that
   line and check again. Do not run until it compiles.
4. `lua_run_script` it once with safe test arguments. For GPIO skills, test on
   an allowed pin (e.g. 38, the status LED). Confirm the return value.
5. Write `skills/<name>/SKILL.md` with:
   - a single `# Title` line,
   - a one-line description (this is what the model sees in the skill list),
   - a `## Script` section giving the entry path, the args schema, and an
     example `lua_run_script` invocation,
   - a `## Origin` section set to `generated`.
6. Confirm with `list_dir skills/<name>/` and tell the user the new skill and
   how to trigger it.

## Script template
```lua
-- <one line: what this does>
local ctx = arg_schema.parse(args, {
  -- name = arg_schema.int{ default = 0, min = 0, max = 100 },
})

-- body

return { ok = true }
```

## Rules
- Always `lua_check` before `lua_run_script`.
- Keep scripts short and single-purpose.
- Read an API doc before using a module; do not guess function names.
- Mark generated skills `## Origin: generated`.
