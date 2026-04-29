// Native LibreSSL-libtls HTTPS listener.
//
// Why not use mongoose's TLS? mongoose's OpenSSL-compat BIO calls send() per
// TLS record, producing fragmented TCP packets (Handshake ServerHello in
// 6 small frames instead of one). The DYMO framework's XHR state machine
// stalls on that fragmentation. libtls writes directly to the socket fd and
// lets the kernel coalesce normally, so the wire behaviour matches what
// Python's uvicorn produces.
//
// Threading model: one OS thread per accepted connection, detached. Each
// thread handles exactly one HTTP request/response then closes. The worker
// count is implicitly bounded by the listen backlog (128) and by how fast
// the render/print pipeline runs.

#include "tls_server.h"
#include "http.h"
#include "log.h"
#include "render.h"
#include "printing.h"
#include "xml_parse.h"

#include <tls.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Body cap for /PrintLabel. Matches server.c. Also caps total bytes we buffer
// per connection — anything larger is rejected with 413.
#define MAX_BODY_BYTES (512 * 1024)
#define MAX_HEADER_BYTES (16 * 1024)
#define READ_TIMEOUT_SEC 10

// --- File helpers --------------------------------------------------------

// Best-effort write of `data` (NUL-terminated) to capture_dir/fname. Uses
// O_CREAT|O_EXCL|O_NOFOLLOW so a pre-planted symlink can't redirect the
// write; since capture_dir is timestamp+microseconds unique, the file
// shouldn't already exist.
static void dump_to_capture_file(const char *capture_dir, const char *fname,
                                 const char *data) {
    if (!data) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", capture_dir, fname);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    size_t remaining = strlen(data);
    const char *p = data;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        p += n;
        remaining -= (size_t)n;
    }
    close(fd);
}

// --- Connection helpers ---------------------------------------------------

typedef struct {
    struct tls *ctx;
    char read_buf[MAX_HEADER_BYTES + MAX_BODY_BYTES + 1];
    size_t read_len;
} conn_t;

// Read at least `min` more bytes into conn->read_buf. Returns bytes now
// buffered, or -1 on error/EOF. Blocks via tls_read (may return TLS_WANT_*).
static ssize_t conn_read_more(conn_t *conn, size_t min_additional) {
    size_t target = conn->read_len + min_additional;
    if (target > sizeof(conn->read_buf) - 1) return -1;

    while (conn->read_len < target) {
        ssize_t n = tls_read(conn->ctx,
                             conn->read_buf + conn->read_len,
                             sizeof(conn->read_buf) - 1 - conn->read_len);
        if (n == TLS_WANT_POLLIN || n == TLS_WANT_POLLOUT) continue;
        if (n <= 0) return -1;
        conn->read_len += (size_t)n;
    }
    return (ssize_t)conn->read_len;
}

// Read until we see "\r\n\r\n" (end of headers) or the buffer fills. Returns
// offset of the first body byte, or -1 on error/oversize.
static ssize_t read_until_headers(conn_t *conn) {
    while (1) {
        char *hdr_end = NULL;
        if (conn->read_len >= 4) {
            // Search only the most recently-added region; extend backwards
            // by 3 bytes to catch a CRLFCRLF split across reads.
            conn->read_buf[conn->read_len] = '\0';
            hdr_end = strstr(conn->read_buf, "\r\n\r\n");
        }
        if (hdr_end) return (hdr_end - conn->read_buf) + 4;
        if (conn->read_len >= MAX_HEADER_BYTES) return -1;

        ssize_t n = tls_read(conn->ctx,
                             conn->read_buf + conn->read_len,
                             sizeof(conn->read_buf) - 1 - conn->read_len);
        if (n == TLS_WANT_POLLIN || n == TLS_WANT_POLLOUT) continue;
        if (n <= 0) return -1;
        conn->read_len += (size_t)n;
    }
}

// Case-insensitive header lookup in the request header block [buf, buf+hdr_len).
// Sets *val_start and *val_len on success.
static bool find_header(const char *buf, size_t len, const char *name,
                        const char **val_start, size_t *val_len) {
    size_t nlen = strlen(name);
    const char *p = buf;
    const char *end = buf + len;
    while (p < end) {
        const char *line_end = memchr(p, '\n', end - p);
        if (!line_end) break;
        size_t line_len = line_end - p;
        if (line_len > 0 && p[line_len - 1] == '\r') line_len--;
        if (line_len > nlen && p[nlen] == ':' && strncasecmp(p, name, nlen) == 0) {
            const char *v = p + nlen + 1;
            while (v < p + line_len && (*v == ' ' || *v == '\t')) v++;
            *val_start = v;
            *val_len = (p + line_len) - v;
            return true;
        }
        p = line_end + 1;
    }
    return false;
}

// Write all bytes via tls_write, looping on TLS_WANT_*. Returns 0 on success.
static int conn_write_all(conn_t *conn, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = tls_write(conn->ctx, p, len);
        if (n == TLS_WANT_POLLIN || n == TLS_WANT_POLLOUT) continue;
        if (n < 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

// Build one complete HTTP/1.1 response into a caller-provided buffer and
// conn_write_all it in a single tls_write sequence. Coalescing into one
// buffer means libtls emits one (or very few) TCP segments — crucial for
// matching Python's on-wire behaviour.
static int write_response(conn_t *conn,
                          const server_cfg_t *cfg,
                          int code,
                          const char *status_text,
                          const char *origin, size_t origin_len,
                          bool origin_allowed_flag,
                          bool is_preflight,
                          const char *acrh, size_t acrh_len,
                          const char *ctype,
                          const void *body, size_t body_len) {
    char buf[8192];
    int off = 0;

    char date[64];
    time_t now = time(NULL);
    struct tm gmt;
    gmtime_r(&now, &gmt);
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &gmt);

    off += snprintf(buf + off, sizeof(buf) - off,
                    "HTTP/1.1 %d %s\r\n"
                    "Date: %s\r\n"
                    "Server: dymo-web-service\r\n",
                    code, status_text, date);

    if (origin_allowed_flag) {
        off += snprintf(buf + off, sizeof(buf) - off,
                        "Access-Control-Allow-Origin: %.*s\r\n"
                        "Access-Control-Allow-Credentials: true\r\n"
                        "Vary: Origin\r\n",
                        (int)origin_len, origin);
        if (is_preflight) {
            off += snprintf(buf + off, sizeof(buf) - off,
                            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                            "Access-Control-Allow-Headers: %.*s\r\n"
                            "Access-Control-Allow-Private-Network: true\r\n"
                            "Access-Control-Allow-Local-Network: true\r\n"
                            "Access-Control-Max-Age: 600\r\n",
                            acrh_len > 0 ? (int)acrh_len : (int)strlen("Content-Type"),
                            acrh_len > 0 ? acrh : "Content-Type");
        }
    }
    if (ctype) {
        off += snprintf(buf + off, sizeof(buf) - off, "Content-Type: %s\r\n", ctype);
    }
    off += snprintf(buf + off, sizeof(buf) - off,
                    "Content-Length: %zu\r\n\r\n", body_len);
    (void)cfg;

    if (off < 0 || (size_t)off >= sizeof(buf)) return -1;

    // Single write: header + body in one coalesced buffer.
    if (body_len > 0 && (size_t)off + body_len <= sizeof(buf)) {
        memcpy(buf + off, body, body_len);
        return conn_write_all(conn, buf, off + body_len);
    }
    // Header first, then body separately (only for large bodies).
    if (conn_write_all(conn, buf, off) != 0) return -1;
    if (body_len > 0) return conn_write_all(conn, body, body_len);
    return 0;
}

// --- Origin allowlist (duplicated from server.c for now) ------------------

static bool origin_in_list(const char *allowlist, const char *o, size_t olen) {
    if (!allowlist || !*allowlist || olen == 0) return false;
    const char *p = allowlist;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        size_t len = (size_t)(end - start);
        if (len == olen && memcmp(start, o, olen) == 0) return true;
    }
    return false;
}

// --- HTTP request dispatch ------------------------------------------------

static const char *status_phrase(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default:  return "Error";
    }
}

// Parse request line + header block. On success sets method, uri, origin, acrh,
// content_length, and hdr_end. Returns 0 on success.
typedef struct {
    const char *method;  size_t method_len;
    const char *uri;     size_t uri_len;
    const char *origin;  size_t origin_len;
    const char *acrh;    size_t acrh_len;  // Access-Control-Request-Headers
    long content_length;                   // -1 if absent
    size_t header_block_end;               // offset into conn->read_buf
} parsed_req_t;

