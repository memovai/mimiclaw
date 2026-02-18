#include "telegram/md_to_html.h"
#include <string.h>
#include <stdbool.h>

/*
 * Minimal Markdown → Telegram HTML converter.
 *
 * Telegram HTML subset:
 *   <b>bold</b>  <i>italic</i>  <s>strikethrough</s>
 *   <code>inline</code>  <pre>block</pre>
 *   <a href="url">text</a>
 *
 * Only < > & need HTML-escaping; quotes inside attributes are safe
 * because we control the output.
 */

/* Append a string, return bytes that WOULD have been written */
static size_t emit(char *buf, size_t pos, size_t cap, const char *s, size_t n)
{
    if (pos + n < cap) {
        memcpy(buf + pos, s, n);
    }
    return n;
}

#define EMIT_STR(s) do { out += emit(html, out, size, (s), strlen(s)); } while(0)
#define EMIT_N(s,n) do { out += emit(html, out, size, (s), (n)); } while(0)

/* Emit a single char, HTML-escaped if needed */
static size_t emit_char_escaped(char *buf, size_t pos, size_t cap, char c)
{
    switch (c) {
    case '<':  return emit(buf, pos, cap, "&lt;", 4);
    case '>':  return emit(buf, pos, cap, "&gt;", 4);
    case '&':  return emit(buf, pos, cap, "&amp;", 5);
    default:   return emit(buf, pos, cap, &c, 1);
    }
}

#define EMIT_ESC(c) do { out += emit_char_escaped(html, out, size, (c)); } while(0)

size_t md_to_telegram_html(const char *md, char *html, size_t size)
{
    if (!md || !html || size == 0) return 0;

    size_t out = 0;
    const char *p = md;
    const size_t len = strlen(md);
    const char *end = md + len;

    bool in_bold = false;
    bool in_italic = false;
    bool in_strike = false;
    bool in_code_block = false;

    while (p < end) {
        /* ── Fenced code block: ``` ── */
        if (!in_code_block && p + 3 <= end && p[0] == '`' && p[1] == '`' && p[2] == '`') {
            p += 3;
            /* Skip optional language tag until newline */
            while (p < end && *p != '\n') p++;
            if (p < end) p++; /* skip the \n */
            EMIT_STR("<pre>");
            in_code_block = true;
            continue;
        }
        if (in_code_block && p + 3 <= end && p[0] == '`' && p[1] == '`' && p[2] == '`') {
            p += 3;
            /* Skip trailing newline if present */
            if (p < end && *p == '\n') p++;
            EMIT_STR("</pre>");
            in_code_block = false;
            continue;
        }
        if (in_code_block) {
            EMIT_ESC(*p);
            p++;
            continue;
        }

        /* ── Inline code: `...` ── */
        if (*p == '`') {
            const char *q = memchr(p + 1, '`', end - p - 1);
            if (q) {
                EMIT_STR("<code>");
                for (const char *c = p + 1; c < q; c++) {
                    EMIT_ESC(*c);
                }
                EMIT_STR("</code>");
                p = q + 1;
                continue;
            }
        }

        /* ── Link: [text](url) ── */
        if (*p == '[') {
            const char *close_bracket = NULL;
            /* Find matching ] — don't cross newlines */
            for (const char *q = p + 1; q < end && *q != '\n'; q++) {
                if (*q == ']') { close_bracket = q; break; }
            }
            if (close_bracket && close_bracket + 1 < end && close_bracket[1] == '(') {
                const char *close_paren = memchr(close_bracket + 2, ')', end - close_bracket - 2);
                if (close_paren) {
                    size_t url_len = close_paren - (close_bracket + 2);
                    EMIT_STR("<a href=\"");
                    EMIT_N(close_bracket + 2, url_len);
                    EMIT_STR("\">");
                    /* Emit link text with escaping */
                    for (const char *c = p + 1; c < close_bracket; c++) {
                        EMIT_ESC(*c);
                    }
                    EMIT_STR("</a>");
                    p = close_paren + 1;
                    continue;
                }
            }
        }

        /* ── Strikethrough: ~~ ── */
        if (p + 1 < end && p[0] == '~' && p[1] == '~') {
            if (in_strike) {
                EMIT_STR("</s>");
            } else {
                EMIT_STR("<s>");
            }
            in_strike = !in_strike;
            p += 2;
            continue;
        }

        /* ── Bold: ** ── */
        if (p + 1 < end && p[0] == '*' && p[1] == '*') {
            if (in_bold) {
                EMIT_STR("</b>");
            } else {
                EMIT_STR("<b>");
            }
            in_bold = !in_bold;
            p += 2;
            continue;
        }

        /* ── Italic: single * (not **) ── */
        if (*p == '*' && !(p + 1 < end && p[1] == '*')) {
            if (in_italic) {
                EMIT_STR("</i>");
            } else {
                EMIT_STR("<i>");
            }
            in_italic = !in_italic;
            p += 1;
            continue;
        }

        /* ── Bold with __  ── */
        if (p + 1 < end && p[0] == '_' && p[1] == '_') {
            if (in_bold) {
                EMIT_STR("</b>");
            } else {
                EMIT_STR("<b>");
            }
            in_bold = !in_bold;
            p += 2;
            continue;
        }

        /* ── Default: emit escaped character ── */
        EMIT_ESC(*p);
        p++;
    }

    /* Close any unclosed tags */
    if (in_code_block) EMIT_STR("</pre>");
    if (in_strike) EMIT_STR("</s>");
    if (in_bold) EMIT_STR("</b>");
    if (in_italic) EMIT_STR("</i>");

    /* NUL-terminate */
    if (out < size) {
        html[out] = '\0';
    } else {
        html[size - 1] = '\0';
    }

    return out;
}
