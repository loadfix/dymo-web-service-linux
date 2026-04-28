// HTTP/HTTPS routing for the DYMO Connect emulator, built on mongoose.
//
// Endpoints mirror dymo_service/app.py — the DYMO JavaScript framework probes
// these exact paths. Response body shapes (XML/text) match what DYMO Connect
// on Windows returns.

#include "server.h"
#include "http.h"
#include "log.h"
#include "xml_parse.h"
#include "render.h"
#include "printing.h"

#include "mongoose.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static volatile int g_running = 1;

static void on_sig(int sig) {
    (void)sig;
    g_running = 0;
}

// ---- CORS --------------------------------------------------------------

// True iff origin (as presented in the request header) is an exact match
// for one of the comma-separated entries in the allowlist. Whitespace around
// entries is tolerated; matching is byte-exact.
static bool origin_allowed(const char *allowlist, struct mg_str origin) {
    if (!allowlist || !*allowlist || origin.len == 0) return false;
    const char *p = allowlist;
    while (*p) {
        // Skip leading whitespace + commas
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        // Trim trailing whitespace
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        size_t len = (size_t)(end - start);
        if (len == origin.len && memcmp(start, origin.buf, len) == 0) {
            return true;
        }
    }
    return false;
}

// Emit CORS headers only when the Origin is explicitly allowed. No wildcard
// fallback — an unknown origin gets no CORS headers and the browser blocks
// the response. This closes the drive-by-print attack surface.
static void cors_headers(struct mg_connection *c, struct mg_http_message *hm,
                         const server_cfg_t *cfg) {
    struct mg_str *origin = mg_http_get_header(hm, "Origin");
    if (!origin || origin->len == 0) return;  // not a cross-origin request
    if (!origin_allowed(cfg->allowed_origins, *origin)) return;

    mg_printf(c, "Access-Control-Allow-Origin: %.*s\r\n",
              (int)origin->len, origin->buf);
    mg_printf(c, "Access-Control-Allow-Credentials: true\r\n");
    mg_printf(c, "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n");
    mg_printf(c, "Access-Control-Allow-Headers: Content-Type\r\n");
    mg_printf(c, "Vary: Origin\r\n");
}

static const char *status_phrase(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default:  return "Error";
    }
}

static void reply_text(struct mg_connection *c, struct mg_http_message *hm,
                       const server_cfg_t *cfg,
                       int code, const char *ctype, const char *body, size_t body_len) {
    mg_printf(c, "HTTP/1.1 %d %s\r\n", code, status_phrase(code));
    cors_headers(c, hm, cfg);
    // Basic security headers. These are cheap defense-in-depth; they do
    // nothing for loopback but matter if the service is ever LAN-exposed
    // or reverse-proxied.
    mg_printf(c, "X-Content-Type-Options: nosniff\r\n");
    mg_printf(c, "X-Frame-Options: DENY\r\n");
    mg_printf(c, "Content-Security-Policy: default-src 'none'; frame-ancestors 'none'\r\n");
    mg_printf(c, "Referrer-Policy: no-referrer\r\n");
    mg_printf(c, "Content-Type: %s\r\n", ctype);
    mg_printf(c, "Content-Length: %lu\r\n\r\n", (unsigned long)body_len);
    mg_send(c, body, body_len);
}

static void reply_200_plain(struct mg_connection *c, struct mg_http_message *hm,
                            const server_cfg_t *cfg, const char *s) {
    reply_text(c, hm, cfg, 200, "text/plain; charset=utf-8", s, strlen(s));
}

static void reply_options(struct mg_connection *c, struct mg_http_message *hm,
                          const server_cfg_t *cfg) {
    mg_printf(c, "HTTP/1.1 204 No Content\r\n");
    cors_headers(c, hm, cfg);
    mg_printf(c, "Content-Length: 0\r\n\r\n");
}

// ---- Handlers ----------------------------------------------------------

static void handle_get_printers(struct mg_connection *c, struct mg_http_message *hm,
                                const server_cfg_t *cfg) {
    char xml[1024];
    const char *name = cfg->reported_name ? cfg->reported_name : "DYMO LabelWriter 450 Turbo";
    int n = snprintf(xml, sizeof(xml),
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<Printers>\n"
        "  <LabelWriterPrinter>\n"
        "    <Name>%s</Name>\n"
        "    <ModelName>%s</ModelName>\n"
        "    <IsConnected>True</IsConnected>\n"
        "    <IsLocal>True</IsLocal>\n"
        "    <IsTwinTurbo>False</IsTwinTurbo>\n"
        "  </LabelWriterPrinter>\n"
        "</Printers>\n", name, name);
    reply_text(c, hm, cfg, 200, "application/xml; charset=utf-8", xml, (size_t)n);
}

