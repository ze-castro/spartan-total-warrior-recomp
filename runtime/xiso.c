#include "xiso.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SECTOR 2048u
static const uint64_t PARTITIONS[] = { 0x18300000ull, 0x0ull, 0xFD90000ull, 0x2080000ull };

struct XIso { FILE *f; uint64_t base; uint32_t root_sector, root_size; };

typedef struct { char name[256]; uint32_t sector, size; int dir; } Entry;
typedef struct { Entry *v; int n, cap; } List;

XIso *xiso_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    for (unsigned i = 0; i < sizeof PARTITIONS / sizeof *PARTITIONS; i++) {
        uint8_t vd[32];
        if (fseeko(f, (off_t)(PARTITIONS[i] + 32ull * SECTOR), SEEK_SET) || fread(vd, 1, 28, f) != 28) continue;
        if (memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20)) continue;
        XIso *x = calloc(1, sizeof *x);
        x->f = f; x->base = PARTITIONS[i];
        memcpy(&x->root_sector, vd + 20, 4);
        memcpy(&x->root_size, vd + 24, 4);
        return x;
    }
    fclose(f);
    return NULL;
}
void xiso_close(XIso *x) { if (x) { fclose(x->f); free(x); } }

static uint8_t *read_at(XIso *x, uint32_t sector, uint32_t size) {
    uint8_t *b = malloc(size ? size : 1);
    if (!b) return NULL;
    if (fseeko(x->f, (off_t)(x->base + (uint64_t)sector * SECTOR), SEEK_SET) || fread(b, 1, size, x->f) != size) { free(b); return NULL; }
    return b;
}
static void list_add(List *l, const Entry *e) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 256; l->v = realloc(l->v, l->cap * sizeof *l->v); }
    l->v[l->n++] = *e;
}
/* The directory is a binary tree stored in 4-byte units: left, right, sector, size, attributes, name length, name. */
static void visit(XIso *x, const uint8_t *data, uint32_t len, uint32_t off, const char *rel, List *out, int depth) {
    if (depth > 64 || (uint64_t)off * 4 + 14 > len) return;
    const uint8_t *p = data + (size_t)off * 4;
    uint16_t left, right; uint32_t sector, size; uint8_t attr, nlen;
    memcpy(&left, p, 2); memcpy(&right, p + 2, 2); memcpy(&sector, p + 4, 4); memcpy(&size, p + 8, 4); attr = p[12]; nlen = p[13];
    if (left == 0xFFFF && right == 0xFFFF && sector == 0xFFFFFFFFu) return;
    if ((uint64_t)off * 4 + 14 + nlen > len) return;
    if (left) visit(x, data, len, left, rel, out, depth + 1);
    Entry e = { {0}, sector, size, (attr & 0x10) != 0 };
    snprintf(e.name, sizeof e.name, "%s%.*s", rel, nlen, (const char *)p + 14);
    list_add(out, &e);
    if (e.dir && size) {
        uint8_t *sub = read_at(x, sector, size);
        if (sub && !(size >= 2 && sub[0] == 0xFF && sub[1] == 0xFF)) {
            char r[260]; snprintf(r, sizeof r, "%s/", e.name);
            visit(x, sub, size, 0, r, out, depth + 1);
        }
        free(sub);
    }
    if (right) visit(x, data, len, right, rel, out, depth + 1);
}
static int list_all(XIso *x, List *out) {
    uint8_t *root = read_at(x, x->root_sector, x->root_size);
    if (!root) return -1;
    if (!(x->root_size >= 2 && root[0] == 0xFF && root[1] == 0xFF)) visit(x, root, x->root_size, 0, "", out, 0);
    free(root);
    return 0;
}

uint8_t *xiso_read_file(XIso *x, const char *path, uint32_t *size) {
    List l = { 0 };
    uint8_t *r = NULL;
    if (list_all(x, &l) == 0)
        for (int i = 0; i < l.n; i++)
            if (!l.v[i].dir && !strcasecmp(l.v[i].name, path)) { r = read_at(x, l.v[i].sector, l.v[i].size); if (size) *size = l.v[i].size; break; }
    free(l.v);
    return r;
}
uint64_t xiso_total_bytes(XIso *x) {
    List l = { 0 };
    uint64_t t = 0;
    if (list_all(x, &l) == 0) for (int i = 0; i < l.n; i++) if (!l.v[i].dir) t += l.v[i].size;
    free(l.v);
    return t;
}

static void mkdirs(const char *path) {
    char p[1100];
    snprintf(p, sizeof p, "%s", path);
    for (char *q = p + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(p, 0755); *q = '/'; }
    mkdir(p, 0755);
}
int xiso_extract(XIso *x, const char *outdir, xiso_progress cb, void *ctx, char *errmsg, int errlen) {
    List l = { 0 };
    if (list_all(x, &l) != 0) { snprintf(errmsg, errlen, "cannot read the disc's file table"); return -1; }
    uint64_t total = 0, done = 0;
    for (int i = 0; i < l.n; i++) if (!l.v[i].dir) total += l.v[i].size;
    enum { CHUNK = 1 << 20 };
    uint8_t *buf = malloc(CHUNK);
    int rc = 0;
    mkdirs(outdir);
    for (int i = 0; i < l.n && rc == 0; i++) {
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", outdir, l.v[i].name);
        if (l.v[i].dir) { mkdirs(path); continue; }
        char *slash = strrchr(path, '/');
        if (slash) { *slash = 0; mkdirs(path); *slash = '/'; }
        FILE *o = fopen(path, "wb");
        if (!o) { snprintf(errmsg, errlen, "cannot create %s: %s", path, strerror(errno)); rc = -1; break; }
        if (fseeko(x->f, (off_t)(x->base + (uint64_t)l.v[i].sector * SECTOR), SEEK_SET)) { snprintf(errmsg, errlen, "seek failed in the disc image"); rc = -1; }
        for (uint32_t left = l.v[i].size; left && rc == 0;) {
            uint32_t n = left < CHUNK ? left : CHUNK;
            if (fread(buf, 1, n, x->f) != n) { snprintf(errmsg, errlen, "the disc image is truncated (%s)", l.v[i].name); rc = -1; break; }
            if (fwrite(buf, 1, n, o) != n) { snprintf(errmsg, errlen, "cannot write %s (disk full?)", path); rc = -1; break; }
            left -= n; done += n;
            if (cb && cb(done, total, l.v[i].name, ctx)) { snprintf(errmsg, errlen, "cancelled"); rc = -1; }
        }
        fclose(o);
    }
    free(buf);
    free(l.v);
    return rc;
}
