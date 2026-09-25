# Copyright (C) 2026, LibreDarwin
# SPDX-License-Identifier: BSD-3-Clause
# Flags for an AddressSanitizer build.
OPT := -O1 -g -fsanitize=address -fno-omit-frame-pointer
CC  := /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang