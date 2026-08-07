-- Blink an LED on a policy-allowed pin.
-- args: pin (int), times (int), interval_ms (int)

local ctx = arg_schema.parse(args, {
  pin         = arg_schema.int{ default = 38 },
  times       = arg_schema.int{ default = 3,   min = 1,  max = 20 },
  interval_ms = arg_schema.int{ default = 300, min = 20, max = 2000 },
})

for i = 1, ctx.times do
  gpio.write(ctx.pin, 1)
  timer.sleep_ms(ctx.interval_ms)
  gpio.write(ctx.pin, 0)
  timer.sleep_ms(ctx.interval_ms)
end

return { pin = ctx.pin, blinks = ctx.times }
