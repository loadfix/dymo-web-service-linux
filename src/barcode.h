#ifndef DYMO_BARCODE_H
#define DYMO_BARCODE_H

#include <stddef.h>

// Width lists use module counts (1 = narrow, 3 = wide for ITF).
// Alternation: index 0 is bar (black), index 1 is space (white), ...
typedef struct {
    int *widths;
    size_t n;
} bar_widths_t;

// Returned buffer must be freed with free(bw.widths).
bar_widths_t itf_pattern(const char *digits);
bar_widths_t code128_pattern(const char *text);

#endif
