#include "json_escape.h"

#include <stdio.h>

size_t json_escape(const char *src, char *dst, size_t dst_size)
{
    if (src == NULL) {
        src = "";
    }
    size_t out = 0;
    for (const char *p = src; *p != '\0'; ++p) {
        unsigned char c = (unsigned char)*p;
        char buf[8];
        const char *chunk;
        size_t chunk_len;

        switch (c) {
        case '"':
            chunk = "\\\"";
            break;
        case '\\':
            chunk = "\\\\";
            break;
        case '\n':
            chunk = "\\n";
            break;
        case '\r':
            chunk = "\\r";
            break;
        case '\t':
            chunk = "\\t";
            break;
        case '\b':
            chunk = "\\b";
            break;
        case '\f':
            chunk = "\\f";
            break;
        default:
            if (c < 0x20) {
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                chunk = buf;
            } else {
                buf[0] = (char)c;
                buf[1] = '\0';
                chunk = buf;
            }
            break;
        }
        chunk_len = 0;
        while (chunk[chunk_len] != '\0') {
            if (out + 1 < dst_size) {
                dst[out] = chunk[chunk_len];
            }
            ++out;
            ++chunk_len;
        }
    }
    if (dst_size > 0) {
        size_t end = out < dst_size ? out : dst_size - 1;
        dst[end] = '\0';
    }
    return out;
}