// Write body content to a file inside capture_dir/<filename>. Open with
// O_CREAT|O_EXCL|O_NOFOLLOW so we refuse to follow a pre-planted symlink or
// overwrite an existing file — the capture_dir is freshly mkstemp'd so any
// existing name indicates an attack attempt rather than a legitimate retry.
static void dump_to_file(const char *dir, const char *fname, const char *data, size_t len) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, fname);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        LOG_WARN("capture open(%s): %s", path, strerror(errno));
        return;
    }
    if (data && len) {
        size_t off = 0;
        while (off < len) {
            ssize_t w = write(fd, data + off, len - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                LOG_WARN("capture write(%s): %s", path, strerror(errno));
                break;
            }
            off += (size_t)w;
        }
    }
    close(fd);
}

static char *make_capture_dir(const char *base) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char stamp[64];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    char micros[32];
    snprintf(micros, sizeof(micros), "%06ld", (long)(ts.tv_nsec / 1000));

    size_t plen = strlen(base) + sizeof(stamp) + sizeof(micros) + 16;
    char *path = malloc(plen);
    if (!path) return NULL;
    snprintf(path, plen, "%s/printlabel-%s-%s", base, stamp, micros);

    // "mkdir -p base" — but report the failure if it's not EEXIST so a broken
    // log dir (disk full, wrong perms) shows up in the journal instead of
    // silently losing captures.
    if (mkdir(base, 0750) != 0 && errno != EEXIST) {
        LOG_WARN("mkdir %s: %s", base, strerror(errno));
        free(path);
        return NULL;
    }
    // Per-request dirs are 0700: only the daemon reads them. Captured label
    // payloads may contain customer data (names, addresses) — widening the
    // mode to group-readable via 0750 would leak that to anyone in `adm`.
    if (mkdir(path, 0700) != 0) {
        LOG_WARN("mkdir %s: %s", path, strerror(errno));
        free(path);
        return NULL;
    }
    return path;
}

static void handle_print_label(struct mg_connection *c, struct mg_http_message *hm,
                               const server_cfg_t *cfg) {
    // Body-size cap. The caller might have set Content-Length to a value
    // within the global mongoose cap but we still want a tighter per-endpoint
    // limit. A legitimate CellarTracker label set is well under 50 KiB; the
    // default cap is 256 KiB which leaves plenty of headroom.
    if (hm->body.len > cfg->max_body_bytes) {
        LOG_WARN("PrintLabel body %zu bytes exceeds cap %zu",
                 hm->body.len, cfg->max_body_bytes);
        const char *msg = "payload too large\n";
        reply_text(c, hm, cfg, 413, "text/plain; charset=utf-8", msg, strlen(msg));
        return;
    }

    // Pull out the four form fields.
    char *printer_name  = form_get(hm->body.buf, hm->body.len, "printerName");
    char *label_xml     = form_get(hm->body.buf, hm->body.len, "labelXml");
    char *print_params  = form_get(hm->body.buf, hm->body.len, "printParamsXml");
    char *label_set_xml = form_get(hm->body.buf, hm->body.len, "labelSetXml");

    // Always capture payloads to logs/ if enabled; this has no observable
    // effect on the printer and makes debugging vastly easier.
    char *capture_dir = NULL;
    if (cfg->capture_payloads && cfg->log_dir) {
        capture_dir = make_capture_dir(cfg->log_dir);
        if (capture_dir) {
            dump_to_file(capture_dir, "printerName.txt",
                         printer_name ? printer_name : "",
                         printer_name ? strlen(printer_name) : 0);
            dump_to_file(capture_dir, "label.xml",
                         label_xml ? label_xml : "",
                         label_xml ? strlen(label_xml) : 0);
            dump_to_file(capture_dir, "printParams.xml",
                         print_params ? print_params : "",
                         print_params ? strlen(print_params) : 0);
            dump_to_file(capture_dir, "labelSet.xml",
                         label_set_xml ? label_set_xml : "",
                         label_set_xml ? strlen(label_set_xml) : 0);
            LOG_INFO("captured PrintLabel payload to %s", capture_dir);
        }
    }

    bool ok = false;
    if (label_xml && label_set_xml && capture_dir) {
        label_t *label = parse_label_xml(label_xml, strlen(label_xml));
        label_set_t *set = parse_label_set_xml(label_set_xml, strlen(label_set_xml));
        if (label && set) {
            char **paths = NULL;
            size_t n = 0;
            if (render_all(label, set, capture_dir, &paths, &n) == 0 && n > 0) {
                // Report success only if *every* label print succeeds. A single
                // CUPS failure (e.g. printer offline) surfaces as HTTP 500 so
                // CellarTracker's UI can reflect reality instead of reporting
                // a fake success and losing the user's job.
                bool all_ok = true;
                for (size_t i = 0; i < n; i++) {
                    if (print_label_png(paths[i], cfg->printer_name) != 0) {
                        all_ok = false;
                    } else {
                        LOG_INFO("printed %s", paths[i]);
                    }
                    free(paths[i]);
                }
                free(paths);
                ok = all_ok;
            }
        } else {
            LOG_WARN("label XML parse failed (label=%p, set=%p)",
                     (void*)label, (void*)set);
        }
        if (label) label_free(label);
        if (set) label_set_free(set);
    }

    free(printer_name);
    free(label_xml);
    free(print_params);
    free(label_set_xml);
    free(capture_dir);

    if (ok) {
        reply_200_plain(c, hm, cfg, "true");
    } else {
        const char *msg = "false";
        reply_text(c, hm, cfg, 500, "text/plain; charset=utf-8", msg, strlen(msg));
    }
}

