# DYMO Web Service (C port) — top-level Makefile.
#
# Builds a statically-LibreSSL-linked daemon that emulates DYMO Connect for
# CellarTracker on Linux. Produces ./build/dymo-web-service and, via
# `make deb`, a .deb installable on the target system.

PREFIX        ?= /usr
BINDIR        ?= $(PREFIX)/bin
SHAREDIR      ?= $(PREFIX)/share/dymo-web-service
SYSCONFDIR    ?= /etc/dymo-web-service
SYSTEMDDIR    ?= /lib/systemd/system

CC            ?= cc
PKG_CONFIG    ?= pkg-config

# ---------------------------------------------------------------------------
# Pinned third-party versions
# ---------------------------------------------------------------------------
#
# SHA256s are enforced — the build fails if the download doesn't match. Bump
# the version and SHA together; do not skip verification. Upstream publishes
# hashes at:
#   https://ftp.openbsd.org/pub/OpenBSD/LibreSSL/SHA256
#   https://github.com/cesanta/mongoose/releases/tag/<tag>
# or (for mongoose) derive the commit SHA via the GitHub API.

LIBRESSL_VERSION := 4.3.1
LIBRESSL_SHA256  := c2db42ace14e7d5419826fab35a742ec6e4d12725a051a51d0cea3c10ba0fa50
LIBRESSL_TARBALL := libressl-$(LIBRESSL_VERSION).tar.gz
LIBRESSL_URL     := https://ftp.openbsd.org/pub/OpenBSD/LibreSSL/$(LIBRESSL_TARBALL)

# mongoose is a single-file amalgamation; we pin to a specific commit SHA to
# freeze the content. The repo tags (7.x) are human-readable and resolvable
# to the same commit.
MONGOOSE_VERSION := 7.21
MONGOOSE_SHA     := b1c2ffe1a0aa13e3d94075b1a2c66b8b43ac9116
MONGOOSE_URL     := https://raw.githubusercontent.com/cesanta/mongoose/$(MONGOOSE_SHA)
# Hashes of the two files at the pinned commit (sha256). If these drift, the
# upstream mirror has been tampered with — the build fails closed.
MONGOOSE_C_SHA256 := cb23c6e2782eb6a115beb0bb41d66dfb31ccf9eeb4b0950788895dc9963ccda3
MONGOOSE_H_SHA256 := 008e31c8006e42983e0f3d7efbf123101de817d9eabe55a2c051159d2da59f19

# ---------------------------------------------------------------------------
# Derived paths
# ---------------------------------------------------------------------------

BUILD_DIR        := build
THIRD_PARTY_DIR  := third_party
LIBRESSL_DIR     := $(THIRD_PARTY_DIR)/libressl
LIBRESSL_SRCDIR  := $(LIBRESSL_DIR)/libressl-$(LIBRESSL_VERSION)
LIBRESSL_PREFIX  := $(abspath $(LIBRESSL_DIR)/install)
LIBRESSL_STAMP   := $(LIBRESSL_DIR)/.built-$(LIBRESSL_VERSION)
LIBRESSL_LIBS    := $(LIBRESSL_PREFIX)/lib/libtls.a $(LIBRESSL_PREFIX)/lib/libssl.a $(LIBRESSL_PREFIX)/lib/libcrypto.a

MONGOOSE_DIR     := $(THIRD_PARTY_DIR)/mongoose
MONGOOSE_SRC     := $(MONGOOSE_DIR)/mongoose.c
MONGOOSE_HDR     := $(MONGOOSE_DIR)/mongoose.h

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------

# Cairo/Pango/expat/qrencode from pkg-config. We bake LibreSSL's include and
# link paths on top.
PKG_DEPS         := cairo pangocairo libqrencode expat
PKG_CFLAGS       := $(shell $(PKG_CONFIG) --cflags $(PKG_DEPS))
PKG_LIBS         := $(shell $(PKG_CONFIG) --libs $(PKG_DEPS))

WARN_FLAGS       := -Wall -Wextra -Wno-unused-parameter -Wpointer-arith -Wshadow
OPT_FLAGS        ?= -O2 -g
STD_FLAGS        := -std=c11 -D_GNU_SOURCE

# We no longer use mongoose's TLS at all — HTTPS is served by src/tls_server.c
# using LibreSSL's native libtls API, which writes directly to the socket fd
# and matches the on-wire behaviour of uvicorn / mainstream servers.
# Mongoose is still used for the plain-HTTP port 41951.
MONGOOSE_FLAGS   := \
	-DMG_TLS=MG_TLS_NONE \
	-DMG_ENABLE_PACKED_FS=0 \
	-DMG_ENABLE_DIRLIST=0

CFLAGS_BASE      := $(STD_FLAGS) $(WARN_FLAGS) $(OPT_FLAGS) \
	$(MONGOOSE_FLAGS) \
	-Isrc \
	-I$(MONGOOSE_DIR) \
	-I$(LIBRESSL_PREFIX)/include \
	$(PKG_CFLAGS)

# Static-link LibreSSL; dynamic-link everything else. -lpthread for mongoose,
# -ldl for glibc fallbacks pulled in by LibreSSL, -lm for Cairo.
LDFLAGS_BASE     :=
LDLIBS_BASE      := \
	$(LIBRESSL_LIBS) \
	$(PKG_LIBS) \
	-lpthread -ldl -lm

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

