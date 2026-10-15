#pragma once

#include <stddef.h>

/* Minimal JSON string escaper.
 *
 * Diagnostic event texts are free-form: they can contain a `"`, a backslash or
 * a control character (a stray `\n`/`\r` from a UART error string is enough).
 * Interpolating such a text straight into `"text":"%s"` produces invalid JSON,
 * which the browser then rejects with "bad control character in string literal".
 *
 * This helper has snprintf-like semantics: it always NUL-terminates `dst` (when
 * dst_size > 0) and returns the number of bytes the escaped result needs,
 * excluding the terminator. A return value >= dst_size means the result was
 * truncated, so the caller can fall back to a safe placeholder.
 *
 * `"` and `\` become `\"` / `\\`, the short escapes \n \r \t \b \f are used
 * where they exist, and every other control byte < 0x20 becomes `\u00XX`. All
 * other bytes are copied verbatim (UTF-8 is passed through unchanged).
 */
size_t json_escape(const char *src, char *dst, size_t dst_size);
