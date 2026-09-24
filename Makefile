# Copyright (C) 2026, LibreDarwin
# SPDX-License-Identifier: BSD-3-Clause
# Clean-room reimplementation of mkbom/lsbom.
#
# Build layout: every artifact lives under build/; final tools go to
# build/release/ or build/debug/ per CONFIG.
#
# Portable to both GNU make and BSD make (bmake): no pattern rules, no
# ifeq/ifdef/.if conditionals and no $(if)/$(shell) functions.  Per-config
# flags come from make/<CONFIG>.mk so both make variants behave identically.

CONFIG ?= release
SDK    ?= /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
CC     := /Users/sunneva/xnuports-root/devel/xcode-tools/build/release/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang

-include make/$(CONFIG).mk

BUILD_DIR := build/$(CONFIG)
OBJDIR    := $(BUILD_DIR)/obj

CFLAGS := $(OPT) -std=c11 -D_DARWIN_C_SOURCE -isysroot "$(SDK)" -Isrc -Wall -Wextra

MK       := $(BUILD_DIR)/mkbom
MK_OBJS  := $(OBJDIR)/mkbom_main.o
LS       := $(BUILD_DIR)/lsbom
LS_OBJS  := $(OBJDIR)/lsbom_main.o
LIB_OBJS := $(OBJDIR)/bom_cksum.o $(OBJDIR)/fs_walk.o $(OBJDIR)/bom_writer.o $(OBJDIR)/bom_read.o

PREFIX  ?= /usr/local
DESTDIR ?=

all: $(MK) $(LS)

$(MK): $(MK_OBJS) $(LIB_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(LS): $(LS_OBJS) $(LIB_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(OBJDIR)/mkbom_main.o: src/mkbom/mkbom.c src/libbom/bom_writer.h src/libbom/fs_walk.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/mkbom/mkbom.c

$(OBJDIR)/lsbom_main.o: src/lsbom/lsbom.c src/libbom/bom_read.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/lsbom/lsbom.c

$(OBJDIR)/bom_cksum.o: src/libbom/bom_cksum.c src/libbom/bom_cksum.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/libbom/bom_cksum.c

$(OBJDIR)/fs_walk.o: src/libbom/fs_walk.c src/libbom/fs_walk.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/libbom/fs_walk.c

$(OBJDIR)/bom_writer.o: src/libbom/bom_writer.c src/libbom/bom_writer.h src/libbom/bom_cksum.h src/libbom/fs_walk.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/libbom/bom_writer.c

$(OBJDIR)/bom_read.o: src/libbom/bom_read.c src/libbom/bom_read.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/libbom/bom_read.c

test: all
	python3 tools/prototype/run_tests.py --subject $(MK)
	python3 tools/prototype/run_tests.py --subject-lsbom --lsbom $(LS)

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 0755 $(MK) $(DESTDIR)$(PREFIX)/bin/mkbom
	install -m 0755 $(LS) $(DESTDIR)$(PREFIX)/bin/lsbom
	install -m 0444 man/mkbom.1 $(DESTDIR)$(PREFIX)/share/man/man1/mkbom.1
	install -m 0444 man/lsbom.1 $(DESTDIR)$(PREFIX)/share/man/man1/lsbom.1

clean:
	rm -rf build

.PHONY: all test install clean