SRC_FILES := \
	src/main.c \
	src/server.c \
	src/tls_server.c \
	src/http.c \
	src/xml_parse.c \
	src/render.c \
	src/barcode.c \
	src/printing.c \
	src/log.c

OBJ_FILES := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRC_FILES)) \
	$(BUILD_DIR)/third_party/mongoose/mongoose.o

TARGET := $(BUILD_DIR)/dymo-web-service

# ---------------------------------------------------------------------------
# Default target
# ---------------------------------------------------------------------------

.PHONY: all
all: $(TARGET)

$(TARGET): $(OBJ_FILES) $(LIBRESSL_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS_BASE) -o $@ $(OBJ_FILES) $(LDLIBS_BASE)

$(BUILD_DIR)/%.o: %.c | $(LIBRESSL_STAMP) $(MONGOOSE_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_BASE) -c -o $@ $<

# Mongoose has its own compile unit — its warnings are noisy, so relax flags.
$(BUILD_DIR)/third_party/mongoose/mongoose.o: $(MONGOOSE_SRC) $(LIBRESSL_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(STD_FLAGS) $(OPT_FLAGS) $(MONGOOSE_FLAGS) \
		-I$(MONGOOSE_DIR) -I$(LIBRESSL_PREFIX)/include \
		-Wno-unused-function -Wno-unused-variable -Wno-sign-compare \
		-c -o $@ $<

# ---------------------------------------------------------------------------
# Mongoose fetch (single-file HTTP server + TLS glue)
# ---------------------------------------------------------------------------

$(MONGOOSE_SRC) $(MONGOOSE_HDR): | $(MONGOOSE_DIR)
	@echo "[fetch] mongoose $(MONGOOSE_VERSION) ($(MONGOOSE_SHA))"
	curl -fsSL -o $(MONGOOSE_HDR) $(MONGOOSE_URL)/mongoose.h
	curl -fsSL -o $(MONGOOSE_SRC) $(MONGOOSE_URL)/mongoose.c
	@echo "[verify] mongoose sha256"
	@echo "$(MONGOOSE_H_SHA256)  $(MONGOOSE_HDR)" | sha256sum -c -
	@echo "$(MONGOOSE_C_SHA256)  $(MONGOOSE_SRC)" | sha256sum -c -
	@# mongoose TLS is disabled (MG_TLS_NONE); no BIO patch needed.

$(MONGOOSE_DIR):
	mkdir -p $@

# ---------------------------------------------------------------------------
# LibreSSL: download, verify, configure --disable-shared, make install
# ---------------------------------------------------------------------------

$(LIBRESSL_STAMP): | $(LIBRESSL_DIR)
	@echo "[fetch] libressl $(LIBRESSL_VERSION)"
	cd $(LIBRESSL_DIR) && \
		curl -fsSL -O $(LIBRESSL_URL)
	@if [ -z "$(LIBRESSL_SHA256)" ]; then \
		echo "error: LIBRESSL_SHA256 is not set — refusing to build with an unverified tarball" >&2; \
		exit 1; \
	fi
	@echo "[verify] libressl sha256"
	@echo "$(LIBRESSL_SHA256)  $(LIBRESSL_DIR)/$(LIBRESSL_TARBALL)" | sha256sum -c -
	cd $(LIBRESSL_DIR) && tar xf $(LIBRESSL_TARBALL)
	cd $(LIBRESSL_SRCDIR) && \
		./configure \
			--prefix=$(LIBRESSL_PREFIX) \
			--disable-shared \
			--enable-static \
			--disable-tests \
			CFLAGS="-fPIC -O2"
	$(MAKE) -C $(LIBRESSL_SRCDIR) -j$$(nproc)
	$(MAKE) -C $(LIBRESSL_SRCDIR) install
	touch $@

$(LIBRESSL_DIR):
	mkdir -p $@

# ---------------------------------------------------------------------------
# Helper targets
# ---------------------------------------------------------------------------

.PHONY: bootstrap
bootstrap: $(LIBRESSL_STAMP) $(MONGOOSE_SRC)

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

.PHONY: distclean
distclean: clean
	rm -rf $(THIRD_PARTY_DIR)/libressl $(THIRD_PARTY_DIR)/mongoose

# ---------------------------------------------------------------------------
# Install / uninstall (used by debian/rules via DESTDIR)
# ---------------------------------------------------------------------------

.PHONY: install
install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/dymo-web-service
	install -d $(DESTDIR)$(SHAREDIR)
	install -m 0644 userscript/cellartracker-dymo-linux.user.js \
		$(DESTDIR)$(SHAREDIR)/cellartracker-dymo-linux.user.js
	install -m 0644 README.md $(DESTDIR)$(SHAREDIR)/README.md
	install -d $(DESTDIR)$(SYSCONFDIR)
	install -m 0644 config/dymo-web-service.conf \
		$(DESTDIR)$(SYSCONFDIR)/dymo-web-service.conf
	install -d $(DESTDIR)$(SYSTEMDDIR)
	install -m 0644 systemd/dymo-web-service.service \
		$(DESTDIR)$(SYSTEMDDIR)/dymo-web-service.service

.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)$(BINDIR)/dymo-web-service
	rm -rf $(DESTDIR)$(SHAREDIR)
	rm -f $(DESTDIR)$(SYSTEMDDIR)/dymo-web-service.service

# ---------------------------------------------------------------------------
# Debian package
# ---------------------------------------------------------------------------

.PHONY: deb
deb:
	dpkg-buildpackage -us -uc -b