static int parse_request_line(const char *buf, size_t len,
                              parsed_req_t *out) {
    // First line: METHOD SP URI SP PROTO CRLF
    const char *line_end = memchr(buf, '\n', len);
    if (!line_end) return -1;
    size_t line_len = line_end - buf;
    if (line_len > 0 && buf[line_len - 1] == '\r') line_len--;
    const char *sp1 = memchr(buf, ' ', line_len);
    if (!sp1) return -1;
    const char *sp2 = memchr(sp1 + 1, ' ', line_len - (sp1 - buf) - 1);
    if (!sp2) return -1;
    out->method     = buf;
    out->method_len = sp1 - buf;
    out->uri        = sp1 + 1;
    out->uri_len    = sp2 - (sp1 + 1);
    return 0;
}

// Handle one complete HTTP request. Returns 0 to keep connection open.
static int handle_request(conn_t *conn, const server_cfg_t *cfg,
                          const parsed_req_t *req, const char *body,
                          size_t body_len) {
    bool origin_allowed = false;
    if (req->origin_len > 0 &&
        origin_in_list(cfg->allowed_origins, req->origin, req->origin_len)) {
        origin_allowed = true;
    }

    bool is_options = req->method_len == 7 &&
                      memcmp(req->method, "OPTIONS", 7) == 0;
    bool is_get     = req->method_len == 3 &&
                      memcmp(req->method, "GET",     3) == 0;
    bool is_post    = req->method_len == 4 &&
                      memcmp(req->method, "POST",    4) == 0;

    // --- OPTIONS preflight ---
    if (is_options) {
        LOG_INFO("OPTIONS %.*s", (int)req->uri_len, req->uri);
        return write_response(conn, cfg, 204, status_phrase(204),
                              req->origin, req->origin_len,
                              origin_allowed, true,
                              req->acrh, req->acrh_len,
                              NULL, NULL, 0);
    }

    // --- Route matching ---
    const char *uri = req->uri;
    size_t ul = req->uri_len;
    LOG_INFO("%.*s %.*s",
             (int)req->method_len, req->method,
             (int)ul, uri);

    if (is_get && ul == 1 && uri[0] == '/') {
        const char *b = "DYMO Web Service (Linux emulator, C, libtls)";
        return write_response(conn, cfg, 200, status_phrase(200),
                              req->origin, req->origin_len,
                              origin_allowed, false, NULL, 0,
                              "text/plain; charset=utf-8", b, strlen(b));
    }
    if (is_get && ul == strlen("/DYMO/DLS/Printing/Check") &&
        memcmp(uri, "/DYMO/DLS/Printing/Check", ul) == 0) {
        return write_response(conn, cfg, 200, status_phrase(200),
                              req->origin, req->origin_len,
                              origin_allowed, false, NULL, 0,
                              "text/plain; charset=utf-8", "true", 4);
    }
    if (is_get && ul == strlen("/DYMO/DLS/Printing/StatusConnected") &&
        memcmp(uri, "/DYMO/DLS/Printing/StatusConnected", ul) == 0) {
        return write_response(conn, cfg, 200, status_phrase(200),
                              req->origin, req->origin_len,
                              origin_allowed, false, NULL, 0,
                              "text/plain; charset=utf-8", "true", 4);
    }
    if (is_get && ul == strlen("/DYMO/DLS/Printing/GetPrinters") &&
        memcmp(uri, "/DYMO/DLS/Printing/GetPrinters", ul) == 0) {
        char xml[1024];
        const char *name = cfg->reported_name ? cfg->reported_name
                                              : "DYMO LabelWriter 450 Turbo";
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
        return write_response(conn, cfg, 200, status_phrase(200),
                              req->origin, req->origin_len,
                              origin_allowed, false, NULL, 0,
                              "application/xml", xml, (size_t)n);
    }
    if (is_post && ul == strlen("/DYMO/DLS/Printing/RenderLabel") &&
        memcmp(uri, "/DYMO/DLS/Printing/RenderLabel", ul) == 0) {
        // Empty body — the framework doesn't parse it.
        return write_response(conn, cfg, 200, status_phrase(200),
                              req->origin, req->origin_len,
                              origin_allowed, false, NULL, 0,
                              "text/plain; charset=utf-8", "", 0);
    }
    if (is_post && ul == strlen("/DYMO/DLS/Printing/PrintLabel") &&
        memcmp(uri, "/DYMO/DLS/Printing/PrintLabel", ul) == 0) {
        char *printer = form_get(body, body_len, "printerName");
        char *label_xml = form_get(body, body_len, "labelXml");
        char *set_xml = form_get(body, body_len, "labelSetXml");

        // Capture to disk if enabled.
        char capture_dir[512];
        capture_dir[0] = '\0';
        if (cfg->capture_payloads && cfg->log_dir) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            struct tm tm;
            localtime_r(&ts.tv_sec, &tm);
            char stamp[32];
            strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
            snprintf(capture_dir, sizeof(capture_dir),
                     "%s/printlabel-%s-%06ld", cfg->log_dir, stamp,
                     (long)(ts.tv_nsec / 1000));
            mkdir(cfg->log_dir, 0750);
            if (mkdir(capture_dir, 0700) != 0) {
                LOG_WARN("mkdir %s: %s", capture_dir, strerror(errno));
                capture_dir[0] = '\0';
            } else {
                // dump files — best-effort; diagnostic captures, not critical
                // path for the print itself.
                dump_to_capture_file(capture_dir, "label.xml", label_xml);
                dump_to_capture_file(capture_dir, "labelSet.xml", set_xml);
                dump_to_capture_file(capture_dir, "printerName.txt", printer);
            }
        }

        bool ok = false;
        if (label_xml && set_xml && capture_dir[0]) {
            label_t *lab = parse_label_xml(label_xml, strlen(label_xml));
            label_set_t *set = parse_label_set_xml(set_xml, strlen(set_xml));
            if (lab && set) {
                char **paths = NULL;
                size_t n = 0;
                if (render_all(lab, set, capture_dir, &paths, &n) == 0 && n > 0) {
                    bool all_ok = true;
                    for (size_t i = 0; i < n; i++) {
                        if (print_label_png(paths[i], cfg->printer_name) != 0)
                            all_ok = false;
                        else
                            LOG_INFO("printed %s", paths[i]);
                        free(paths[i]);
                    }
                    free(paths);
                    ok = all_ok;
                }
            }
            if (lab) label_free(lab);
            if (set) label_set_free(set);
        }
        free(printer);
        free(label_xml);
        free(set_xml);

        const char *body_out = ok ? "true" : "false";
        int code = ok ? 200 : 500;
        return write_response(conn, cfg, code, status_phrase(code),
                              req->origin, req->origin_len,
                              origin_allowed, false, NULL, 0,
                              "text/plain; charset=utf-8",
                              body_out, strlen(body_out));
    }

    const char *nf = "Not found";
    return write_response(conn, cfg, 404, status_phrase(404),
                          req->origin, req->origin_len,
                          origin_allowed, false, NULL, 0,
                          "text/plain", nf, strlen(nf));
}

