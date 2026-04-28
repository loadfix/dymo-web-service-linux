// Hand the rendered label PNG to CUPS.
//
// The Python reference rotates the image to portrait and resizes to exactly
// 300×638 (1"×2.125" at 300 DPI) before calling lp. We do the same here using
// Cairo so the bytes handed to CUPS match.

#include "printing.h"
#include "log.h"
#include "render.h"

#include <cairo.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MEDIA_SHORT_IN 1.0
#define MEDIA_LONG_IN  2.125
#define MEDIA_CODE     "w72h154"
#define DEFAULT_PRINTER "LabelWriter-450-Turbo"

static int short_px(void) { return (int)lround(MEDIA_SHORT_IN * DYMO_DPI); }
static int long_px(void)  { return (int)lround(MEDIA_LONG_IN * DYMO_DPI); }

// Rotate an ARGB32 surface 90° counter-clockwise (equivalent to Pillow's
// rotate(-90, expand=True)). Caller destroys both surfaces.
static cairo_surface_t *rotate_ccw_90(cairo_surface_t *src) {
    int w = cairo_image_surface_get_width(src);
    int h = cairo_image_surface_get_height(src);
    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, h, w);
    cairo_t *cr = cairo_create(out);
    cairo_translate(cr, 0, w);
    cairo_rotate(cr, -M_PI / 2.0);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_destroy(cr);
    return out;
}

// Nearest-neighbor resize (Pillow's Image.NEAREST). Keeps bar edges crisp.
static cairo_surface_t *resize_nearest(cairo_surface_t *src, int new_w, int new_h) {
    int sw = cairo_image_surface_get_width(src);
    int sh = cairo_image_surface_get_height(src);
    if (sw == new_w && sh == new_h) {
        // Return a copy so we can uniformly destroy the returned surface.
        cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
        cairo_t *cr = cairo_create(out);
        cairo_set_source_surface(cr, src, 0, 0);
        cairo_paint(cr);
        cairo_destroy(cr);
        return out;
    }
    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, new_w, new_h);
    cairo_t *cr = cairo_create(out);
    cairo_scale(cr, (double)new_w / sw, (double)new_h / sh);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_destroy(cr);
    return out;
}

// Pick a temp directory that the daemon can actually write to. With systemd
// PrivateTmp=true our /tmp is a per-service private namespace, which is the
// safest default. Fall back to DYMO_TMPDIR / $TMPDIR only if explicitly set.
static const char *tmp_dir(void) {
    const char *p = getenv("DYMO_TMPDIR");
    if (p && *p) return p;
    p = getenv("TMPDIR");
    if (p && *p) return p;
    return "/tmp";
}

// Cairo PNG write callback: push bytes into the already-open file descriptor
// that mkstemps handed us. No path is reopened by name, so there's no window
// for a symlink-race attacker to redirect the write.
static cairo_status_t write_to_fd(void *closure, const unsigned char *data,
                                  unsigned int length) {
    int fd = *(int *)closure;
    while (length > 0) {
        ssize_t w = write(fd, data, length);
        if (w < 0) {
            if (errno == EINTR) continue;
            return CAIRO_STATUS_WRITE_ERROR;
        }
        data += w;
        length -= (unsigned int)w;
    }
    return CAIRO_STATUS_SUCCESS;
}

// Prepare a portrait-oriented PNG at exact media pixel dimensions. Returns a
// malloc'd path to a tmp PNG; caller frees and unlinks.
static char *prepare_print_png(const char *input_png) {
    cairo_surface_t *src = cairo_image_surface_create_from_png(input_png);
    if (cairo_surface_status(src) != CAIRO_STATUS_SUCCESS) {
        LOG_ERROR("failed to read %s", input_png);
        cairo_surface_destroy(src);
        return NULL;
    }
    int w = cairo_image_surface_get_width(src);
    int h = cairo_image_surface_get_height(src);

    cairo_surface_t *rotated = (w > h) ? rotate_ccw_90(src) : NULL;
    cairo_surface_t *oriented = rotated ? rotated : src;
    cairo_surface_t *resized = resize_nearest(oriented, short_px(), long_px());
    if (rotated) cairo_surface_destroy(rotated);
    cairo_surface_destroy(src);

    const char *base = tmp_dir();
    // Permissions 0700 on our sub-dir so other local users can't enumerate or
    // plant files even if /tmp isn't PrivateTmp'd.
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/dymo-service", base);
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        LOG_ERROR("mkdir %s: %s", dir, strerror(errno));
        cairo_surface_destroy(resized);
        return NULL;
    }

    char tmpl[600];
    snprintf(tmpl, sizeof(tmpl), "%s/dymoXXXXXX.png", dir);
    int fd = mkstemps(tmpl, 4);
    if (fd < 0) {
        LOG_ERROR("mkstemps(%s): %s", tmpl, strerror(errno));
        cairo_surface_destroy(resized);
        return NULL;
    }

    // Write via the open fd instead of reopening `tmpl` by name — this closes
    // the mkstemps → cairo_surface_write_to_png TOCTOU window a local attacker
    // would otherwise have to swap the file with a symlink to something the
    // daemon can write.
    cairo_status_t st = cairo_surface_write_to_png_stream(resized, write_to_fd, &fd);
    cairo_surface_destroy(resized);
    close(fd);
    if (st != CAIRO_STATUS_SUCCESS) {
        LOG_ERROR("write_to_png_stream(%s): %s", tmpl, cairo_status_to_string(st));
        unlink(tmpl);
        return NULL;
    }
    return strdup(tmpl);
}

int print_label_png(const char *png_path, const char *printer_name) {
    if (!printer_name || !*printer_name) printer_name = DEFAULT_PRINTER;

    char *prepped = prepare_print_png(png_path);
    if (!prepped) return -1;

    // Build argv for `lp` matching Python's call.
    char *argv[] = {
        "lp",
        "-d", (char *)printer_name,
        "-o", "media=" MEDIA_CODE,
        "-o", "DymoHalftoning=NLL",
        "-o", "DymoPrintQuality=Graphics",
        "-o", "DymoPrintDensity=Dark",
        "-o", "fit-to-page",
        prepped,
        NULL,
    };

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("fork: %s", strerror(errno));
        unlink(prepped);
        free(prepped);
        return -1;
    }
    if (pid == 0) {
        // Child: silence stdout (lp prints "request id is …"), keep stderr.
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDOUT_FILENO); close(dn); }
        execvp("lp", argv);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        LOG_ERROR("waitpid: %s", strerror(errno));
        unlink(prepped);
        free(prepped);
        return -1;
    }
    unlink(prepped);
    free(prepped);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        LOG_INFO("printed %s to %s", png_path, printer_name);
        return 0;
    }
    LOG_ERROR("lp exited with status %d", WEXITSTATUS(status));
    return -1;
}