// ---- Mongoose event handler --------------------------------------------

static bool uri_eq(struct mg_http_message *hm, const char *path) {
    return mg_match(hm->uri, mg_str(path), NULL);
}

static bool method_is(struct mg_http_message *hm, const char *m) {
    return mg_strcmp(hm->method, mg_str(m)) == 0;
}

static void handle_http(struct mg_connection *c, int ev, void *ev_data) {
    const server_cfg_t *cfg = (const server_cfg_t *)c->fn_data;

    if (ev == MG_EV_ACCEPT) {
        // If this connection was accepted on the HTTPS listener, start TLS.
        // We mark TLS listeners by storing non-NULL in c->loc (via labels
        // below) — but the simpler pattern is to attach tls_opts via the
        // listener. Handled in server_run.
        return;
    }

    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        LOG_INFO("%.*s %.*s",
                 (int)hm->method.len, hm->method.buf,
                 (int)hm->uri.len, hm->uri.buf);

        // CORS preflight
        if (method_is(hm, "OPTIONS")) {
            reply_options(c, hm, cfg);
            return;
        }

        if (method_is(hm, "GET") && uri_eq(hm, "/")) {
            reply_200_plain(c, hm, cfg, "DYMO Web Service (Linux emulator, C)");
            return;
        }
        if (method_is(hm, "GET") && uri_eq(hm, "/DYMO/DLS/Printing/Check")) {
            reply_200_plain(c, hm, cfg, "true");
            return;
        }
        if (method_is(hm, "GET") && uri_eq(hm, "/DYMO/DLS/Printing/StatusConnected")) {
            reply_200_plain(c, hm, cfg, "true");
            return;
        }
        if (method_is(hm, "GET") && uri_eq(hm, "/DYMO/DLS/Printing/GetPrinters")) {
            handle_get_printers(c, hm, cfg);
            return;
        }
        if (method_is(hm, "POST") && uri_eq(hm, "/DYMO/DLS/Printing/PrintLabel")) {
            handle_print_label(c, hm, cfg);
            return;
        }
        if (method_is(hm, "POST") && uri_eq(hm, "/DYMO/DLS/Printing/RenderLabel")) {
            // Empty string is enough to let the framework proceed.
            reply_200_plain(c, hm, cfg, "");
            return;
        }

        const char *nf = "Not found";
        reply_text(c, hm, cfg, 404, "text/plain", nf, strlen(nf));
    }
}

// Return true for addresses that are safe to bind to without opt-in.
static bool is_loopback_bind(const char *addr) {
    if (!addr) return false;
    // IPv4 loopback block + IPv6 loopback + bare "localhost" alias.
    if (strncmp(addr, "127.", 4) == 0) return true;
    if (strcmp(addr, "::1") == 0) return true;
    if (strcmp(addr, "localhost") == 0) return true;
    return false;
}

