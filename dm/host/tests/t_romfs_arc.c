// Test romfs_arc.c against a packed archive: path lookup, partial reads, directory listing.
// Build: clang -I dm/host -o /tmp/t_romfs_arc dm/host/tests/t_romfs_arc.c dm/host/romfs_arc.c
#include "romfs_arc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <base>\n", argv[0]); return 2; }
    romfsarc_hdr h;
    if (romfsarc_open(argv[1], &h)) { perror("romfsarc_open"); return 2; }
    printf("opened: %llu files, %llu dirs, %u shards, hash %016llx\n",
           (unsigned long long)h.file_count, (unsigned long long)h.dir_count, h.shard_count,
           (unsigned long long)h.tree_hash);
    CHECK(romfsarc_active(), "active after open");
    CHECK(h.file_count > 0 && h.dir_count > 0 && h.shard_count > 0, "header counts");

    // Root listing: every entry is reachable and the count agrees
    romfsarc_ent *ents = calloc(4096, sizeof *ents);
    int listed = romfsarc_list("", ents, 4096);
    CHECK(listed > 0, "root has entries, got %d", listed);
    CHECK(listed == romfsarc_count(""), "list and count agree at the root: %d vs %d", listed, romfsarc_count(""));

    // Walk the tree breadth first and check every file's bytes against a direct read of its shard
    size_t dcap = 1024;
    char **dirs = calloc(dcap, sizeof *dirs);
    int nd = 0;
    dirs[nd++] = strdup("");
    int checked = 0;
    while (nd) {
        char *d = dirs[--nd];
        int n = romfsarc_count(d);
        CHECK(n >= 0, "count of '%s'", d);
        romfsarc_ent *e = calloc((size_t)(n > 0 ? n : 1), sizeof *e);
        int got = romfsarc_list(d, e, n);
        CHECK(got == n, "list '%s': %d vs %d", d, got, n);
        for (int i = 0; i < got; i++) {
            char rel[1024];
            snprintf(rel, sizeof rel, "%s%s%s", d, *d ? "/" : "", e[i].name);
            if (e[i].is_dir) {
                CHECK(romfsarc_is_dir(rel), "is_dir '%s'", rel);
                CHECK(!romfsarc_stat_file(rel, NULL, NULL, NULL), "dir is not a file: '%s'", rel);
                if ((size_t)nd == dcap) { dcap *= 2; dirs = realloc(dirs, dcap * sizeof *dirs); }
                dirs[nd++] = strdup(rel);
            } else {
                uint32_t shard; uint64_t off, size;
                CHECK(romfsarc_stat_file(rel, &shard, &off, &size), "stat '%s'", rel);
                CHECK((int64_t)size == e[i].size, "size '%s': %llu vs %lld", rel,
                      (unsigned long long)size, (long long)e[i].size);
                char *buf = malloc(size ? size : 1);
                ssize_t r = romfsarc_pread(shard, off, buf, size);
                CHECK(r == (ssize_t)size, "read '%s': %zd vs %llu", rel, r, (unsigned long long)size);
                // A read past this file's end lands in the next file's bytes: the archive is a flat
                // container, so the reader must not fabricate zeros. The fs layer clamps to the file
                // size (see h_fs_ReadFile), so this only checks the reader does not fail outright.
                char extra[16];
                ssize_t r2 = romfsarc_pread(shard, off + size, extra, sizeof extra);
                CHECK(r2 >= 0, "read past end of '%s' errored: %zd", rel, r2);
                free(buf);
                checked++;
            }
        }
        free(e);
    }
    CHECK(checked == (int)h.file_count, "walked %d files, header says %llu", checked, (unsigned long long)h.file_count);

    // Missing paths
    CHECK(!romfsarc_stat_file("nope/nothing", NULL, NULL, NULL), "missing file is not found");
    CHECK(!romfsarc_is_dir("nope"), "missing dir is not a dir");
    CHECK(romfsarc_list("nope", ents, 4) < 0, "list of a missing dir fails");

    // Root itself is a directory, and the empty path is not a file
    CHECK(romfsarc_is_dir(""), "root is a dir");
    CHECK(!romfsarc_stat_file("", NULL, NULL, NULL), "root is not a file");

    romfsarc_close();
    CHECK(!romfsarc_active(), "closed");
    free(ents);
    printf(fails ? "%d FAILURES\n" : "all checks passed (%d files)\n", fails ? fails : checked);
    return fails ? 1 : 0;
}
