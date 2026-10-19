# Build inside the steamlink-build image (docker/Dockerfile):
#   make docker            -> build/aarch64/steamlink (for the handheld)
#   make docker ARCH=x86_64 -> build/x86_64/steamlink (for local testing)
ARCH ?= aarch64

ifeq ($(ARCH),aarch64)
CC := aarch64-linux-gnu-gcc
STRIP := aarch64-linux-gnu-strip
TRIPLET := aarch64-linux-gnu
else
CC := gcc
STRIP := strip
TRIPLET := x86_64-linux-gnu
endif

FF := /opt/ff-$(ARCH)
LIBDIR := /usr/lib/$(TRIPLET)
OUT := build/$(ARCH)

CFLAGS := -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -D_REENTRANT -pthread \
          -I$(FF)/include -idirafter /usr/include/$(TRIPLET) -idirafter /usr/include
LDFLAGS := -pthread -L$(LIBDIR) -Wl,-rpath-link,$(LIBDIR) -Wl,--as-needed
LDLIBS := $(FF)/lib/libavcodec.a $(FF)/lib/libswresample.a $(FF)/lib/libavutil.a \
          $(LIBDIR)/libssl.a $(LIBDIR)/libcrypto.a -lSDL2 -lm -ldl

SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:src/%.c=$(OUT)/%.o)

all: $(OUT)/steamlink

$(OUT)/%.o: src/%.c $(wildcard src/*.h)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/steamlink: $(OBJS)
	$(CC) $(LDFLAGS) -o $@.debug $(OBJS) $(LDLIBS)
	$(STRIP) -o $@ $@.debug

docker:
	docker run --rm -u $$(id -u):$$(id -g) -v "$(CURDIR)":/work steamlink-build make ARCH=$(ARCH) -j4

# Copy dist/MUOS to the root of the muOS SD card (or upload "dist/MUOS/application/Steam Link").
# CREDENTIALS=path/to/device.json also copies your pairing file; never share that zip.
APPDIR := dist/MUOS/application/Steam Link
package: build/aarch64/steamlink
	rm -rf dist
	mkdir -p "$(APPDIR)"
	cp -r "muos/Steam Link/." "$(APPDIR)/"
	cp build/aarch64/steamlink "$(APPDIR)/steamlink"
	cp /etc/ssl/certs/ca-certificates.crt "$(APPDIR)/cacert.pem"
	chmod +x "$(APPDIR)/steamlink" "$(APPDIR)/mux_launch.sh"
	if [ -n "$(CREDENTIALS)" ]; then install -m 600 "$(CREDENTIALS)" "$(APPDIR)/device.json"; fi
	cd dist && rm -f SteamLink-muOS.zip && python3 -m zipfile -c SteamLink-muOS.zip MUOS
	@echo "package ready: $(CURDIR)/dist"

clean:
	rm -rf build dist

.PHONY: all docker package clean
