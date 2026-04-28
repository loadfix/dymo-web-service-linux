#ifndef DYMO_XML_PARSE_H
#define DYMO_XML_PARSE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// --- Geometry -------------------------------------------------------------

typedef struct {
    double x, y, w, h;  // twips
} bounds_t;

typedef enum {
    ROT_0   = 0,
    ROT_90  = 90,
    ROT_180 = 180,
    ROT_270 = 270,
} rotation_t;

typedef enum {
    HALIGN_LEFT,
    HALIGN_CENTER,
    HALIGN_RIGHT,
} halign_t;

typedef enum {
    VALIGN_TOP,
    VALIGN_MIDDLE,
    VALIGN_BOTTOM,
} valign_t;

typedef enum {
    FIT_NONE,
    FIT_SHRINK,
} fit_mode_t;

// --- Text -----------------------------------------------------------------

typedef struct {
    char *text;
    char *family;
    double size_pt;
    bool bold;
    bool italic;
} text_segment_t;

typedef struct {
    char *name;
    bounds_t bounds;
    rotation_t rotation;
    halign_t h_align;
    valign_t v_align;
    fit_mode_t fit_mode;
    text_segment_t *segments;
    size_t n_segments;
} text_object_t;

// --- Barcode --------------------------------------------------------------

typedef enum {
    BC_CODE2OF5,
    BC_CODE128,
    BC_QR,
    BC_UNKNOWN,
} barcode_type_t;

typedef struct {
    char *name;
    bounds_t bounds;
    rotation_t rotation;
    halign_t h_align;
    barcode_type_t bc_type;
    char *text;
    char *size;           // "Small"/"Medium"/"Large" — informational
    char *text_position;  // "None"/"Bottom"/"Top"
} barcode_object_t;

// --- Object --------------------------------------------------------------

typedef enum {
    OBJ_TEXT,
    OBJ_BARCODE,
} object_kind_t;

typedef struct {
    object_kind_t kind;
    union {
        text_object_t text;
        barcode_object_t barcode;
    } u;
} label_object_t;

// --- Label ---------------------------------------------------------------

typedef struct {
    double paper_w_twips;  // already swapped for landscape by parse_label
    double paper_h_twips;
    char *orientation;     // "Landscape"/"Portrait"
    label_object_t *objects;
    size_t n_objects;
} label_t;

typedef struct {
    char *name;
    char *value;
} object_data_t;

typedef struct {
    object_data_t *fields;
    size_t n_fields;
} label_record_t;

typedef struct {
    label_record_t *records;
    size_t n_records;
} label_set_t;

// --- API -----------------------------------------------------------------

// Returns NULL on error. Caller frees via label_free.
label_t *parse_label_xml(const char *xml, size_t len);
void label_free(label_t *);

label_set_t *parse_label_set_xml(const char *xml, size_t len);
void label_set_free(label_set_t *);

// Lookup helper: return value for a given name in a record, or NULL.
const char *record_get(const label_record_t *rec, const char *name);

#endif
