// RomFS archive reader: see romfs_arc.h for the on-disk format.
#include "romfs_arc.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

typedef struct {
    uint32_t name_off, shard;
    uint64_t off, size;
    uint32_t mode, res;
} filerec;

typedef struct {
    uint32_t name_off, first_child, child_count, res;
} dirrec;

static struct {
    int open;
    void *idx;            // mmap of the index file
    size_t idx_size;
    romfsarc_hdr hdr;
    const char *pool;
    const filerec *files;
    const dirrec *dirs;
    const uint32_t *children;
    char base[1024];
    int shard_fd[256];
    char shard_path[256][1024];
    pthread_mutex_t lock;
} A = { .shard_fd = { [0 ... 255] = -1 }, .lock = PTHREAD_MUTEX_INITIALIZER };

static uint64_t rd64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }

char g_arc_base[1024];   // the archive's base path, matched against mount directories by hle_fs.c

static const char *fname(const filerec *f) { return A.pool + f->name_off; }
static const char *dname(const dirrec *d) { return A.pool + d->name_off; }

// Binary search the file table by path. Returns the record or NULL.
static const filerec *find_file(const char *rel) {
    uint64_t lo = 0, hi = A.hdr.file_count;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        int c = strcmp(fname(&A.files[mid]), rel);
        if (c == 0) return &A.files[mid];
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

// Binary search the directory table by path. Returns the record or NULL.
static const dirrec *find_dir(const char *rel) {
    uint64_t lo = 0, hi = A.hdr.dir_count;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        int c = strcmp(dname(&A.dirs[mid]), rel);
        if (c == 0) return &A.dirs[mid];
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

// Open and validate one index file. Returns the mapping (and its size) or MAP_FAILED.
static void *map_index(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return MAP_FAILED;
    struct stat st;
    if (fstat(fd, &st)) { close(fd); errno = EIO; return MAP_FAILED; }
    if ((size_t)st.st_size < ROMFSARC_HDR) { close(fd); errno = EINVAL; return MAP_FAILED; }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return MAP_FAILED;
    const uint8_t *b = m;
    if (memcmp(b, ROMFSARC_MAGIC, 8) || rd32(b + 8) != ROMFSARC_VERSION) {
        munmap(m, (size_t)st.st_size); errno = EINVAL; return MAP_FAILED;
    }
    *size = (size_t)st.st_size;
    return m;
}

int romfsarc_open(const char *base, romfsarc_hdr *out) {
    // The base is where the shards live, without the ".idx". Callers pass either the base itself
    // (DM_ROMFS=/games/totk/romfs with romfs.idx beside it) or a directory that holds the archive
    // (DM_ROMFS=Documents/romfs with Documents/romfs/romfs.idx inside, which is what the iOS app
    // sets). Try both, and a base that already carries the .idx suffix.
    char cand[4][1200];
    char stripped[1200];
    strlcpy(stripped, base, sizeof stripped);
    size_t sl = strlen(stripped);
    if (sl > 4 && !strcmp(stripped + sl - 4, ".idx")) stripped[sl - 4] = 0;
    snprintf(cand[0], sizeof cand[0], "%s.idx", stripped);          // <base>.idx
    snprintf(cand[1], sizeof cand[1], "%s/romfs.idx", stripped);    // <base>/romfs.idx
    snprintf(cand[2], sizeof cand[2], "%s/romfs/romfs.idx", stripped);  // one more level (a bundle-style layout)
    cand[3][0] = 0;
    void *m = MAP_FAILED; size_t sz = 0; char used[1200] = "";
    for (int i = 0; i < 3; i++) {
        m = map_index(cand[i], &sz);
        if (m != MAP_FAILED) { strlcpy(used, cand[i], sizeof used); break; }
    }
    if (m == MAP_FAILED) return -1;
    // The resolved base is the index path minus ".idx": shard paths are built from it
    size_t ul = strlen(used); if (ul > 4) used[ul - 4] = 0;
    const uint8_t *b = m;
    romfsarc_hdr h;
    h.shard_count = rd32(b + 12);
    h.file_count = rd64(b + 24);
    h.dir_count = rd64(b + 32);
    h.tree_hash = rd64(b + 56);
    if (h.shard_count == 0 || h.shard_count > 256) { munmap(m, sz); errno = EINVAL; return -1; }
    uint64_t pool_sz = rd64(b + 40);
    uint64_t need = ROMFSARC_HDR + pool_sz + h.file_count * ROMFSARC_FILEREC + h.dir_count * ROMFSARC_DIRREC;
    // The child list follows the directory table; its length is implied by the last directory's range,
    // so the file only has to be at least as long as the three tables above.
    if (need > (uint64_t)sz) { munmap(m, sz); errno = EINVAL; return -1; }
    pthread_mutex_lock(&A.lock);
    A.open = 1; A.idx = m; A.idx_size = sz; A.hdr = h;
    A.pool = (const char *)(b + ROMFSARC_HDR);
    A.files = (const filerec *)(b + ROMFSARC_HDR + pool_sz);
    A.dirs = (const dirrec *)((const uint8_t *)A.files + h.file_count * ROMFSARC_FILEREC);
    A.children = (const uint32_t *)((const uint8_t *)A.dirs + h.dir_count * ROMFSARC_DIRREC);
    for (int i = 0; i < 256; i++) { A.shard_fd[i] = -1; A.shard_path[i][0] = 0; }
    // Shards are named after the index file's own base, so <dir>/romfs.idx pairs with <dir>/romfs.00
    strlcpy(A.base, used, sizeof A.base);
    strlcpy(g_arc_base, used, sizeof g_arc_base);
    pthread_mutex_unlock(&A.lock);
    if (out) *out = h;
    return 0;
}

int romfsarc_active(void) { return A.open; }

int romfsarc_stat_file(const char *rel, uint32_t *shard, uint64_t *off, uint64_t *size) {
    if (!A.open) return 0;
    const filerec *f = find_file(rel);
    if (!f) return 0;
    if (shard) *shard = f->shard;
    if (off) *off = f->off;
    if (size) *size = f->size;
    return 1;
}

int romfsarc_is_dir(const char *rel) {
    if (!A.open) return 0;
    return find_dir(rel) != NULL;
}

static int shard_fd(uint32_t i) {
    if (i >= A.hdr.shard_count) { errno = EINVAL; return -1; }
    if (A.shard_fd[i] >= 0) return A.shard_fd[i];
    pthread_mutex_lock(&A.lock);
    if (A.shard_fd[i] < 0) {
        char p[1024];
        if (snprintf(p, sizeof p, "%s.%02u", A.base, i) < (int)sizeof p) {
            int fd = open(p, O_RDONLY);
            if (fd >= 0) { A.shard_fd[i] = fd; strlcpy(A.shard_path[i], p, sizeof A.shard_path[i]); }
        }
    }
    int fd = A.shard_fd[i];
    pthread_mutex_unlock(&A.lock);
    return fd;
}

ssize_t romfsarc_pread(uint32_t shard, uint64_t off, void *buf, size_t n) {
    int fd = shard_fd(shard);
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < n) {
        ssize_t r = pread(fd, (uint8_t *)buf + done, n - done, (off_t)(off + done));
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) break;
        done += (size_t)r;
    }
    return (ssize_t)done;
}

