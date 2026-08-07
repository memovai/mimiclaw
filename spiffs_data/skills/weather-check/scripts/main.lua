-- Fetch current weather for a city via web_search (network through tool.invoke).
local ctx = arg_schema.parse(args, {
  city = arg_schema.str{ required = true },
})

local result = tool.invoke("web_search", { query = "current weather " .. ctx.city })

return { city = ctx.city, report = result }
