#ifndef DYMO_RENDER_H
#define DYMO_RENDER_H

#include "xml_parse.h"

// Render each LabelRecord to a 1-bit PNG at 300 DPI and append the file path
// (malloc'd, caller-owned) to out_paths. The PNG is written to out_dir.
//
// Returns 0 on success, -1 on error. *n_out is set to the number of PNGs
// produced. On success, out_paths is a calloc'd array of malloc'd strings;
// caller frees each string and the array.
int render_all(const label_t *label,
               const label_set_t *set,
               const char *out_dir,
               char ***out_paths,
               size_t *n_out);

#define DYMO_DPI 300
#define DYMO_TWIPS_PER_INCH 1440

#endif
