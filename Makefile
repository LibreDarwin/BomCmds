# Copyright (C) 2026, LibreDarwin
# SPDX-License-Identifier: BSD-3-Clause
# Clean-room reimplementation of mkbom/lsbom/ditto.
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
DI       := $(BUILD_DIR)/ditto
DI_OBJS  := $(OBJDIR)/ditto_main.o $(OBJDIR)/adouble.o $(OBJDIR)/bomf.o $(OBJDIR)/cpio.o $(OBJDIR)/macho.o $(OBJDIR)/zip.o
DI_CFLAGS := $(CFLAGS) -Isrc/ditto -Isrc/libbom
BOM_FW   := $(BUILD_DIR)/Bom.framework
BOM_FW_CFG := src/libbom/Info.plist src/libbom/version.plist src/libbom/CodeResources

PREFIX  ?= /usr/local
DESTDIR ?=

all: $(MK) $(LS) $(DI) $(BOM_FW)

$(MK): $(MK_OBJS) $(LIB_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(LS): $(LS_OBJS) $(LIB_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(DI): $(DI_OBJS) $(OBJDIR)/bom_read.o
	@mkdir -p $(BUILD_DIR)
	$(CC) $(DI_CFLAGS) -o $@ $^ -lz -lbz2

# Bom.framework: byte-identical replica of Apple's
# /System/Library/PrivateFrameworks/Bom.framework container (plists,
# CodeResources, symlinks).  Apple ships no binary (it lives in the dyld
# shared cache); ours is linked from the libbom objects in its place, and
# _CodeSignature/CodeResources is the verbatim Apple file (rule-only plist,
# no hashes).  Re-sign with `codesign -f -s - Bom.framework` when importing
# the built framework elsewhere; that rewrites CodeResources and adds a seal.
$(BOM_FW): $(LIB_OBJS) $(BOM_FW_CFG)
	@rm -rf $@
	@mkdir -p $@/Versions/A/_CodeSignature $@/Versions/A/Resources
	$(CC) $(CFLAGS) -dynamiclib \
		-Wl,-install_name,$(PREFIX)/Library/Frameworks/Bom.framework/Versions/A/Bom \
		-Wl,-compatibility_version,1.0.0 -Wl,-current_version,1.0.0 \
		-o $@/Versions/A/Bom $(LIB_OBJS)
	cp src/libbom/Info.plist $@/Versions/A/Resources/
	cp src/libbom/version.plist $@/Versions/A/Resources/
	cp src/libbom/CodeResources $@/Versions/A/_CodeSignature/
	ln -sfn A $@/Versions/Current
	ln -sfn Versions/Current/Bom $@/Bom
	ln -sfn Versions/Current/Resources $@/Resources

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

$(OBJDIR)/ditto_main.o: src/ditto/ditto.c src/ditto/ditto.h src/ditto/usage.inc
	@mkdir -p $(OBJDIR)
	$(CC) $(DI_CFLAGS) -c -o $@ src/ditto/ditto.c

$(OBJDIR)/adouble.o: src/ditto/adouble.c src/ditto/ditto.h
	@mkdir -p $(OBJDIR)
	$(CC) $(DI_CFLAGS) -c -o $@ src/ditto/adouble.c

$(OBJDIR)/bomf.o: src/ditto/bomf.c src/ditto/ditto.h src/libbom/bom_read.h
	@mkdir -p $(OBJDIR)
	$(CC) $(DI_CFLAGS) -c -o $@ src/ditto/bomf.c

$(OBJDIR)/cpio.o: src/ditto/cpio.c src/ditto/ditto.h
	@mkdir -p $(OBJDIR)
	$(CC) $(DI_CFLAGS) -c -o $@ src/ditto/cpio.c

$(OBJDIR)/macho.o: src/ditto/macho.c src/ditto/ditto.h
	@mkdir -p $(OBJDIR)
	$(CC) $(DI_CFLAGS) -c -o $@ src/ditto/macho.c

$(OBJDIR)/zip.o: src/ditto/zip.c src/ditto/ditto.h
	@mkdir -p $(OBJDIR)
	$(CC) $(DI_CFLAGS) -c -o $@ src/ditto/zip.c

test: all
	python3 tools/prototype/run_tests.py --subject $(MK)
	python3 tools/prototype/run_tests.py --subject-lsbom --lsbom $(LS)
	python3 tools/prototype/run_ditto_tests.py --subject $(DI)

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 0755 $(MK) $(DESTDIR)$(PREFIX)/bin/mkbom
	install -m 0755 $(LS) $(DESTDIR)$(PREFIX)/bin/lsbom
	install -m 0755 $(DI) $(DESTDIR)$(PREFIX)/bin/ditto
	install -m 0444 man/mkbom.1 $(DESTDIR)$(PREFIX)/share/man/man1/mkbom.1
	install -m 0444 man/lsbom.1 $(DESTDIR)$(PREFIX)/share/man/man1/lsbom.1
	install -m 0444 man/ditto.1 $(DESTDIR)$(PREFIX)/share/man/man1/ditto.1
	install -d $(DESTDIR)$(PREFIX)/Library/Frameworks
	cp -R $(BOM_FW) $(DESTDIR)$(PREFIX)/Library/Frameworks/

clean:
	rm -rf build

.PHONY: all test install clean