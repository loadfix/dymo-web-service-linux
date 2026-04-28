// Label renderer: DieCutLabel XML → 1-bit PNG at 300 DPI.
//
// Design goal: visually match the Python Pillow/ImageDraw renderer in
// ../../dymo_service/render.py. Text uses Pango (still targets Liberation Sans
// / DejaVu Sans as fallback for "Arial"). Barcodes use integer-pixel module
// widths so scans stay clean. QR via libqrencode. Output is thresholded to
// 1-bit to defeat the DYMO driver's halftoning (paired with DymoHalftoning=NLL).

#include "render.h"
#include "barcode.h"
#include "log.h"

#include <cairo.h>
#include <pango/pangocairo.h>
#include <qrencode.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PX_PER_TWIP ((double)DYMO_DPI / (double)DYMO_TWIPS_PER_INCH)

static int twips_to_px(double twips) {
    return (int)lround(twips * PX_PER_TWIP);
}

// ----------------------------------------------------------------------------
// Pango font setup
// ----------------------------------------------------------------------------

// Cairo+Pango resolve "Arial" to fontconfig's default sans, which on the
// target Ubuntu systems is Liberation Sans (metrically compatible with Arial).
// We just pass the family name through.
static PangoFontDescription *make_font_desc(const char *family, double size_pt,
                                            bool bold, bool italic) {
    PangoFontDescription *fd = pango_font_description_new();
    // Map "Arial" → "Arial,Liberation Sans,DejaVu Sans" so the renderer picks
    // up the closest-matching family via fontconfig fallback.
    if (!family || !*family || strcmp(family, "Arial") == 0) {
        pango_font_description_set_family(fd, "Arial, Liberation Sans, DejaVu Sans");
    } else {
        pango_font_description_set_family(fd, family);
    }
    pango_font_description_set_size(fd, (int)lround(size_pt * PANGO_SCALE));
    pango_font_description_set_weight(fd, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_style(fd, italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
    // Pango sizes are in points relative to the cairo DPI we set on the context.
    pango_font_description_set_absolute_size(fd, size_pt * DYMO_DPI / 72.0 * PANGO_SCALE);
    return fd;
}

// ----------------------------------------------------------------------------
// Text rendering with ShrinkToFit
// ----------------------------------------------------------------------------

static void draw_text_object(cairo_t *cr,
                             const text_object_t *obj,
                             const label_record_t *rec) {
    int x  = twips_to_px(obj->bounds.x);
    int y  = twips_to_px(obj->bounds.y);
    int w  = twips_to_px(obj->bounds.w);
    int h  = twips_to_px(obj->bounds.h);
    if (w <= 0 || h <= 0) return;

    // Substitute from record: DYMO replaces all segments with the ObjectData
    // text, keeping the first segment's style.
    const text_segment_t *proto = obj->n_segments ? &obj->segments[0] : NULL;
    const char *replacement = rec ? record_get(rec, obj->name) : NULL;

    char *text = NULL;
    if (replacement) {
        text = strdup(replacement);
    } else {
        // Join segments with newlines, matching the Python behavior.
        size_t tot = 0;
        for (size_t i = 0; i < obj->n_segments; i++)
            tot += (obj->segments[i].text ? strlen(obj->segments[i].text) : 0) + 1;
        text = malloc(tot + 1);
        if (text) {
            text[0] = '\0';
            for (size_t i = 0; i < obj->n_segments; i++) {
                if (i) strcat(text, "\n");
                if (obj->segments[i].text) strcat(text, obj->segments[i].text);
            }
        }
    }
    if (!text) return;

    const char *family = proto ? proto->family : "Arial";
    double size_pt     = proto ? proto->size_pt : 8.0;
    bool bold          = proto ? proto->bold    : false;
    bool italic        = proto ? proto->italic  : false;

    // Build the layer off-screen so rotation can apply cleanly.
    cairo_surface_t *layer = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
    cairo_t *lc = cairo_create(layer);
    // Fill with transparent (0) and paint text at full alpha.
    cairo_set_source_rgba(lc, 0, 0, 0, 0);
    cairo_paint(lc);
    cairo_set_source_rgba(lc, 0, 0, 0, 1);

    PangoLayout *layout = pango_cairo_create_layout(lc);
    pango_layout_set_width(layout, w * PANGO_SCALE);
    // Pango wraps on whitespace; this matches Python's per-word wrap.
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD);
    if (obj->h_align == HALIGN_CENTER) pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    else if (obj->h_align == HALIGN_RIGHT) pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
    else pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);

    double cur_size = size_pt;
    PangoFontDescription *fd = make_font_desc(family, cur_size, bold, italic);
    pango_layout_set_font_description(layout, fd);
    pango_layout_set_text(layout, text, -1);

    if (obj->fit_mode == FIT_SHRINK) {
        // Shrink by 0.5pt until the wrapped text fits both axes, min 4pt.
        int lw, lh;
        while (cur_size > 4.0) {
            pango_layout_get_pixel_size(layout, &lw, &lh);
            if (lw <= w && lh <= h) break;
            cur_size -= 0.5;
            pango_font_description_free(fd);
            fd = make_font_desc(family, cur_size, bold, italic);
            pango_layout_set_font_description(layout, fd);
        }
    }

    // Vertical alignment inside the bounds.
    int lw, lh;
    pango_layout_get_pixel_size(layout, &lw, &lh);
    int ty = 0;
    if (obj->v_align == VALIGN_MIDDLE) ty = (h - lh) / 2;
    else if (obj->v_align == VALIGN_BOTTOM) ty = h - lh;
    if (ty < 0) ty = 0;

    cairo_move_to(lc, 0, ty);
    pango_cairo_show_layout(lc, layout);
    pango_font_description_free(fd);
    g_object_unref(layout);
    cairo_destroy(lc);

    // Place the layer onto the main canvas, rotated if needed.
    if (obj->rotation == ROT_0) {
        cairo_set_source_surface(cr, layer, x, y);
        cairo_paint(cr);
    } else {
        double angle = -(double)obj->rotation * M_PI / 180.0;
        cairo_save(cr);
        // Translate to the bounds origin, rotate around it, then paint.
        cairo_translate(cr, x, y);
        cairo_rotate(cr, angle);
        // After rotation, the layer's (0,0) is at the bounds origin in the
        // rotated frame. For 90°/270° we'd also need to offset so the rotated
        // image lands inside the bounds; match Python's Image.rotate(expand=True)
        // by keeping the layer anchored at (0,0) of the rotated frame.
        cairo_set_source_surface(cr, layer, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    cairo_surface_destroy(layer);
    free(text);
}

// ----------------------------------------------------------------------------
// Barcode drawing
// ----------------------------------------------------------------------------

static cairo_surface_t *make_bars(const bar_widths_t *bw,
                                  int bar_h, int module_px, int quiet_px) {
    int total_px = 0;
    for (size_t i = 0; i < bw->n; i++) total_px += bw->widths[i] * module_px;
    int width = total_px + 2 * quiet_px;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_A8, width, bar_h);
    cairo_t *c = cairo_create(s);
    // transparent bg (A8=0 everywhere) already
    cairo_set_source_rgba(c, 0, 0, 0, 1);
    int x = quiet_px;
    bool draw_bar = true;  // alternate bar/space starting with bar
    for (size_t i = 0; i < bw->n; i++) {
        int span = bw->widths[i] * module_px;
        if (draw_bar) {
            cairo_rectangle(c, x, 0, span, bar_h);
            cairo_fill(c);
        }
        x += span;
        draw_bar = !draw_bar;
    }
    cairo_destroy(c);
    return s;
}

static cairo_surface_t *make_qr(const char *text, int px_w, int px_h) {
    QRcode *qr = QRcode_encodeString(text ? text : "", 0, QR_ECLEVEL_M,
                                     QR_MODE_8, 1);
    if (!qr) return NULL;
    int size = (px_w < px_h) ? px_w : px_h;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_A8, size, size);
    unsigned char *data = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);

    // Map QR modules to pixels via integer scaling (nearest-neighbor).
    int module_px = size / qr->width;
    if (module_px < 1) module_px = 1;
    int qr_size = module_px * qr->width;
    int offset = (size - qr_size) / 2;

    memset(data, 0, stride * size);
    for (int y = 0; y < qr->width; y++) {
        for (int x = 0; x < qr->width; x++) {
            if (qr->data[y * qr->width + x] & 1) {
                // module is black
                int py0 = offset + y * module_px;
                int px0 = offset + x * module_px;
                for (int dy = 0; dy < module_px; dy++) {
                    unsigned char *row = data + (py0 + dy) * stride;
                    for (int dx = 0; dx < module_px; dx++) {
                        row[px0 + dx] = 255;
                    }
                }
            }
        }
    }
    cairo_surface_mark_dirty(s);
    QRcode_free(qr);
    return s;
}

