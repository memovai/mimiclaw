# Weather

Get current weather and forecasts using web_search.

## When to use
When the user asks about weather, temperature, or forecasts.

## How to use
1. Use get_current_time to know the current date
2. Use web_search with a query like "weather in [city] today"
3. Extract temperature, conditions, and forecast from results
4. Present in a concise, friendly format

## Example
User: "What's the weather in Tokyo?"
→ get_current_time
→ web_search "weather Tokyo today February 2026"
→ "Tokyo: 8°C, partly cloudy. High 12°C, low 4°C. Light wind from the north."
