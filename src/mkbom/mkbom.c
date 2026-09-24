/* Copyright (C) 2026, LibreDarwin
 * SPDX-License-Identifier: BSD-3-Clause
 * mkbom: build a bill of materials for a directory tree.
 * Clean-room dir-mode reimplementation (block-identical to Apple mkbom;
 * simplified mode -s and -i file-list mode are not implemented yet). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libbom/bom_writer.h"
#include "libbom/fs_walk.h"

static void usage(void) {
    fprintf(stderr, "usage: mkbom [-s] directory bomFile\n");
}

int main(int argc, char **argv) {
    int ch;
    int simplified = 0;
    while ((ch = getopt(argc, argv, "s")) != -1) {
        switch (ch) {
        case 's':
            simplified = 1;
            break;
        default:
            usage();
            return 1;
        }
    }
    argc -= optind;
    argv += optind;
    if (simplified) {
        fprintf(stderr, "mkbom: simplified mode (-s) not implemented\n");
        return 1;
    }
    if (argc != 2) {
        usage();
        return 1;
    }

    bm_walk walk;
    if (bm_scan(argv[0], &walk) != 0) {
        fprintf(stderr, "mkbom: %s: %s\n", argv[0], strerror(errno));
        return 1;
    }

    char errbuf[256];
    int rc = bm_write_bom(&walk, argv[1], errbuf, sizeof(errbuf));
    bm_walk_free(&walk);
    if (rc != 0) {
        fprintf(stderr, "mkbom: %s: %s\n", argv[1],
                errbuf[0] ? errbuf : strerror(errno));
        return 1;
    }
    return 0;
}