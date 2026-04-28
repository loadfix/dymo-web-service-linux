// Label / LabelSet XML parser using expat.
//
// The input XML is small (typically <50KB) and comes from a trusted local
// origin (the DYMO framework). We use expat's streaming parser but buffer
// character data until an end tag closes a known element. Memory discipline:
// all strings in the output tree are strdup'd from expat's buffers and owned
// by the label_t / label_set_t.

#include "xml_parse.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <expat.h>

// ---- small utilities -----------------------------------------------------

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

static void free_safe(void *p) { if (p) free(p); }

static const char *attr_get(const XML_Char **attrs, const char *name) {
    for (int i = 0; attrs[i]; i += 2) {
        if (strcmp(attrs[i], name) == 0) return attrs[i + 1];
    }
    return NULL;
}

static double attr_double(const XML_Char **attrs, const char *name, double defv) {
    const char *v = attr_get(attrs, name);
    return v ? atof(v) : defv;
}

static char *rstrip_copy(const char *s) {
    if (!s) return xstrdup("");
    size_t n = strlen(s);
    // Trim leading + trailing ASCII whitespace for element text nodes where
    // we treat them as attribute-like values (Rotation, Name, alignments...)
    size_t i = 0;
    while (i < n && isspace((unsigned char)s[i])) i++;
    size_t j = n;
    while (j > i && isspace((unsigned char)s[j - 1])) j--;
    size_t m = j - i;
    char *p = malloc(m + 1);
    if (!p) return NULL;
    memcpy(p, s + i, m);
    p[m] = '\0';
    return p;
}

// ---- parser state --------------------------------------------------------

typedef enum {
    MODE_LABEL,
    MODE_LABEL_SET,
} parse_mode_t;

// Growable char buffer for accumulating CharacterData between start/end tags.
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} cbuf_t;

