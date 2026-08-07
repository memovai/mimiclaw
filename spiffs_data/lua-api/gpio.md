# gpio module

Control GPIO pins. Pins are validated against the firmware allowlist; an
attempt to use a disallowed pin raises an error and aborts the script.

## Functions
- `gpio.write(pin, level)` — set pin HIGH (level=1) or LOW (level=0). No return.
- `gpio.read(pin)` — return the pin level as 0 or 1.

## Example
```lua
gpio.write(38, 1)          -- pin 38 HIGH
local v = gpio.read(5)     -- read pin 5
```

## Notes
- `pin` and `level` are integers.
- Errors are fatal to the script; do not wrap in pcall unless you intend to
  handle a policy rejection.