// Worker thread argv.
typedef struct {
    struct tls *listen_ctx;
    int client_fd;
    const server_cfg_t *cfg;
} worker_arg_t;

static void *worker(void *a) {
    worker_arg_t *wa = (worker_arg_t *)a;
    conn_t conn = {0};

    if (tls_accept_socket(wa->listen_ctx, &conn.ctx, wa->client_fd) != 0) {
        LOG_WARN("tls_accept_socket: %s", tls_error(wa->listen_ctx));
        close(wa->client_fd);
        free(wa);
        return NULL;
    }

    // Force the handshake to complete up front so any handshake failure is
    // surfaced before we try to dispatch. libtls will otherwise run it
    // on first read/write.
    int hs;
    while ((hs = tls_handshake(conn.ctx)) == TLS_WANT_POLLIN ||
           hs == TLS_WANT_POLLOUT) { /* spin */ }
    if (hs != 0) {
        LOG_WARN("tls_handshake: %s", tls_error(conn.ctx));
        goto done;
    }

    // Read until end of headers.
    ssize_t body_off = read_until_headers(&conn);
    if (body_off < 0) goto done;

    parsed_req_t req = {0};
    if (parse_request_line(conn.read_buf, body_off, &req) != 0) goto done;

    const char *hdr_block = conn.read_buf;
    size_t hdr_block_len = (size_t)body_off;
    if (find_header(hdr_block, hdr_block_len, "Origin",
                    &req.origin, &req.origin_len)) {}
    if (find_header(hdr_block, hdr_block_len, "Access-Control-Request-Headers",
                    &req.acrh, &req.acrh_len)) {}
    req.content_length = -1;
    const char *cl_v; size_t cl_l;
    if (find_header(hdr_block, hdr_block_len, "Content-Length", &cl_v, &cl_l)) {
        char tmp[32];
        size_t n = cl_l < sizeof(tmp) - 1 ? cl_l : sizeof(tmp) - 1;
        memcpy(tmp, cl_v, n); tmp[n] = '\0';
        req.content_length = strtol(tmp, NULL, 10);
    }

    // Read the body if any.
    const char *body_ptr = NULL;
    size_t body_len = 0;
    if (req.content_length > 0) {
        if (req.content_length > MAX_BODY_BYTES) {
            const char *msg = "body too large\n";
            write_response(&conn, wa->cfg, 413, status_phrase(413),
                           NULL, 0, false, false, NULL, 0,
                           "text/plain", msg, strlen(msg));
            goto done;
        }
        size_t already = conn.read_len - (size_t)body_off;
        size_t need = (size_t)req.content_length > already
                      ? (size_t)req.content_length - already : 0;
        if (need > 0) {
            if (conn_read_more(&conn, need) < 0) goto done;
        }
        body_ptr = conn.read_buf + body_off;
        body_len = (size_t)req.content_length;
    }

    handle_request(&conn, wa->cfg, &req, body_ptr, body_len);

done:
    if (conn.ctx) {
        // libtls close is two-phase; spin on WANT_*.
        int rc;
        while ((rc = tls_close(conn.ctx)) == TLS_WANT_POLLIN ||
               rc == TLS_WANT_POLLOUT) {}
        tls_free(conn.ctx);
    }
    close(wa->client_fd);
    free(wa);
    return NULL;
}

