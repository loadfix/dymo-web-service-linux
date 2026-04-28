#ifndef DYMO_HTTP_H
#define DYMO_HTTP_H

#include <stddef.h>

// URL-decode `in` (len bytes) into a freshly malloc'd NUL-terminated string.
// Returns NULL on OOM. '+' is decoded to ' ' per application/x-www-form-urlencoded.
char *url_decode(const char *in, size_t len);

// Iterate application/x-www-form-urlencoded body. For each key, if it matches
// `name` exactly, return the URL-decoded value (malloc'd). Returns NULL if not
// found. Caller frees.
char *form_get(const char *body, size_t body_len, const char *name);

#endif