static void cbuf_reset(cbuf_t *b) {
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

static void cbuf_append(cbuf_t *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 128;
        while (ncap < b->len + n + 1) ncap *= 2;
        char *np = realloc(b->data, ncap);
        if (!np) return;
        b->data = np;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void cbuf_free(cbuf_t *b) { free_safe(b->data); b->data = NULL; b->cap = b->len = 0; }

// Dynamic arrays for object/segment/record/field lists. The push function
// returns true on success and false on OOM. Callers that transfer ownership
// of malloc'd fields (e.g. text_segment_t.text) MUST free those fields when
// push fails, or memory leaks.
#define DEF_VEC(NAME, ITEM_T) \
    typedef struct { ITEM_T *data; size_t len, cap; } vec_##NAME##_t; \
    __attribute__((warn_unused_result)) \
    static bool vec_##NAME##_push(vec_##NAME##_t *v, ITEM_T item) { \
        if (v->len + 1 > v->cap) { \
            size_t nc = v->cap ? v->cap * 2 : 8; \
            ITEM_T *np = realloc(v->data, nc * sizeof(ITEM_T)); \
            if (!np) return false; \
            v->data = np; v->cap = nc; \
        } \
        v->data[v->len++] = item; \
        return true; \
    }

DEF_VEC(obj, label_object_t)
DEF_VEC(seg, text_segment_t)
DEF_VEC(rec, label_record_t)
DEF_VEC(fld, object_data_t)

// We process tags with a small state stack so sibling tags inside an
// ObjectInfo or LabelRecord don't get confused with tags at other levels.
typedef struct state_s {
    parse_mode_t mode;

    // Accumulator for CharacterData between tags.
    cbuf_t cdata;

    // Whether we're inside certain containers (bitmap of bool flags).
    bool in_object_info;
    bool in_text_object;
    bool in_barcode_object;
    bool in_styled_text_element;
    bool in_label_record;
    bool in_object_data;
    bool in_draw_commands;

    // Current in-flight text object (accumulating segments).
    text_object_t cur_text;
    vec_seg_t cur_text_segments;
    text_segment_t cur_segment;  // filled as we see <Element><String> + Font attrs

    // Current in-flight barcode object.
    barcode_object_t cur_barcode;

    // Current Bounds — found inside ObjectInfo *sibling* to the Text/Barcode
    // object. We capture it when we see <Bounds …/> and attach when the
    // object is closed.
    bounds_t cur_bounds;
    bool cur_bounds_set;

    // While ObjectInfo holds the object, the object is committed to this
    // list when </ObjectInfo> closes.
    vec_obj_t objects;

    // LabelSet parsing: records + current record's fields.
    vec_rec_t records;
    vec_fld_t cur_fields;

    // LabelSet parsing: the ObjectData we're currently inside.
    char *cur_od_name;

    // Label paper rect (first RoundRectangle).
    double paper_w_twips;
    double paper_h_twips;
    bool paper_set;

    // Label orientation.
    char *orientation;
} state_t;

static void state_init(state_t *s, parse_mode_t mode) {
    memset(s, 0, sizeof(*s));
    s->mode = mode;
    s->paper_w_twips = 1440;
    s->paper_h_twips = 3060;
}

// ---- element handlers ----------------------------------------------------

static rotation_t rot_from_str(const char *s) {
    if (!s) return ROT_0;
    if (strcmp(s, "Rotation90") == 0) return ROT_90;
    if (strcmp(s, "Rotation180") == 0) return ROT_180;
    if (strcmp(s, "Rotation270") == 0) return ROT_270;
    return ROT_0;
}

static halign_t halign_from_str(const char *s) {
    if (!s) return HALIGN_LEFT;
    if (strcmp(s, "Center") == 0) return HALIGN_CENTER;
    if (strcmp(s, "Right")  == 0) return HALIGN_RIGHT;
    return HALIGN_LEFT;
}

static valign_t valign_from_str(const char *s) {
    if (!s) return VALIGN_TOP;
    if (strcmp(s, "Middle") == 0) return VALIGN_MIDDLE;
    if (strcmp(s, "Bottom") == 0) return VALIGN_BOTTOM;
    return VALIGN_TOP;
}

static fit_mode_t fit_from_str(const char *s) {
    if (s && strcmp(s, "ShrinkToFit") == 0) return FIT_SHRINK;
    return FIT_NONE;
}

static barcode_type_t bc_type_from_str(const char *s) {
    if (!s) return BC_CODE128;
    if (strcmp(s, "Code2of5") == 0) return BC_CODE2OF5;
    if (strncmp(s, "Code128", 7) == 0) return BC_CODE128;
    // QRCode, QR, etc.
    if (strncasecmp(s, "qr", 2) == 0) return BC_QR;
    return BC_CODE128;
}

static void start_elem(void *user, const XML_Char *name, const XML_Char **attrs) {
    state_t *s = user;
    cbuf_reset(&s->cdata);

    if (s->mode == MODE_LABEL) {
        if (strcmp(name, "DrawCommands") == 0) {
            s->in_draw_commands = true;
            return;
        }
        if (strcmp(name, "RoundRectangle") == 0 && s->in_draw_commands && !s->paper_set) {
            s->paper_w_twips = attr_double(attrs, "Width", 1440);
            s->paper_h_twips = attr_double(attrs, "Height", 3060);
            s->paper_set = true;
            return;
        }
        if (strcmp(name, "ObjectInfo") == 0) {
            s->in_object_info = true;
            memset(&s->cur_text, 0, sizeof(s->cur_text));
            memset(&s->cur_barcode, 0, sizeof(s->cur_barcode));
            memset(&s->cur_text_segments, 0, sizeof(s->cur_text_segments));
            s->cur_bounds_set = false;
            return;
        }
        if (strcmp(name, "Bounds") == 0 && s->in_object_info) {
            s->cur_bounds.x = attr_double(attrs, "X", 0);
            s->cur_bounds.y = attr_double(attrs, "Y", 0);
            s->cur_bounds.w = attr_double(attrs, "Width", 0);
            s->cur_bounds.h = attr_double(attrs, "Height", 0);
            s->cur_bounds_set = true;
            return;
        }
        if (strcmp(name, "TextObject") == 0 && s->in_object_info) {
            s->in_text_object = true;
            return;
        }
        if (strcmp(name, "BarcodeObject") == 0 && s->in_object_info) {
            s->in_barcode_object = true;
            return;
        }
        if (s->in_text_object && strcmp(name, "Element") == 0) {
            s->in_styled_text_element = true;
            memset(&s->cur_segment, 0, sizeof(s->cur_segment));
            s->cur_segment.family = xstrdup("Arial");
            s->cur_segment.size_pt = 8;
            return;
        }
        if (s->in_styled_text_element && strcmp(name, "Font") == 0) {
            const char *fam = attr_get(attrs, "Family");
            const char *sz  = attr_get(attrs, "Size");
            const char *b   = attr_get(attrs, "Bold");
            const char *it  = attr_get(attrs, "Italic");
            free_safe(s->cur_segment.family);
            s->cur_segment.family = xstrdup(fam ? fam : "Arial");
            s->cur_segment.size_pt = sz ? atof(sz) : 8.0;
            s->cur_segment.bold = b && strcmp(b, "True") == 0;
            s->cur_segment.italic = it && strcmp(it, "True") == 0;
            return;
        }
        return;
    }

    // MODE_LABEL_SET
    if (strcmp(name, "LabelRecord") == 0) {
        s->in_label_record = true;
        memset(&s->cur_fields, 0, sizeof(s->cur_fields));
        return;
    }
    if (s->in_label_record && strcmp(name, "ObjectData") == 0) {
        s->in_object_data = true;
        const char *n = attr_get(attrs, "Name");
        s->cur_od_name = xstrdup(n ? n : "");
        return;
    }
}

// Forward declarations — used by finish_* on OOM before the definitions.
static void free_text_object(text_object_t *t);
static void free_barcode_object(barcode_object_t *b);

static void finish_text_object(state_t *s) {
    text_object_t to = s->cur_text;
    to.segments = s->cur_text_segments.data;
    to.n_segments = s->cur_text_segments.len;
    if (s->cur_bounds_set) to.bounds = s->cur_bounds;

    label_object_t obj = { .kind = OBJ_TEXT, .u.text = to };
    if (!vec_obj_push(&s->objects, obj)) {
        // realloc failed — free the strings we own so we don't leak on OOM.
        free_text_object(&obj.u.text);
    }
    // Reset the in-flight holders; ownership has been transferred (or freed).
    memset(&s->cur_text, 0, sizeof(s->cur_text));
    memset(&s->cur_text_segments, 0, sizeof(s->cur_text_segments));
}

static void finish_barcode_object(state_t *s) {
    barcode_object_t bc = s->cur_barcode;
    if (s->cur_bounds_set) bc.bounds = s->cur_bounds;

    label_object_t obj = { .kind = OBJ_BARCODE, .u.barcode = bc };
    if (!vec_obj_push(&s->objects, obj)) {
        free_barcode_object(&obj.u.barcode);
    }
    memset(&s->cur_barcode, 0, sizeof(s->cur_barcode));
}

static void end_elem(void *user, const XML_Char *name) {
    state_t *s = user;

    if (s->mode == MODE_LABEL) {
        // Character-data consumers: match end tag → consume s->cdata.
        if (strcmp(name, "PaperOrientation") == 0) {
            free_safe(s->orientation);
            s->orientation = rstrip_copy(s->cdata.data);
        } else if (s->in_text_object) {
            if (strcmp(name, "Name") == 0) {
                free_safe(s->cur_text.name);
                s->cur_text.name = rstrip_copy(s->cdata.data);
            } else if (strcmp(name, "Rotation") == 0) {
                char *v = rstrip_copy(s->cdata.data);
                s->cur_text.rotation = rot_from_str(v);
                free_safe(v);
            } else if (strcmp(name, "HorizontalAlignment") == 0) {
                char *v = rstrip_copy(s->cdata.data);
                s->cur_text.h_align = halign_from_str(v);
                free_safe(v);
            } else if (strcmp(name, "VerticalAlignment") == 0) {
                char *v = rstrip_copy(s->cdata.data);
                s->cur_text.v_align = valign_from_str(v);
                free_safe(v);
            } else if (strcmp(name, "TextFitMode") == 0) {
                char *v = rstrip_copy(s->cdata.data);
                s->cur_text.fit_mode = fit_from_str(v);
                free_safe(v);
            } else if (s->in_styled_text_element && strcmp(name, "String") == 0) {
                // Preserve whitespace inside String — DYMO uses xml:space="preserve".
                free_safe(s->cur_segment.text);
                s->cur_segment.text = xstrdup(s->cdata.data ? s->cdata.data : "");
            } else if (strcmp(name, "Element") == 0) {
                s->in_styled_text_element = false;
                if (!s->cur_segment.text) s->cur_segment.text = xstrdup("");
                if (!vec_seg_push(&s->cur_text_segments, s->cur_segment)) {
                    free_safe(s->cur_segment.text);
                    free_safe(s->cur_segment.family);
                }
                memset(&s->cur_segment, 0, sizeof(s->cur_segment));
            } else if (strcmp(name, "TextObject") == 0) {
                s->in_text_object = false;
                // The text object is not committed here; ObjectInfo close does it.
            }
        } else if (s->in_barcode_object) {
            if (strcmp(name, "Name") == 0) {
                free_safe(s->cur_barcode.name);
                s->cur_barcode.name = rstrip_copy(s->cdata.data);
            } else if (strcmp(name, "Rotation") == 0) {
                char *v = rstrip_copy(s->cdata.data);
                s->cur_barcode.rotation = rot_from_str(v);
                free_safe(v);
            } else if (strcmp(name, "HorizontalAlignment") == 0) {
                char *v = rstrip_copy(s->cdata.data);
                s->cur_barcode.h_align = halign_from_str(v);
                free_safe(v);
            } else if (strcmp(name, "Text") == 0) {
                free_safe(s->cur_barcode.text);
                s->cur_barcode.text = xstrdup(s->cdata.data ? s->cdata.data : "");
            } else if (strcmp(name, "Type") == 0) {
                char *v = rstrip_copy(s->cdata.data);
                s->cur_barcode.bc_type = bc_type_from_str(v);
                free_safe(v);
            } else if (strcmp(name, "Size") == 0) {
                free_safe(s->cur_barcode.size);
                s->cur_barcode.size = rstrip_copy(s->cdata.data);
            } else if (strcmp(name, "TextPosition") == 0) {
                free_safe(s->cur_barcode.text_position);
                s->cur_barcode.text_position = rstrip_copy(s->cdata.data);
            } else if (strcmp(name, "BarcodeObject") == 0) {
                s->in_barcode_object = false;
            }
        }

        if (strcmp(name, "ObjectInfo") == 0 && s->in_object_info) {
            s->in_object_info = false;
            // Commit the pending object — either text or barcode — depending
            // on which branch got populated.
            if (s->cur_text_segments.data || s->cur_text.name) {
                finish_text_object(s);
            } else if (s->cur_barcode.name || s->cur_barcode.text) {
                finish_barcode_object(s);
            }
        }
        if (strcmp(name, "DrawCommands") == 0) s->in_draw_commands = false;
        cbuf_reset(&s->cdata);
        return;
    }

    // MODE_LABEL_SET
    if (strcmp(name, "ObjectData") == 0 && s->in_object_data) {
        object_data_t od = {
            .name = s->cur_od_name,
            .value = xstrdup(s->cdata.data ? s->cdata.data : ""),
        };
        s->cur_od_name = NULL;
        if (!vec_fld_push(&s->cur_fields, od)) {
            free_safe(od.name);
            free_safe(od.value);
        }
        s->in_object_data = false;
    } else if (strcmp(name, "LabelRecord") == 0 && s->in_label_record) {
        s->in_label_record = false;
        label_record_t r = { .fields = s->cur_fields.data, .n_fields = s->cur_fields.len };
        if (!vec_rec_push(&s->records, r)) {
            // The fields array and its malloc'd strings are now orphaned —
            // free them rather than leak on OOM.
            for (size_t k = 0; k < r.n_fields; k++) {
                free_safe(r.fields[k].name);
                free_safe(r.fields[k].value);
            }
            free_safe(r.fields);
        }
        memset(&s->cur_fields, 0, sizeof(s->cur_fields));
    }
    cbuf_reset(&s->cdata);
}

static void cdata_handler(void *user, const XML_Char *data, int len) {
    state_t *s = user;
    cbuf_append(&s->cdata, data, (size_t)len);
}

// ---- public API ----------------------------------------------------------

label_t *parse_label_xml(const char *xml, size_t len) {
    state_t s;
    state_init(&s, MODE_LABEL);

    XML_Parser p = XML_ParserCreate(NULL);
    if (!p) return NULL;
    XML_SetUserData(p, &s);
    XML_SetElementHandler(p, start_elem, end_elem);
    XML_SetCharacterDataHandler(p, cdata_handler);

    if (XML_Parse(p, xml, (int)len, 1) == XML_STATUS_ERROR) {
        XML_ParserFree(p);
        // Free partially-parsed state. We re-use label_free by building
        // a label_t that owns what's been collected.
        label_t *lab = calloc(1, sizeof(label_t));
        if (lab) {
            lab->objects = s.objects.data;
            lab->n_objects = s.objects.len;
            lab->orientation = s.orientation;
            label_free(lab);
        }
        cbuf_free(&s.cdata);
        return NULL;
    }
    XML_ParserFree(p);
    cbuf_free(&s.cdata);

    label_t *lab = calloc(1, sizeof(label_t));
    if (!lab) {
        free_safe(s.orientation);
        // s.objects would also leak here on OOM, but an OOM at this point
        // means calloc(32 bytes) failed — the allocator is already wedged.
        return NULL;
    }
    if (!s.orientation) s.orientation = xstrdup("Landscape");
    if (!s.orientation) {
        free(lab);
        return NULL;
    }
    lab->orientation = s.orientation;
    double pw = s.paper_w_twips, ph = s.paper_h_twips;
    // In Landscape, the .label XML reports the rectangle in its portrait frame
    // but places objects in the oriented frame. Swap to match render_record.
    if (strcmp(lab->orientation, "Landscape") == 0) {
        double t = pw; pw = ph; ph = t;
    }
    lab->paper_w_twips = pw;
    lab->paper_h_twips = ph;
    lab->objects = s.objects.data;
    lab->n_objects = s.objects.len;
    return lab;
}

label_set_t *parse_label_set_xml(const char *xml, size_t len) {
    state_t s;
    state_init(&s, MODE_LABEL_SET);

    XML_Parser p = XML_ParserCreate(NULL);
    if (!p) return NULL;
    XML_SetUserData(p, &s);
    XML_SetElementHandler(p, start_elem, end_elem);
    XML_SetCharacterDataHandler(p, cdata_handler);

    enum XML_Status st = XML_Parse(p, xml, (int)len, 1);
    XML_ParserFree(p);
    cbuf_free(&s.cdata);
    free_safe(s.cur_od_name);

    if (st == XML_STATUS_ERROR) {
        // Free whatever we collected without calling label_set_free (which
        // expects a heap-allocated label_set_t).
        for (size_t i = 0; i < s.records.len; i++) {
            label_record_t *r = &s.records.data[i];
            for (size_t j = 0; j < r->n_fields; j++) {
                free_safe(r->fields[j].name);
                free_safe(r->fields[j].value);
            }
            free_safe(r->fields);
        }
        free_safe(s.records.data);
        return NULL;
    }

    label_set_t *set = calloc(1, sizeof(label_set_t));
    if (!set) return NULL;
    set->records = s.records.data;
    set->n_records = s.records.len;
    return set;
}

const char *record_get(const label_record_t *rec, const char *name) {
    if (!rec || !name) return NULL;
    for (size_t i = 0; i < rec->n_fields; i++) {
        if (rec->fields[i].name && strcmp(rec->fields[i].name, name) == 0) {
            return rec->fields[i].value;
        }
    }
    return NULL;
}

static void free_text_object(text_object_t *t) {
    free_safe(t->name);
    for (size_t i = 0; i < t->n_segments; i++) {
        free_safe(t->segments[i].text);
        free_safe(t->segments[i].family);
    }
    free_safe(t->segments);
}

static void free_barcode_object(barcode_object_t *b) {
    free_safe(b->name);
    free_safe(b->text);
    free_safe(b->size);
    free_safe(b->text_position);
}

void label_free(label_t *l) {
    if (!l) return;
    for (size_t i = 0; i < l->n_objects; i++) {
        if (l->objects[i].kind == OBJ_TEXT) free_text_object(&l->objects[i].u.text);
        else free_barcode_object(&l->objects[i].u.barcode);
    }
    free_safe(l->objects);
    free_safe(l->orientation);
    free(l);
}

void label_set_free(label_set_t *s) {
    if (!s) return;
    for (size_t i = 0; i < s->n_records; i++) {
        label_record_t *r = &s->records[i];
        for (size_t j = 0; j < r->n_fields; j++) {
            free_safe(r->fields[j].name);
            free_safe(r->fields[j].value);
        }
        free_safe(r->fields);
    }
    free_safe(s->records);
    free(s);
}
