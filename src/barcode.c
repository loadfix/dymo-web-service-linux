// Interleaved-2-of-5 and Code 128 encoders. Byte-identical to the Python
// reference in ../../dymo_service/render.py — same tables, same checksum, same
// bar-space alternation.

#include "barcode.h"

#include <stdlib.h>
#include <string.h>

static int *widths_new(size_t cap) {
    return malloc(cap * sizeof(int));
}

// ITF encoding: each digit expands to 5 elements, narrow (n=1) or wide (w=3).
static const char *ITF_TABLE[10] = {
    "nnwwn",  // 0
    "wnnnw",  // 1
    "nwnnw",  // 2
    "wwnnn",  // 3
    "nnwnw",  // 4
    "wnwnn",  // 5
    "nwwnn",  // 6
    "nnnww",  // 7
    "wnnwn",  // 8
    "nwnwn",  // 9
};

bar_widths_t itf_pattern(const char *digits) {
    size_t n = digits ? strlen(digits) : 0;
    // Worst-case width count: 4 start + 10 per digit pair + 3 stop.
    size_t pair_count = (n + 1) / 2;
    size_t cap = 4 + pair_count * 10 + 3;
    bar_widths_t out = { .widths = widths_new(cap), .n = 0 };
    if (!out.widths) return out;

    // Start: narrow bar, narrow space, narrow bar, narrow space.
    out.widths[out.n++] = 1;
    out.widths[out.n++] = 1;
    out.widths[out.n++] = 1;
    out.widths[out.n++] = 1;

    for (size_t i = 0; i + 1 < n; i += 2) {
        int d1 = digits[i] - '0';
        int d2 = digits[i + 1] - '0';
        if (d1 < 0 || d1 > 9 || d2 < 0 || d2 > 9) continue;
        const char *a = ITF_TABLE[d1];
        const char *b = ITF_TABLE[d2];
        for (int k = 0; k < 5; k++) {
            out.widths[out.n++] = (a[k] == 'w') ? 3 : 1;
            out.widths[out.n++] = (b[k] == 'w') ? 3 : 1;
        }
    }

    // Stop: wide bar, narrow space, narrow bar.
    out.widths[out.n++] = 3;
    out.widths[out.n++] = 1;
    out.widths[out.n++] = 1;

    return out;
}

// Code 128 patterns, one 6-element string per value 0..105. Value 106 is the
// stop code which is 7 modules — stored verbatim. Matches the Python table.
static const char *C128_PATTERNS[] = {
    "212222","222122","222221","121223","121322","131222","122213","122312","132212","221213",
    "221312","231212","112232","122132","122231","113222","123122","123221","223211","221132",
    "221231","213212","223112","312131","311222","321122","321221","312212","322112","322211",
    "212123","212321","232121","111323","131123","131321","112313","132113","132311","211313",
    "231113","231311","112133","112331","132131","113123","113321","133121","313121","211331",
    "231131","213113","213311","213131","311123","311321","331121","312113","312311","332111",
    "314111","221411","431111","111224","111422","121124","121421","141122","141221","112214",
    "112412","122114","122411","142112","142211","241211","221114","413111","241112","134111",
    "111242","121142","121241","114212","124112","124211","411212","421112","421211","212141",
    "214121","412121","111143","111341","131141","114113","114311","411113","411311","113141",
    "114131","311141","411131","211412","211214","211232","2331112",
};
#define C128_VALUES 107
#define C128_START_B 104
#define C128_STOP    106

bar_widths_t code128_pattern(const char *text) {
    size_t n = text ? strlen(text) : 0;

    // Build the value list: START_B, per-char values, checksum, STOP.
    size_t val_cap = n + 3;
    int *values = malloc(val_cap * sizeof(int));
    if (!values) return (bar_widths_t){0};
    size_t vn = 0;
    values[vn++] = C128_START_B;
    for (size_t i = 0; i < n; i++) {
        // Code B covers ASCII 0x20..0x7E (values 0..94). Any byte outside
        // this range (control chars, UTF-8 multi-byte leads/continuations)
        // would index past the end of C128_PATTERNS[107] below — so reject
        // the whole barcode input rather than silently corrupting output.
        unsigned char b = (unsigned char)text[i];
        if (b < 0x20 || b > 0x7E) {
            free(values);
            return (bar_widths_t){0};
        }
        values[vn++] = (int)b - 32;
    }

    // Checksum = (start + Σ i*v_i) mod 103, where i starts at 1 for first data symbol.
    long checksum = values[0];
    for (size_t i = 1; i < vn; i++) {
        checksum += (long)i * values[i];
    }
    checksum %= 103;
    values[vn++] = (int)checksum;
    values[vn++] = C128_STOP;

    // Count total elements across all value patterns (mostly 6, stop is 7).
    // Defence in depth: any value outside [0, 106] would index OOB.
    enum { C128_TABLE_N = sizeof(C128_PATTERNS) / sizeof(C128_PATTERNS[0]) };
    size_t total = 0;
    for (size_t i = 0; i < vn; i++) {
        if (values[i] < 0 || values[i] >= (int)C128_TABLE_N) {
            free(values);
            return (bar_widths_t){0};
        }
        total += strlen(C128_PATTERNS[values[i]]);
    }
    bar_widths_t out = { .widths = widths_new(total), .n = 0 };
    if (!out.widths) { free(values); return out; }
    for (size_t i = 0; i < vn; i++) {
        const char *p = C128_PATTERNS[values[i]];
        for (const char *c = p; *c; c++) {
            out.widths[out.n++] = *c - '0';
        }
    }
    free(values);
    return out;
}
