#pragma once
#include <stddef.h>

/**
 * Convert Markdown text to Telegram-compatible HTML.
 *
 * Handles: **bold**, *italic*, `inline code`, ```code blocks```,
 * ~~strikethrough~~, [text](url) links.
 * Escapes <, >, & for HTML safety.
 *
 * @param md     Input markdown string
 * @param html   Output buffer for HTML
 * @param size   Size of output buffer
 * @return       Number of bytes written (excluding NUL)
 */
size_t md_to_telegram_html(const char *md, char *html, size_t size);