// Plain-HTTP listener wrapper. When the service is bound beyond loopback,
// redirect browsers to the HTTPS port instead of serving API traffic over
// plaintext — but only on safe methods. DYMO framework probes GET first;
// POSTs carrying labelXml must not be replayed, so we reject with 400
// rather than redirecting (redirecting a POST body is a known footgun).
static void handle_http_plain(struct mg_connection *c, int ev, void *ev_data) {
    const server_cfg_t *cfg = (const server_cfg_t *)c->fn_data;
    if (ev == MG_EV_HTTP_MSG && !is_loopback_bind(cfg->bind_addr)) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        // Build Location: https://<Host without :port>:<https_port><uri>
        struct mg_str *host_hdr = mg_http_get_header(hm, "Host");
        char host[256] = "127.0.0.1";
        if (host_hdr && host_hdr->len > 0 && host_hdr->len < sizeof(host)) {
            size_t n = host_hdr->len;
            memcpy(host, host_hdr->buf, n);
            host[n] = '\0';
            // Strip an existing :port so we can substitute ours.
            char *colon = strrchr(host, ':');
            if (colon) *colon = '\0';
        }

        if (method_is(hm, "GET") || method_is(hm, "HEAD") || method_is(hm, "OPTIONS")) {
            char loc[512];
            int n = snprintf(loc, sizeof(loc),
                "https://%s:%d%.*s", host, cfg->https_port,
                (int)hm->uri.len, hm->uri.buf);
            (void)n;
            mg_printf(c,
                "HTTP/1.1 301 Moved Permanently\r\n"
                "Location: %s\r\n"
                "Content-Length: 0\r\n"
                "\r\n", loc);
        } else {
            const char *msg = "use HTTPS\n";
            reply_text(c, hm, cfg, 400, "text/plain; charset=utf-8", msg, strlen(msg));
        }
        return;
    }
    handle_http(c, ev, ev_data);
}

// TLS credentials are read from disk once at startup and reused for every
// accepted HTTPS connection. Reading them per-accept works but leaks the
// buffers because mg_tls_init consumes them synchronously without taking
// ownership.
static struct mg_str g_tls_cert;
static struct mg_str g_tls_key;

// Listener wrapper that enables TLS on every accepted connection. mongoose
// consumes the cert/key PEM during mg_tls_init but holds pointers into them
// for the SSL session, so the buffers must outlive the accepted connection —
// hence the process-scoped g_tls_cert / g_tls_key loaded in server_run.
static void handle_https(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_ACCEPT) {
        struct mg_tls_opts opts = {
            .cert = g_tls_cert,
            .key  = g_tls_key,
        };
        mg_tls_init(c, &opts);
        return;
    }
    handle_http(c, ev, ev_data);
}

// ---- Public entry point -------------------------------------------------

int server_run(const server_cfg_t *cfg) {
    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    // Refuse to expose the service beyond loopback unless the operator has
    // acknowledged the risk. There is no authentication on any endpoint;
    // anyone who can reach a non-loopback bind can trigger prints.
    if (!is_loopback_bind(cfg->bind_addr) && !cfg->allow_lan) {
        LOG_ERROR("bind address %s is not loopback; pass --allow-lan (or set "
                  "DYMO_ALLOW_LAN=1) to expose this service beyond 127.0.0.1. "
                  "Note: the service has no authentication.", cfg->bind_addr);
        return -1;
    }
    if (!is_loopback_bind(cfg->bind_addr)) {
        LOG_WARN("binding to %s — the service is reachable from other hosts "
                 "and has NO AUTHENTICATION. Anyone with network access to "
                 "this port can print labels.", cfg->bind_addr);
    }

    // Load TLS material once; mongoose holds pointers into these buffers
    // for the life of the accepted SSL session.
    g_tls_cert = mg_file_read(&mg_fs_posix, cfg->cert_path);
    g_tls_key  = mg_file_read(&mg_fs_posix, cfg->key_path);
    if (g_tls_cert.len == 0 || g_tls_key.len == 0) {
        LOG_ERROR("failed to read cert=%s or key=%s", cfg->cert_path, cfg->key_path);
        return -1;
    }

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    mg_log_set(MG_LL_ERROR);

    char http_url[128], https_url[128];
    snprintf(http_url,  sizeof(http_url),  "http://%s:%d",  cfg->bind_addr, cfg->http_port);
    snprintf(https_url, sizeof(https_url), "http://%s:%d", cfg->bind_addr, cfg->https_port);

    // Mongoose uses "http://host:port" for both plain and TLS listeners; TLS
    // is enabled per-connection via mg_tls_init in MG_EV_ACCEPT.
    struct mg_connection *http_c  = mg_http_listen(&mgr, http_url,  handle_http_plain, (void*)cfg);
    struct mg_connection *https_c = mg_http_listen(&mgr, https_url, handle_https,      (void*)cfg);
    if (!http_c)  { LOG_ERROR("failed to bind %s",  http_url);  mg_mgr_free(&mgr); return -1; }
    if (!https_c) { LOG_ERROR("failed to bind %s", https_url); mg_mgr_free(&mgr); return -1; }

    LOG_INFO("listening HTTP  %s", http_url);
    LOG_INFO("listening HTTPS %s:%d (cert=%s)", cfg->bind_addr, cfg->https_port, cfg->cert_path);

    while (g_running) {
        mg_mgr_poll(&mgr, 200);
    }
    LOG_INFO("shutting down");
    mg_mgr_free(&mgr);
    return 0;
}
