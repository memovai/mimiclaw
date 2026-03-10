# Skill Creator

Create new skills for MimiClaw.

## When to use
When the user asks to create a new skill, teach the bot something, or add a new capability.

## How to create a skill
1. Choose a short, descriptive name (lowercase, hyphens ok)
2. Write a SKILL.md file with this structure:
   - `# Title` — clear name
   - Brief description paragraph
   - `## When to use` — trigger conditions
   - `## How to use` — step-by-step instructions
   - `## Example` — concrete example (optional but helpful)
3. Save to `/spiffs/skills/<name>.md` using write_file
4. The skill will be automatically available after the next conversation

## Python-enabled skills
You can create skills that execute Python code using the `run_python` tool.
MicroPython capabilities: math, string processing, json, regex, list/dict ops, collections, struct, binascii, random.
MicroPython limitations: NO network, NO file I/O, NO hardware access, NO pip packages.
Use `print()` to return results — stdout is captured as the tool output.

Example: a skill could instruct the agent to use `run_python` for calculations,
data transformations, or text processing that would be unreliable via pure LLM reasoning.

## Best practices
- Keep skills concise — the context window is limited
- Focus on WHAT to do, not HOW (the agent is smart)
- Include specific tool calls the agent should use
- Test by asking the agent to use the new skill

## Example
To create a "translate" skill:
write_file path="/spiffs/skills/translate.md" content="# Translate\n\nTranslate text between languages.\n\n## When to use\nWhen the user asks to translate text.\n\n## How to use\n1. Identify source and target languages\n2. Translate directly using your language knowledge\n3. For specialized terms, use web_search to verify\n"
