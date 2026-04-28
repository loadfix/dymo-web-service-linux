#ifndef DYMO_PRINTING_H
#define DYMO_PRINTING_H

#include <stdbool.h>

// Rotate+resize the PNG to match the label media dimensions, then fork/exec
// `lp` with the same CUPS options the Python reference uses:
//   -o media=w72h154
//   -o DymoHalftoning=NLL
//   -o DymoPrintQuality=Graphics
//   -o DymoPrintDensity=Dark
//   -o fit-to-page
//
// `printer_name` is the CUPS destination (e.g. "LabelWriter-450-Turbo"). If
// NULL, "LabelWriter-450-Turbo" is used.
//
// Returns 0 on success, -1 on error.
int print_label_png(const char *png_path, const char *printer_name);

#endif
