# GPIO Control

Control and monitor GPIO pins on the ESP32-S3 for digital I/O.

## When to use
When the user asks to:
- Turn on/off LEDs, relays, or other outputs
- Check switch states, button presses, or sensor readings
- Confirm digital I/O status (switch confirmation)
- Get an overview of all GPIO pin states

## How to use
1. To **read a switch/sensor**: use gpio_read with the pin number
   - Returns HIGH (1) or LOW (0)
   - HIGH typically means switch is ON / circuit closed
   - LOW typically means switch is OFF / circuit open
2. To **set an output**: use gpio_write with pin and state (1=HIGH, 0=LOW)
3. To **scan all pins**: use gpio_read_all for a full status overview
4. For **switch confirmation**: read the pin, report state, optionally toggle and re-read to verify

## Pin safety
- Only pins within the allowed range can be accessed
- ESP32 flash pins (6-11) are always blocked
- If a pin is rejected, suggest an alternative within the allowed range

## Example
User: "Check if the switch on pin 4 is on"
→ gpio_read {"pin": 4}
→ "Pin 4 = HIGH"
→ "The switch on pin 4 is currently ON (HIGH)."

User: "Turn on the relay on pin 5"
→ gpio_write {"pin": 5, "state": 1}
→ "Pin 5 set to HIGH"
→ gpio_read {"pin": 5}
→ "Pin 5 = HIGH"
→ "Relay on pin 5 is now ON. Confirmed HIGH."
