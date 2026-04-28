#ifndef DYMO_SERVER_H
#define DYMO_SERVER_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *bind_addr;       // "127.0.0.1" (default) or "0.0.0.0"
    int http_port;               // 41951
    int https_port;              // 41952
    const char *cert_path;       // PEM cert path
    const char *key_path;        // PEM key path
    const char *printer_name;    // CUPS destination, default "LabelWriter-450-Turbo"
    const char *reported_name;   // Reported in GetPrinters XML, default "DYMO LabelWriter 450 Turbo"
    const char *log_dir;         // where to capture PrintLabel payloads
    bool capture_payloads;       // write label.xml / labelSet.xml / label-*.png

    // Comma-separated list of origins allowed for CORS. Default:
    // "https://www.cellartracker.com". Requests with an Origin header not in
    // this list get no CORS response headers (browsers block the request).
    const char *allowed_origins;

    // Non-loopback binds are dangerous (no auth). Require an explicit opt-in.
    bool allow_lan;

    // Hard cap on request body size. Bodies larger than this are rejected
    // with 413. Default: 256 KiB.
    size_t max_body_bytes;
} server_cfg_t;

int server_run(const server_cfg_t *cfg);

#endif
