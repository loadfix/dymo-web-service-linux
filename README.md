# dymo-web-service-linux

A Linux implementation of the DYMO Connect web service. Lets browser code
that uses `dymo.connect.framework.js` — most notably
[CellarTracker](https://www.cellartracker.com/) — print to a DYMO
LabelWriter attached to a Linux machine.

DYMO ships a "Connect for Desktop" daemon on Windows and macOS that
listens on `127.0.0.1:41951/41952` and speaks a small HTTP/HTTPS API.
Browser pages load DYMO's framework JS, which POSTs label XML to that
local service. On Linux there is no such daemon — this project is one.

---

## How it fits together

```
┌─────────────┐    HTTPS     ┌──────────────┐    exec      ┌──────────┐   USB
│   Browser   │ ───────────► │ dymo-web-    │ ───────────► │   CUPS   │ ──────► LabelWriter
│ (userscript │              │ service      │              │ (filters │
│  patches    │              │ (this repo)  │              │ + backend)│
│  framework) │              │ port 41952   │              └──────────┘
└─────────────┘              └──────────────┘
```

Three pieces have to cooperate:

1. **The daemon** (this repo) — a small C HTTP/HTTPS server that emulates
   DYMO Connect's API, renders `.label` XML + LabelSet to a 1-bit PNG,
   and hands it to CUPS via `lp`.
2. **The userscript** at
   [`userscript/cellartracker-dymo-linux.user.js`](userscript/cellartracker-dymo-linux.user.js)
   — CellarTracker's page-level JS refuses to run on Linux before
   contacting the service. The userscript patches the framework in-page
   so it stops refusing.
3. **A patched DYMO CUPS driver** — the stock Ubuntu `printer-driver-dymo`
   has a zombie-job bug that leaves `lpstat` stuck on "now printing"
   after a successful print. A fork with that bug fixed lives at
   [loadfix/dymo-cups-drivers](https://github.com/loadfix/dymo-cups-drivers).

---

## What it does

The daemon implements the endpoints the DYMO framework probes and POSTs
to:

| Method | Path | Response |
|---|---|---|
| `GET` | `/DYMO/DLS/Printing/Check` | `"true"` |
| `GET` | `/DYMO/DLS/Printing/StatusConnected` | `"true"` |
| `GET` | `/DYMO/DLS/Printing/GetPrinters` | `<Printers><LabelWriterPrinter>…</LabelWriterPrinter></Printers>` |
| `POST` | `/DYMO/DLS/Printing/PrintLabel` | Parses `labelXml` + `labelSetXml`, renders each `LabelRecord` to a 1-bit PNG at 300 DPI, submits to CUPS via `lp` |
| `POST` | `/DYMO/DLS/Printing/RenderLabel` | Empty (stubbed; framework only needs it to proceed) |

Rendering handles the subset of the `.label` format CellarTracker uses:

- `DieCutLabel` with twip units, Landscape orientation
- `TextObject` with `StyledText`, `Bounds`, alignment, rotation, `TextFitMode=ShrinkToFit`
- `BarcodeObject` with `Code2of5` (Interleaved 2 of 5), `Code128`, `QRCode`
- `<ObjectData Name="…">` substitution from the LabelSet

Byte discipline matters for barcode scannability: bars use integer-pixel
module widths, the final canvas is thresholded to true 1-bit before being
handed to the driver (paired with `DymoHalftoning=NLL`), and the image
orientation is fixed before CUPS so no resampling happens downstream.

---

## Dependencies

**Runtime** (from the stock Ubuntu 26.04 archive):

- `cups-client` — for `/usr/bin/lp`
- `printer-driver-dymo` — the raster filter; use the
  [loadfix fork](https://github.com/loadfix/dymo-cups-drivers) if you
  hit zombie jobs
- `libcairo2`, `libpango-1.0-0`, `libpangocairo-1.0-0`, `libqrencode4`,
  `libexpat1`

**Build-time** (also from the Ubuntu archive):

- `build-essential`, `pkg-config`, `debhelper (>= 13)`
- `libcairo2-dev`, `libpango1.0-dev`, `libqrencode-dev`, `libexpat1-dev`
- `curl`, `ca-certificates` — for fetching pinned tarballs

TLS and HTTP are not in this list:

- **[LibreSSL 4.3.1](https://libressl.org/)** is fetched at build time
  (SHA256-pinned in the Makefile), built with `--disable-shared`, and
  statically linked into the binary. The resulting `.deb` has no
  `libssl3`/`libssl-dev` runtime dependency.
- **[Mongoose 7.21](https://github.com/cesanta/mongoose/)** is fetched at
  build time (commit SHA pinned, both files SHA256-checked) and compiled
  into the daemon as a single translation unit. One small patch is
  applied (`third_party/patches/mongoose-bio-init.patch`) to make the
  custom BIO work with LibreSSL's stricter init check.

---

## Build

```sh
sudo apt-get install -y \
    build-essential pkg-config debhelper \
    libcairo2-dev libpango1.0-dev libqrencode-dev libexpat1-dev \
    curl ca-certificates

make bootstrap    # downloads + verifies LibreSSL and mongoose
make              # builds ./build/dymo-web-service
```

To build a `.deb`:

```sh
make deb          # runs dpkg-buildpackage -us -uc -b
```

The resulting `dymo-web-service_*_amd64.deb` lands in the parent
directory and can be installed with `sudo dpkg -i`.

---

## Install

```sh
sudo dpkg -i dymo-web-service_*_amd64.deb
sudo apt-get install -f    # pull in any missing runtime deps
```

The `postinst` script creates:

- a `dymoweb` system user (no supplementary groups — `/usr/bin/lp` works
  over a UNIX-domain socket without `lp` group membership),
- a self-signed TLS cert at `/etc/dymo-web-service/cert.pem` (valid for
  `localhost` and `127.0.0.1`, 10-year expiry),
- `/var/log/dymo-web-service/` (0700, owned by `dymoweb`),
- a systemd unit at `/lib/systemd/system/dymo-web-service.service`
  (hardened: `ProtectSystem=strict`, empty `CapabilityBoundingSet`,
  `SystemCallFilter=@system-service`, `IPAddressAllow=localhost`, etc.)

### Two manual steps after install

**1. Trust the cert.** The daemon serves HTTPS with a self-signed cert.
Browsers block same-origin mixed content and the DYMO framework probes
HTTPS before HTTP, so the cert has to be pre-trusted:

- *Firefox:* Settings → Privacy & Security → Certificates → View
  Certificates → Authorities → Import `/etc/dymo-web-service/cert.pem`.
  Check "Trust this CA to identify websites."
- *Chrome/Chromium:* `chrome://settings/certificates` → Authorities → Import.

**2. Install the userscript.** CellarTracker's JS does an OS sniff and
refuses to run on Linux *before* it probes the local service. A
userscript manager (Violentmonkey / Tampermonkey / Greasemonkey) is
required to patch the framework in-page. Navigate to:

```
file:///usr/share/dymo-web-service/cellartracker-dymo-linux.user.js
```

— the userscript manager will offer to install. Then visit
CellarTracker and verify in devtools:

```
[dymo-linux] framework patched — Linux web service enabled
```

---

## Configuration

`/etc/dymo-web-service/dymo-web-service.conf` — KEY=VALUE pairs read by
systemd via `EnvironmentFile=`. Restart after editing:

```sh
sudo systemctl restart dymo-web-service
```

| Key | Default | Notes |
|---|---|---|
| `DYMO_BIND` | `127.0.0.1` | Set to `0.0.0.0` only with `DYMO_ALLOW_LAN=1` (the daemon otherwise refuses to start — **there is no authentication**) |
| `DYMO_HTTP_PORT` | `41951` | |
| `DYMO_HTTPS_PORT` | `41952` | |
| `DYMO_CERT` / `DYMO_KEY` | `/etc/dymo-web-service/{cert,key}.pem` | Regenerate with `openssl req -x509 …` if you need a different CN/SAN |
| `DYMO_PRINTER` | `LabelWriter-450-Turbo` | CUPS destination (`lpstat -p`) |
| `DYMO_REPORTED_NAME` | `DYMO LabelWriter 450 Turbo` | Name shown to the browser |
| `DYMO_ALLOWED_ORIGINS` | `https://www.cellartracker.com` | CORS allowlist (comma-separated). Unknown origins get no CORS headers and the browser blocks the request |
| `DYMO_MAX_BODY_BYTES` | `262144` | POSTs larger than this get `413` |
| `DYMO_LOG_DIR` | `/var/log/dymo-web-service` | Per-request subdirs capture the raw payload + rendered PNGs |

---

## Troubleshooting

**"Failed to connect to LabelWriter" in CellarTracker**

- Is the daemon running? `systemctl status dymo-web-service`
- Is the cert trusted? Visiting `https://127.0.0.1:41952/DYMO/DLS/Printing/Check`
  should show `true` without a cert warning.
- Is the userscript loaded? Console should show
  `[dymo-linux] framework patched`.

**"now printing" stuck forever after a successful print**

This is the zombie-job bug in the stock `printer-driver-dymo`. Use the
[loadfix fork](https://github.com/loadfix/dymo-cups-drivers), or recover
with:

```sh
cancel -a <printer> && sudo systemctl restart cups
```

**Logs**

```sh
journalctl -u dymo-web-service -f
ls /var/log/dymo-web-service/                  # captured payloads
```

---

## License

MIT — see [LICENSE](LICENSE).

Bundled third-party code retains its own terms:

- **LibreSSL** (OpenBSD team) — ISC / BSD / OpenSSL licenses; fetched at build time.
- **Mongoose** (Cesanta) — GPLv2 (dual-licensed commercially); fetched at build time, shipped as source inside the daemon.
