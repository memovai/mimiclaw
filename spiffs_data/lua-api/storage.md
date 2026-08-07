# storage module

Read and write files on SPIFFS. All paths must start with `/spiffs/`.
Use this for skill state that must persist across runs (the interpreter keeps
no state between executions).

## Functions
- `storage.read(path)` — return file contents as a string, or `nil, err` if
  the file cannot be opened. Files larger than 32 KB raise an error.
- `storage.write(path, content)` — overwrite the file. Returns `true`, or
  `nil, err`.
- `storage.append(path, content)` — append to the file. Returns `true`, or
  `nil, err`.
- `storage.exists(path)` — return `true`/`false`.
- `storage.list(prefix)` — return an array of file names (relative to the
  mount, e.g. `"skills/foo.md"`) optionally filtered by `prefix`.

## Example
```lua
local n = tonumber(storage.read("/spiffs/counter.txt") or "0")
storage.write("/spiffs/counter.txt", tostring(n + 1))
```
