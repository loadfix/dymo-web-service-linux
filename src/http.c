#include "http.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *url_decode(const char *in, size_t len) {
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == '+') out[o++] = ' ';
        else if (c == '%' && i + 2 < len) {
            int h1 = hexval((unsigned char)in[i + 1]);
            int h2 = hexval((unsigned char)in[i + 2]);
            if (h1 >= 0 && h2 >= 0) {
                out[o++] = (char)((h1 << 4) | h2);
                i += 2;
            } else {
                out[o++] = c;
            }
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
    return out;
}

char *form_get(const char *body, size_t body_len, const char *name) {
    if (!body || !name) return NULL;
    size_t nlen = strlen(name);
    size_t i = 0;
    while (i < body_len) {
        // Find '=' and next '&'
        size_t k_start = i;
        while (i < body_len && body[i] != '=' && body[i] != '&') i++;
        size_t k_end = i;
        size_t v_start = k_end;
        size_t v_end = k_end;
        if (i < body_len && body[i] == '=') {
            v_start = i + 1;
            i++;
            while (i < body_len && body[i] != '&') i++;
            v_end = i;
        }
        if (i < body_len && body[i] == '&') i++;

        if (k_end - k_start == nlen && memcmp(body + k_start, name, nlen) == 0) {
            return url_decode(body + v_start, v_end - v_start);
        }
    }
    return NULL;
}
