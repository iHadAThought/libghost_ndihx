# libghost_ndihx — standalone NDI|HX receive library (+ media_core + discover)
PREFIX      ?= /usr/local
CC          ?= gcc
AR          ?= ar
PKG_CONFIG  ?= pkg-config
CFLAGS      ?= -O2 -Wall -Wextra -Wno-unused-parameter
CFLAGS      += -Iinclude -I$(PREFIX)/include
LDFLAGS     ?= -L$(PREFIX)/lib -Wl,-rpath,$(PREFIX)/lib
NDI_LIBS    := -lndi -ldl -lpthread -lm
UNAME_M     := $(shell uname -m)
UNAME_S     := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  MDNS_CFLAGS :=
  MDNS_LIBS :=
else
  MDNS_CFLAGS := $(shell $(PKG_CONFIG) --cflags avahi-client 2>/dev/null)
  MDNS_LIBS := $(shell $(PKG_CONFIG) --libs avahi-client 2>/dev/null)
  ifeq ($(MDNS_LIBS),)
    MDNS_LIBS := -lavahi-client -lavahi-common
  endif
endif

.PHONY: all clean install install-lib info

all: info libmedia_core.a libghost_discover.a libghost_ndihx.a

info:
	@echo "Building libghost_ndihx for arch=$(UNAME_M) PREFIX=$(PREFIX)"

libmedia_core.a: src/core/media_core.c include/media_core.h
	$(CC) $(CFLAGS) -c -o media_core.o src/core/media_core.c
	$(AR) rcs $@ media_core.o
	rm -f media_core.o

libghost_discover.a: src/modules/discover/ghost_discover.c \
		src/modules/discover/ghost_discover_bonjour.c \
		src/modules/discover/ghost_discover_ndi_sdk.c \
		include/ghost_discover.h
	$(CC) $(CFLAGS) $(MDNS_CFLAGS) -c -o ghost_discover.o src/modules/discover/ghost_discover.c
	$(CC) $(CFLAGS) $(MDNS_CFLAGS) -c -o ghost_discover_bonjour.o src/modules/discover/ghost_discover_bonjour.c
	$(CC) $(CFLAGS) $(MDNS_CFLAGS) -c -o ghost_discover_ndi_sdk.o src/modules/discover/ghost_discover_ndi_sdk.c
	$(AR) rcs $@ ghost_discover.o ghost_discover_bonjour.o ghost_discover_ndi_sdk.o
	rm -f ghost_discover.o ghost_discover_bonjour.o ghost_discover_ndi_sdk.o

libghost_ndihx.a: src/modules/ghost_ndihx/ghost_ndihx.c include/ghost_ndihx.h include/ghost_discover.h include/media_core.h \
		libmedia_core.a libghost_discover.a
	$(CC) $(CFLAGS) -c -o ghost_ndihx.o src/modules/ghost_ndihx/ghost_ndihx.c
	$(AR) rcs $@ ghost_ndihx.o
	rm -f ghost_ndihx.o

install: install-lib

install-lib: libmedia_core.a libghost_discover.a libghost_ndihx.a
	install -d $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib
	install -m 644 include/media_core.h $(DESTDIR)$(PREFIX)/include/media_core.h
	install -m 644 include/ghost_discover.h $(DESTDIR)$(PREFIX)/include/ghost_discover.h
	install -m 644 include/ghost_ndihx.h $(DESTDIR)$(PREFIX)/include/ghost_ndihx.h
	install -m 644 libmedia_core.a $(DESTDIR)$(PREFIX)/lib/libmedia_core.a
	install -m 644 libghost_discover.a $(DESTDIR)$(PREFIX)/lib/libghost_discover.a
	install -m 644 libghost_ndihx.a $(DESTDIR)$(PREFIX)/lib/libghost_ndihx.a

clean:
	rm -f libghost_ndihx.a libghost_discover.a libmedia_core.a *.o