static void draw_barcode_object(cairo_t *cr,
                                const barcode_object_t *obj,
                                const label_record_t *rec) {
    int x = twips_to_px(obj->bounds.x);
    int y = twips_to_px(obj->bounds.y);
    int w = twips_to_px(obj->bounds.w);
    int h = twips_to_px(obj->bounds.h);
    if (w <= 0 || h <= 0) return;

    const char *raw = rec ? record_get(rec, obj->name) : NULL;
    if (!raw || !*raw) raw = obj->text ? obj->text : "";

    cairo_surface_t *img = NULL;
    int img_w = 0, img_h = 0;

    if (obj->bc_type == BC_QR) {
        const char *text = raw;
        if (strncmp(text, "URL:", 4) == 0) text += 4;
        img = make_qr(text, w, h);
        if (img) {
            img_w = cairo_image_surface_get_width(img);
            img_h = cairo_image_surface_get_height(img);
        }
    } else {
        const char *text = raw;
        char *digits_buf = NULL;
        bar_widths_t bw;
        const char *caption;

        if (obj->bc_type == BC_CODE2OF5) {
            // Filter to digits, pad left with 0 for odd count.
            size_t n = strlen(text);
            digits_buf = malloc(n + 2);
            if (!digits_buf) return;
            size_t j = 0;
            for (size_t i = 0; i < n; i++) {
                if (text[i] >= '0' && text[i] <= '9') digits_buf[j++] = text[i];
            }
            digits_buf[j] = '\0';
            if (j % 2 == 1) {
                memmove(digits_buf + 1, digits_buf, j + 1);
                digits_buf[0] = '0';
            }
            bw = itf_pattern(digits_buf);
            caption = digits_buf;
        } else {
            // Code 128 (or unknown → treat as Code 128)
            bw = code128_pattern(text);
            caption = text;
        }

        if (!bw.widths || bw.n == 0) { free(digits_buf); return; }

        // Module sizing: fit the largest integer module into the bounds.
        size_t total_modules = 0;
        for (size_t i = 0; i < bw.n; i++) total_modules += bw.widths[i];
        int quiet_modules = 10;
        int total_all = (int)total_modules + 2 * quiet_modules;
        int module_px = w / total_all;
        if (module_px < 1) module_px = 1;
        int quiet_px = module_px * quiet_modules;

        bool show_text = obj->text_position && strcmp(obj->text_position, "None") != 0;
        // Caption font (Pango) — use 8pt Arial fallback, matching Python.
        int caption_h = 0;
        PangoLayout *cap_layout = NULL;
        if (show_text) {
            cap_layout = pango_cairo_create_layout(cr);
            PangoFontDescription *fd = make_font_desc("Arial", 8, false, false);
            pango_layout_set_font_description(cap_layout, fd);
            pango_layout_set_text(cap_layout, caption ? caption : "", -1);
            int cw, ch;
            pango_layout_get_pixel_size(cap_layout, &cw, &ch);
            caption_h = ch + 4;
            pango_font_description_free(fd);
        }
        int bar_h = h - caption_h;
        if (bar_h < 10) bar_h = 10;

        cairo_surface_t *bars = make_bars(&bw, bar_h, module_px, quiet_px);
        int bars_w = cairo_image_surface_get_width(bars);
        int bars_h = cairo_image_surface_get_height(bars);

        // Compose onto canvas-sized layer.
        img_w = bars_w;
        img_h = bars_h + caption_h;
        img = cairo_image_surface_create(CAIRO_FORMAT_A8, img_w, img_h);
        cairo_t *ic = cairo_create(img);
        cairo_set_source_rgba(ic, 0, 0, 0, 0);
        cairo_paint(ic);
        cairo_set_source_surface(ic, bars, 0, 0);
        cairo_paint(ic);
        if (show_text && cap_layout) {
            int cw, ch;
            pango_layout_get_pixel_size(cap_layout, &cw, &ch);
            int tx = (img_w - cw) / 2;
            if (tx < 0) tx = 0;
            int ty = bars_h + 2;
            cairo_set_source_rgba(ic, 0, 0, 0, 1);
            cairo_move_to(ic, tx, ty);
            pango_cairo_show_layout(ic, cap_layout);
            g_object_unref(cap_layout);
        }
        cairo_destroy(ic);
        cairo_surface_destroy(bars);
        free(bw.widths);
        free(digits_buf);
    }

    if (!img) return;

    // Placement on the main canvas. Apply rotation around the bounds origin
    // to match Python's Image.rotate(expand=True) + paste(x, y).
    int ox = x;
    if (obj->h_align == HALIGN_CENTER) ox = x + (w - img_w) / 2;
    else if (obj->h_align == HALIGN_RIGHT) ox = x + (w - img_w);
    int oy = y + (h - img_h) / 2;
    if (ox < x) ox = x;
    if (oy < y) oy = y;

    if (obj->rotation == ROT_0) {
        cairo_set_source_surface(cr, img, ox, oy);
        cairo_paint(cr);
    } else {
        double angle = -(double)obj->rotation * M_PI / 180.0;
        cairo_save(cr);
        cairo_translate(cr, ox, oy);
        cairo_rotate(cr, angle);
        cairo_set_source_surface(cr, img, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    cairo_surface_destroy(img);
}

// ----------------------------------------------------------------------------
// Threshold an ARGB/A8 canvas to 1-bit and save as PNG at 300 DPI.
// ----------------------------------------------------------------------------

static int save_1bit_png(cairo_surface_t *src, const char *path) {
    int w = cairo_image_surface_get_width(src);
    int h = cairo_image_surface_get_height(src);

    // Convert the A8 canvas (alpha = ink density 0..255) to a 1-bit mask by
    // thresholding at 128, then write via Cairo as an ARGB32 surface where
    // black pixels are opaque black and everything else is opaque white.
    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    unsigned char *sd = cairo_image_surface_get_data(src);
    int ss = cairo_image_surface_get_stride(src);
    unsigned char *od = cairo_image_surface_get_data(out);
    int os = cairo_image_surface_get_stride(out);

    for (int y = 0; y < h; y++) {
        unsigned char *sr = sd + y * ss;
        uint32_t *orow = (uint32_t *)(od + y * os);
        for (int x = 0; x < w; x++) {
            // A8 pixel: 0 = no ink, 255 = full ink. Threshold at 128.
            uint32_t px;
            if (sr[x] >= 128) {
                // black, opaque
                px = 0xFF000000u;
            } else {
                // white, opaque
                px = 0xFFFFFFFFu;
            }
            orow[x] = px;
        }
    }
    cairo_surface_mark_dirty(out);
    cairo_status_t st = cairo_surface_write_to_png(out, path);
    cairo_surface_destroy(out);
    if (st != CAIRO_STATUS_SUCCESS) {
        LOG_ERROR("cairo_surface_write_to_png(%s) failed: %s",
                  path, cairo_status_to_string(st));
        return -1;
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

static cairo_surface_t *render_record(const label_t *label,
                                      const label_record_t *rec) {
    int canvas_w = twips_to_px(label->paper_w_twips);
    int canvas_h = twips_to_px(label->paper_h_twips);

    // Use A8: 0 = white (paper), 255 = black (ink). Thresholding is done
    // at save time. This keeps the compositing logic the same for text and
    // barcode layers.
    cairo_surface_t *canvas = cairo_image_surface_create(CAIRO_FORMAT_A8, canvas_w, canvas_h);
    cairo_t *cr = cairo_create(canvas);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    // A8 canvas starts at 0 (white). Objects paint with alpha=1 (black).

    // Cairo's DPI hint for Pango so point sizes → pixels match Python.
    PangoContext *pctx = pango_cairo_create_context(cr);
    cairo_font_options_t *fo = cairo_font_options_create();
    cairo_get_font_options(cr, fo);
    pango_cairo_context_set_font_options(pctx, fo);
    cairo_font_options_destroy(fo);
    g_object_unref(pctx);

    for (size_t i = 0; i < label->n_objects; i++) {
        const label_object_t *o = &label->objects[i];
        if (o->kind == OBJ_TEXT) draw_text_object(cr, &o->u.text, rec);
        else                     draw_barcode_object(cr, &o->u.barcode, rec);
    }

    cairo_destroy(cr);
    return canvas;
}

int render_all(const label_t *label,
               const label_set_t *set,
               const char *out_dir,
               char ***out_paths,
               size_t *n_out) {
    if (!label || !set || !out_dir || !out_paths || !n_out) return -1;

    *out_paths = calloc(set->n_records, sizeof(char *));
    if (!*out_paths) return -1;
    *n_out = 0;

    for (size_t i = 0; i < set->n_records; i++) {
        cairo_surface_t *canvas = render_record(label, &set->records[i]);
        if (!canvas) continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/label-%zu.png", out_dir, i);
        if (save_1bit_png(canvas, path) == 0) {
            (*out_paths)[*n_out] = strdup(path);
            (*n_out)++;
        }
        cairo_surface_destroy(canvas);
    }
    return 0;
}
