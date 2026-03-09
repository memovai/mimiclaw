# Tavily Web Search Configuration Guide

<p align="center">
  <img src="images/tavily-og.png" alt="Tavily - Search Engine for AI Agents" width="600" />
</p>

[Tavily](https://tavily.com) is a real-time search engine designed for AI agents and RAG workflows. MimiClaw uses Tavily as the **preferred** web search provider (with Brave Search as a fallback).

## Table of Contents

- [Overview](#overview)
- [Getting an API Key](#getting-an-api-key)
- [Configuring MimiClaw](#configuring-mimiclaw)
- [How It Works in MimiClaw](#how-it-works-in-mimiclaw)
- [API Details](#api-details)
- [Pricing](#pricing)
- [Rate Limits](#rate-limits)
- [Troubleshooting](#troubleshooting)

## Overview

MimiClaw's `web_search` tool allows the AI agent to search the internet for real-time information. When a Tavily API key is configured, MimiClaw will use Tavily as the search provider. If no Tavily key is set but a Brave Search key exists, it falls back to Brave Search.

**Why Tavily?**

- Purpose-built for AI agents — returns clean, structured results
- Includes relevance scoring for each result
- Supports advanced search depth for higher quality results
- 1,000 free API credits per month (no credit card required)

## Getting an API Key

1. **Sign up** at [app.tavily.com](https://app.tavily.com/home)
2. **Create an account** — you can sign in with Google or email
3. **Copy your API key** — it starts with `tvly-` and is available on the dashboard immediately
4. The free **Researcher** plan gives you **1,000 API credits per month** with no credit card required

Your API key will look like: `tvly-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx`

## Configuring MimiClaw

There are two ways to configure the Tavily API key:

### Option 1: Build-time Configuration (Recommended for First Setup)

1. Copy the secrets template if you haven't already:

```bash
cp main/mimi_secrets.h.example main/mimi_secrets.h
```

2. Edit `main/mimi_secrets.h` and set your Tavily key:

```c
#define MIMI_SECRET_TAVILY_KEY      "tvly-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
```

3. Rebuild the firmware:

```bash
idf.py fullclean && idf.py build
```

4. Flash to your ESP32-S3:

```bash
idf.py -p PORT flash monitor
```

### Option 2: Runtime Configuration via Serial CLI

Connect to the UART (COM) port and run:

```
mimi> set_tavily_key tvly-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
```

This saves the key to NVS flash and takes effect immediately — no rebuild needed. Runtime values override build-time defaults.

### Verify Configuration

Check that the key is set:

```
mimi> config_show
```

You should see `tavily_key: tvly-****` (masked) in the output.

## How It Works in MimiClaw

When the AI agent needs to search the web, it calls the `web_search` tool. MimiClaw selects the search provider with this priority:

```
1. Tavily (if MIMI_SECRET_TAVILY_KEY is set)  <-- preferred
2. Brave Search (if MIMI_SECRET_SEARCH_KEY is set)
3. No search available (tool returns error)
```

The search flow:

```
User asks a question requiring web info
        |
        v
Agent decides to call web_search tool
        |
        v
MimiClaw sends POST to https://api.tavily.com/search
  - Authorization: Bearer tvly-xxxxx
  - Body: {"query": "...", "max_results": 5, "search_depth": "basic"}
        |
        v
Tavily returns structured results
        |
        v
Agent incorporates results into response
```

Each `basic` search costs **1 API credit**. With the free plan's 1,000 credits/month, that's about 33 searches per day.

## API Details

MimiClaw uses the Tavily Search API endpoint:

| Parameter | Value |
|-----------|-------|
| **Endpoint** | `POST https://api.tavily.com/search` |
| **Auth** | `Authorization: Bearer <API_KEY>` |
| **Content-Type** | `application/json` |

### Request Body (as sent by MimiClaw)

| Field | Type | Value | Description |
|-------|------|-------|-------------|
| `query` | string | (from agent) | The search query |
| `max_results` | integer | `5` | Number of results to return |
| `search_depth` | string | `"basic"` | Search depth (`basic` = 1 credit) |

### Response Fields

| Field | Type | Description |
|-------|------|-------------|
| `query` | string | The executed search query |
| `results` | array | Ranked search results |
| `results[].title` | string | Result title |
| `results[].url` | string | Source URL |
| `results[].content` | string | Summary snippet |
| `results[].score` | float | Relevance score (0-1) |
| `response_time` | float | Execution time in seconds |

### HTTP Status Codes

| Code | Meaning |
|------|---------|
| 200 | Success |
| 400 | Bad Request — invalid parameters |
| 401 | Unauthorized — missing or invalid API key |
| 429 | Rate Limit — too many requests |
| 432 | Plan Limit — monthly quota exceeded |
| 500 | Server Error |

## Pricing

Tavily offers a generous free tier and several paid plans:

| Plan | Monthly Credits | Cost | Per-Credit Rate |
|------|-----------------|------|-----------------|
| **Researcher** | 1,000 | **Free** | -- |
| Project | 4,000 | $30 | $0.0075 |
| Bootstrap | 15,000 | $100 | $0.0067 |
| Startup | 38,000 | $220 | $0.0058 |
| Growth | 100,000 | $500 | $0.005 |
| Pay-as-you-go | Variable | $0.008/credit | $0.008 |

**Credit costs per search:**

- `basic` search depth: **1 credit**
- `advanced` search depth: **2 credits**

MimiClaw uses `basic` by default, so each search = 1 credit.

## Rate Limits

| Environment | Limit |
|-------------|-------|
| Development | 100 requests per minute |
| Production | 1,000 requests per minute |

For an ESP32-based agent, you will never hit these limits in practice.

## Troubleshooting

### "No search provider configured"

The agent returns this when neither Tavily nor Brave Search keys are set.

**Fix:** Set a Tavily API key via `set_tavily_key` CLI command or in `mimi_secrets.h`.

### Search returns empty results

- Verify your API key is correct: `config_show` should show `tavily_key: tvly-****`
- Check your monthly credit balance at [app.tavily.com](https://app.tavily.com/home)
- Ensure the ESP32 has internet access: `wifi_status`

### HTTP 401 Unauthorized

Your API key is invalid or expired. Generate a new one at [app.tavily.com](https://app.tavily.com/home).

### HTTP 432 Plan Limit

You've exceeded your monthly credit quota. Wait for the next billing cycle or upgrade your plan.

### Search works but results are poor

Tavily's `basic` search depth is optimized for speed. The quality is generally sufficient for an AI agent's needs. If you need higher quality, you can modify `SEARCH_RESULT_COUNT` in `tool_web_search.c`.

## References

- [Tavily Documentation](https://docs.tavily.com)
- [Tavily API Reference](https://docs.tavily.com/documentation/api-reference/endpoint/search)
- [Tavily API Credits](https://docs.tavily.com/documentation/api-credits)
- [Tavily Rate Limits](https://docs.tavily.com/documentation/rate-limits)
- [Tavily Playground](https://app.tavily.com/playground)