int romfsarc_list(const char *rel, romfsarc_ent *out, int max) {
    if (!A.open) return -1;
    const dirrec *d = find_dir(rel);
    if (!d) return -1;
    int n = 0;
    for (uint32_t i = 0; i < d->child_count && n < max; i++) {
        uint32_t tag = A.children[d->first_child + i];
        romfsarc_ent *e = &out[n];
        if (tag & ROMFSARC_CHILD_DIR) {
            const dirrec *c = &A.dirs[tag & ~ROMFSARC_CHILD_DIR];
            // The directory table stores full paths; a child list entry carries only the leaf name
            const char *full = dname(c), *leaf = strrchr(full, '/');
            strlcpy(e->name, leaf ? leaf + 1 : full, sizeof e->name);
            e->is_dir = 1; e->size = 0;
        } else {
            const filerec *c = &A.files[tag];
            // The file table stores full paths; a child list entry carries only the leaf name
            const char *full = fname(c), *leaf = strrchr(full, '/');
            strlcpy(e->name, leaf ? leaf + 1 : full, sizeof e->name);
            e->is_dir = 0; e->size = (int64_t)c->size;
        }
        n++;
    }
    return n;
}

int romfsarc_count(const char *rel) {
    if (!A.open) return -1;
    const dirrec *d = find_dir(rel);
    return d ? (int)d->child_count : -1;
}

void romfsarc_close(void) {
    pthread_mutex_lock(&A.lock);
    for (int i = 0; i < 256; i++) if (A.shard_fd[i] >= 0) { close(A.shard_fd[i]); A.shard_fd[i] = -1; }
    if (A.idx) { munmap(A.idx, A.idx_size); A.idx = NULL; }
    A.open = 0;
    pthread_mutex_unlock(&A.lock);
}
