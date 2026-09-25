# libghost_ndihx — standalone NDI|HX receive library (+ media_core)
PREFIX      ?= /usr/local
CC          ?= gcc
AR          ?= ar
CFLAGS      ?= -O2 -Wall -Wextra -Wno-unused-parameter
CFLAGS      += -Iinclude -I$(PREFIX)/include
LDFLAGS     ?= -L$(PREFIX)/lib -Wl,-rpath,$(PREFIX)/lib
NDI_LIBS    := -lndi -ldl -lpthread -lm
UNAME_M     := $(shell uname -m)

.PHONY: all clean install install-lib info

all: info libmedia_core.a libghost_ndihx.a

info:
	@echo "Building libghost_ndihx for arch=$(UNAME_M) PREFIX=$(PREFIX)"

libmedia_core.a: src/core/media_core.c include/media_core.h
	$(CC) $(CFLAGS) -c -o media_core.o src/core/media_core.c
	$(AR) rcs $@ media_core.o
	rm -f media_core.o

libghost_ndihx.a: src/modules/ghost_ndihx/ghost_ndihx.c include/ghost_ndihx.h include/media_core.h libmedia_core.a
	$(CC) $(CFLAGS) -c -o ghost_ndihx.o src/modules/ghost_ndihx/ghost_ndihx.c
	$(AR) rcs $@ ghost_ndihx.o
	rm -f ghost_ndihx.o

install: install-lib

install-lib: libmedia_core.a libghost_ndihx.a
	install -d $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib
	install -m 644 include/media_core.h $(DESTDIR)$(PREFIX)/include/media_core.h
	install -m 644 include/ghost_ndihx.h $(DESTDIR)$(PREFIX)/include/ghost_ndihx.h
	install -m 644 libmedia_core.a $(DESTDIR)$(PREFIX)/lib/libmedia_core.a
	install -m 644 libghost_ndihx.a $(DESTDIR)$(PREFIX)/lib/libghost_ndihx.a

clean:
	rm -f libghost_ndihx.a libmedia_core.a *.o