// --- Public entry point ---------------------------------------------------

int tls_server_run(const server_cfg_t *cfg, volatile int *running) {
    if (tls_init() != 0) {
        LOG_ERROR("tls_init failed");
        return -1;
    }

    struct tls_config *tc = tls_config_new();
    if (!tc) { LOG_ERROR("tls_config_new failed"); return -1; }

    if (tls_config_set_cert_file(tc, cfg->cert_path) != 0) {
        LOG_ERROR("tls_config_set_cert_file(%s): %s", cfg->cert_path,
                  tls_config_error(tc));
        tls_config_free(tc); return -1;
    }
    if (tls_config_set_key_file(tc, cfg->key_path) != 0) {
        LOG_ERROR("tls_config_set_key_file(%s): %s", cfg->key_path,
                  tls_config_error(tc));
        tls_config_free(tc); return -1;
    }
    // tls_server contexts do not require client certs by default, so there's
    // no verification to disable.

    struct tls *listen_ctx = tls_server();
    if (!listen_ctx) {
        LOG_ERROR("tls_server failed");
        tls_config_free(tc); return -1;
    }
    if (tls_configure(listen_ctx, tc) != 0) {
        LOG_ERROR("tls_configure: %s", tls_error(listen_ctx));
        tls_free(listen_ctx); tls_config_free(tc); return -1;
    }
    tls_config_free(tc);  // ref-counted; listen_ctx keeps its own

    // Bind + listen.
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { LOG_ERROR("socket: %s", strerror(errno)); return -1; }
    int one = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)cfg->https_port);
    if (inet_pton(AF_INET, cfg->bind_addr, &sa.sin_addr) != 1) {
        LOG_ERROR("invalid bind address: %s", cfg->bind_addr);
        close(sfd); return -1;
    }
    if (bind(sfd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        LOG_ERROR("bind(%s:%d): %s", cfg->bind_addr, cfg->https_port,
                  strerror(errno));
        close(sfd); return -1;
    }
    if (listen(sfd, 128) != 0) {
        LOG_ERROR("listen: %s", strerror(errno));
        close(sfd); return -1;
    }
    LOG_INFO("listening HTTPS %s:%d (libtls, cert=%s)",
             cfg->bind_addr, cfg->https_port, cfg->cert_path);

    // Make the listen socket non-blocking so accept() can honour *running.
    int flags = fcntl(sfd, F_GETFL, 0);
    fcntl(sfd, F_SETFL, flags | O_NONBLOCK);

    while (*running) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int cfd = accept(sfd, (struct sockaddr *)&cli, &cli_len);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000000 }; // 100ms
                nanosleep(&ts, NULL);
                continue;
            }
            if (errno == EINTR) continue;
            LOG_WARN("accept: %s", strerror(errno));
            continue;
        }

        // Per-connection: blocking IO again (inside libtls). TCP_NODELAY off —
        // we want Nagle to coalesce if anything.
        int zero = 0;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &zero, sizeof(zero));

        worker_arg_t *wa = malloc(sizeof(*wa));
        if (!wa) { close(cfd); continue; }
        wa->listen_ctx = listen_ctx;
        wa->client_fd = cfd;
        wa->cfg = cfg;

        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &attr, worker, wa) != 0) {
            LOG_WARN("pthread_create: %s", strerror(errno));
            close(cfd);
            free(wa);
        }
        pthread_attr_destroy(&attr);
    }

    close(sfd);
    tls_free(listen_ctx);
    return 0;
}
