// Entry point for dymo-web-service (C).
//
// CLI flags are minimal; most config lives in /etc/dymo-web-service/dymo-web-service.conf
// (simple KEY=VALUE, one per line), which the systemd unit uses via EnvironmentFile.

#include "log.h"
#include "server.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --bind ADDR            Bind address (default: 127.0.0.1)\n"
        "  --allow-lan            Permit non-loopback bind (no auth — DANGEROUS)\n"
        "  --http-port N          HTTP port (default: 41951)\n"
        "  --https-port N         HTTPS port (default: 41952)\n"
        "  --cert PATH            Path to PEM cert (default: /etc/dymo-web-service/cert.pem)\n"
        "  --key PATH             Path to PEM key  (default: /etc/dymo-web-service/key.pem)\n"
        "  --printer NAME         CUPS printer destination (default: LabelWriter-450-Turbo)\n"
        "  --reported-name NAME   Printer name reported to the browser\n"
        "                         (default: DYMO LabelWriter 450 Turbo)\n"
        "  --allowed-origins CSV  CORS allowlist, comma-separated\n"
        "                         (default: https://www.cellartracker.com)\n"
        "  --max-body-bytes N     Reject POST bodies larger than N (default: 262144)\n"
        "  --log-dir DIR          Directory for captured payloads\n"
        "                         (default: /var/log/dymo-web-service)\n"
        "  --no-capture           Don't write PrintLabel payloads to log-dir\n"
        "  --verbose              Enable DEBUG logging\n"
        "  --help                 Show this help\n",
        argv0);
}

// Parse a TCP port from a string: returns -1 on invalid, else [1, 65535].
static int parse_port(const char *s) {
    if (!s || !*s) return -1;
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || !end || *end != '\0') return -1;
    if (v < 1 || v > 65535) return -1;
    return (int)v;
}

// Parse a non-negative size from a string: returns (size_t)-1 on invalid.
static size_t parse_size(const char *s) {
    if (!s || !*s) return (size_t)-1;
    char *end = NULL;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno || !end || *end != '\0') return (size_t)-1;
    if (v < 0 || (unsigned long long)v > (unsigned long long)SIZE_MAX) return (size_t)-1;
    return (size_t)v;
}

static const char *env_or_default(const char *name, const char *def) {
    const char *v = getenv(name);
    return (v && *v) ? v : def;
}

static bool env_flag(const char *name) {
    const char *v = getenv(name);
    if (!v || !*v) return false;
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T';
}

int main(int argc, char **argv) {
    server_cfg_t cfg = {
        .bind_addr        = env_or_default("DYMO_BIND", "127.0.0.1"),
        .http_port        = 41951,
        .https_port       = 41952,
        .cert_path        = env_or_default("DYMO_CERT", "/etc/dymo-web-service/cert.pem"),
        .key_path         = env_or_default("DYMO_KEY",  "/etc/dymo-web-service/key.pem"),
        .printer_name     = env_or_default("DYMO_PRINTER", "LabelWriter-450-Turbo"),
        .reported_name    = env_or_default("DYMO_REPORTED_NAME", "DYMO LabelWriter 450 Turbo"),
        .log_dir          = env_or_default("DYMO_LOG_DIR", "/var/log/dymo-web-service"),
        .capture_payloads = true,
        .allowed_origins  = env_or_default("DYMO_ALLOWED_ORIGINS",
                                           "https://www.cellartracker.com"),
        .allow_lan        = env_flag("DYMO_ALLOW_LAN"),
        .max_body_bytes   = 256 * 1024,
    };

    // Apply env-supplied numeric fields (validated — reject garbage).
    const char *env_http  = getenv("DYMO_HTTP_PORT");
    const char *env_https = getenv("DYMO_HTTPS_PORT");
    const char *env_max   = getenv("DYMO_MAX_BODY_BYTES");
    if (env_http) {
        int p = parse_port(env_http);
        if (p < 0) { fprintf(stderr, "invalid DYMO_HTTP_PORT: %s\n", env_http); return 2; }
        cfg.http_port = p;
    }
    if (env_https) {
        int p = parse_port(env_https);
        if (p < 0) { fprintf(stderr, "invalid DYMO_HTTPS_PORT: %s\n", env_https); return 2; }
        cfg.https_port = p;
    }
    if (env_max) {
        size_t m = parse_size(env_max);
        if (m == (size_t)-1) { fprintf(stderr, "invalid DYMO_MAX_BODY_BYTES: %s\n", env_max); return 2; }
        cfg.max_body_bytes = m;
    }

    static struct option opts[] = {
        {"bind",            required_argument, 0, 'b'},
        {"allow-lan",       no_argument,       0, 'A'},
        {"http-port",       required_argument, 0, 'H'},
        {"https-port",      required_argument, 0, 'S'},
        {"cert",            required_argument, 0, 'c'},
        {"key",             required_argument, 0, 'k'},
        {"printer",         required_argument, 0, 'p'},
        {"reported-name",   required_argument, 0, 'n'},
        {"allowed-origins", required_argument, 0, 'O'},
        {"max-body-bytes",  required_argument, 0, 'M'},
        {"log-dir",         required_argument, 0, 'L'},
        {"no-capture",      no_argument,       0, 'C'},
        {"verbose",         no_argument,       0, 'v'},
        {"help",            no_argument,       0, 'h'},
        {0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "b:AH:S:c:k:p:n:O:M:L:Cvh", opts, NULL)) != -1) {
        switch (c) {
            case 'b': cfg.bind_addr = optarg; break;
            case 'A': cfg.allow_lan = true; break;
            case 'H': {
                int p = parse_port(optarg);
                if (p < 0) { fprintf(stderr, "invalid --http-port: %s\n", optarg); return 2; }
                cfg.http_port = p;
            } break;
            case 'S': {
                int p = parse_port(optarg);
                if (p < 0) { fprintf(stderr, "invalid --https-port: %s\n", optarg); return 2; }
                cfg.https_port = p;
            } break;
            case 'c': cfg.cert_path = optarg; break;
            case 'k': cfg.key_path = optarg; break;
            case 'p': cfg.printer_name = optarg; break;
            case 'n': cfg.reported_name = optarg; break;
            case 'O': cfg.allowed_origins = optarg; break;
            case 'M': {
                size_t m = parse_size(optarg);
                if (m == (size_t)-1) { fprintf(stderr, "invalid --max-body-bytes: %s\n", optarg); return 2; }
                cfg.max_body_bytes = m;
            } break;
            case 'L': cfg.log_dir = optarg; break;
            case 'C': cfg.capture_payloads = false; break;
            case 'v': log_set_level(LOG_LVL_DEBUG); break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 2;
        }
    }

    return server_run(&cfg) == 0 ? 0 : 1;
}
