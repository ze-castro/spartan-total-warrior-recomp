/* File system HLE: Xbox NT paths -> host directories.
 *   \Device\CdRom0                  -> <game root>  (the extracted disc; see paths.c)
 *   \Device\Harddisk0\PartitionN    -> <data dir>/save/PN/  (console hard disk)
 * Drive letters are symbolic links (\??\D: etc.) kept in a small table the game can extend. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "kernel.h"
#include "paths.h"

#define STATUS_SUCCESS 0u
#define STATUS_NO_MORE_FILES 0x80000006u
#define STATUS_INFO_LENGTH_MISMATCH 0xC0000004u
#define STATUS_INVALID_HANDLE 0xC0000008u
#define STATUS_END_OF_FILE 0xC0000011u
#define STATUS_NO_SUCH_FILE 0xC000000Fu
#define STATUS_ACCESS_DENIED 0xC0000022u
#define STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define STATUS_OBJECT_NAME_COLLISION 0xC0000035u
#define STATUS_OBJECT_PATH_NOT_FOUND 0xC000003Au
#define STATUS_BUFFER_OVERFLOW 0x80000005u
#define STATUS_NOT_IMPLEMENTED 0xC0000002u
enum { H_FILE = 2 };
#define HDD_PREFIX "\\Device\\Harddisk0\\Partition"

typedef struct { int fd; int isdir; char path[1024]; void *dir; int dir_started; char host[1200]; int del; } FileObj;   /* del: delete the host file when the handle closes */
static int ftrace(void) { static int v = -1; if (v < 0) v = getenv("STW_TRACE") != NULL || getenv("STW_FILETRACE") != NULL; return v; }   /* STW_FILETRACE=1: file operations only */
#define MAXF 1024
static FileObj files[MAXF];

/* ---- symbolic links ---- */
#define MAXL 64
static struct { char name[64]; char target[256]; } links[MAXL];
static int nlinks;
static int links_ready;

static void add_link(const char *name, const char *target) {
    for (int i = 0; i < nlinks; i++)
        if (!strcasecmp(links[i].name, name)) { snprintf(links[i].target, sizeof links[i].target, "%s", target); return; }
    if (nlinks < MAXL) {
        snprintf(links[nlinks].name, sizeof links[nlinks].name, "%s", name);
        snprintf(links[nlinks].target, sizeof links[nlinks].target, "%s", target);
        nlinks++;
    }
}
static void init_links(void) {
    if (links_ready) return;
    links_ready = 1;
    add_link("\\??\\D:", "\\Device\\CdRom0");
    add_link("\\??\\C:", "\\Device\\Harddisk0\\Partition2");
    add_link("\\??\\E:", "\\Device\\Harddisk0\\Partition1");
    add_link("\\??\\X:", "\\Device\\Harddisk0\\Partition3");
    add_link("\\??\\Y:", "\\Device\\Harddisk0\\Partition4");
    add_link("\\??\\Z:", "\\Device\\Harddisk0\\Partition5");
}

static void gcopy(char *dst, size_t n, uint32_t ansi) {          /* ANSI_STRING* -> C string */
    uint32_t len = LD16(ansi), buf = LD32(ansi + 4);
    if (len >= n) len = n - 1;
    memcpy(dst, G + buf, len);
    dst[len] = 0;
}

/* Expand drive letters / links until the path starts with \Device\... */
static void expand(char *path, size_t n) {
    init_links();
    char tmp[1100];
    if (path[0] && path[1] == ':') {                             /* "D:\x" -> "\??\D:\x" */
        snprintf(tmp, sizeof tmp, "\\??\\%s", path);
        snprintf(path, n, "%s", tmp);
    }
    for (int iter = 0; iter < 8; iter++) {
        int hit = 0;
        for (int i = 0; i < nlinks; i++) {
            size_t l = strlen(links[i].name);
            if (!strncasecmp(path, links[i].name, l) && (path[l] == 0 || path[l] == '\\')) {
                snprintf(tmp, sizeof tmp, "%s%s", links[i].target, path + l);
                snprintf(path, n, "%s", tmp);
                hit = 1;
                break;
            }
        }
        if (!hit) break;
    }
}

/* Find `name` in directory `dir` ignoring case; writes the real path into out. */
static int lookup_ci(const char *dir, const char *name, char *out, size_t n) {
    snprintf(out, n, "%s/%s", dir, name);
    struct stat st;
    if (!stat(out, &st)) return 1;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int ok = 0;
    while ((e = readdir(d)))
        if (!strcasecmp(e->d_name, name)) { snprintf(out, n, "%s/%s", dir, e->d_name); ok = 1; break; }
    closedir(d);
    return ok;
}

/* NT path -> host path. Returns 0 on success; *exists tells whether the target exists. pathmiss is set when a missing component
 * has more components after it (the kernel then reports PATH_NOT_FOUND = Win32 3 instead of NAME_NOT_FOUND = 2, which the game tells apart). */
static int pathmiss;
static int to_host(const char *nt, char *host, size_t n, int *exists) {
    char p[1100];
    snprintf(p, sizeof p, "%s", nt);
    expand(p, sizeof p);
    const char *rest;
    if (!strncasecmp(p, "\\Device\\CdRom0", 14)) { snprintf(host, n, "%s", stw_game_root()); rest = p + 14; }
    else if (!strncasecmp(p, HDD_PREFIX, strlen(HDD_PREFIX))) {
        int part = atoi(p + strlen(HDD_PREFIX));
        char sdir[1100];
        stw_path(sdir, sizeof sdir, "save");
        snprintf(host, n, "%s/P%d", sdir, part);
        mkdir(sdir, 0755); mkdir(host, 0755);
        rest = p + strlen(HDD_PREFIX);
        while (*rest >= '0' && *rest <= '9') rest++;
    } else return -1;
    *exists = 1;
    pathmiss = 0;
    char comp[256];
    while (*rest) {
        while (*rest == '\\') rest++;
        size_t k = 0;
        while (*rest && *rest != '\\' && k < sizeof comp - 1) comp[k++] = *rest++;
        comp[k] = 0;
        if (!k) break;
        char next[1100];
        if (!*exists) pathmiss = 1;                                  /* a parent is already missing */
        if (*exists && lookup_ci(host, comp, next, sizeof next)) snprintf(host, n, "%s", next);
        else { *exists = 0; snprintf(next, sizeof next, "%s/%s", host, comp); snprintf(host, n, "%s", next); }
    }
    return 0;
}

static int obj_path(uint32_t oa, char *nt, size_t n) {           /* OBJECT_ATTRIBUTES -> NT path */
    uint32_t root = LD32(oa), name = LD32(oa + 4);
    char rel[1024] = "";
    nt[0] = 0;
    if (name) gcopy(rel, sizeof rel, name);
    if (root == 0xFFFFFFFDu) {                    /* the DOS-devices directory: "D:\\x" means "\\??\\D:\\x" */
        snprintf(nt, n, "\\??\\%s", rel);
    } else if (root) {
        uint32_t i = root / 4;
        extern uint32_t handle_obj(uint32_t);
        uint32_t fi = handle_obj(root);
        (void)i;
        if (!fi || fi >= MAXF || !files[fi].path[0]) return -1;
        snprintf(nt, n, "%s%s%s", files[fi].path, rel[0] == '\\' ? "" : "\\", rel);
    } else snprintf(nt, n, "%s", rel);
    return 0;
}

static int alloc_file(void) {
    for (int i = 1; i < MAXF; i++) if (!files[i].path[0]) return i;
    return -1;
}

static uint64_t ft_from(time_t t) { return ((uint64_t)t + 11644473600ull) * 10000000ull; }
static void put64(uint32_t a, uint64_t v) { ST32(a, (uint32_t)v); ST32(a + 4, (uint32_t)(v >> 32)); }

static uint32_t status_of_errno(int e, int path_missing) {
    switch (e) {
    case ENOENT: return path_missing ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_OBJECT_NAME_NOT_FOUND;
    case ENOTDIR: return STATUS_OBJECT_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EROFS: return STATUS_ACCESS_DENIED;
    case EEXIST: return STATUS_OBJECT_NAME_COLLISION;
    default: return STATUS_ACCESS_DENIED;
    }
}

/* disposition: 0 supersede,1 open,2 create,3 open_if,4 overwrite,5 overwrite_if; opts: bit0 = directory */
static uint32_t open_common(uint32_t ph, uint32_t access, uint32_t oa, uint32_t iosb, int disp, uint32_t opts) {
    char nt[1100], host[1200];
    int exists;
    if (obj_path(oa, nt, sizeof nt) || to_host(nt, host, sizeof host, &exists)) {
        if (ftrace())
            fprintf(stderr, "[file] open \"%s\" (root %#x, name len %u) -> no such device\n", nt, LD32(oa), LD16(LD32(oa + 4)));
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    int write = (access & 0x40000002u) != 0 || disp != 1;            /* GENERIC_WRITE / FILE_WRITE_DATA */
    struct stat st;
    int isdir = exists && !stat(host, &st) && S_ISDIR(st.st_mode);
    uint32_t info = 1;                                              /* FILE_OPENED */
    if (ftrace()) fprintf(stderr, "[file] open %s -> %s (%s)\n", nt, host, exists ? "exists" : "missing");
    if (!exists) {
        if (disp == 1 || disp == 4) return pathmiss ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_OBJECT_NAME_NOT_FOUND;
        if (opts & 1) { if (mkdir(host, 0755)) return status_of_errno(errno, 1); isdir = 1; }
        info = 2;
    } else if (disp == 2) return STATUS_OBJECT_NAME_COLLISION;
    else if (disp == 0 || disp == 4 || disp == 5) info = disp == 0 ? 0 : 3;
    int fi = alloc_file();
    if (fi < 0) return 0xC000009Au;
    FileObj *f = &files[fi];
    memset(f, 0, sizeof *f);
    f->fd = -1;
    f->isdir = isdir;
    if (!isdir) {
        int flags = write ? O_RDWR | O_CREAT : O_RDONLY;
        if (disp == 0 || disp == 4 || disp == 5) flags |= O_TRUNC;
        f->fd = open(host, flags, 0644);
        if (f->fd < 0 && write) f->fd = open(host, O_RDONLY);
        if (f->fd < 0) return status_of_errno(errno, pathmiss);
    }
    snprintf(f->path, sizeof f->path, "%s", nt);
    snprintf(f->host, sizeof f->host, "%s", host);
    f->del = (opts & 0x1000) != 0;                                   /* FILE_DELETE_ON_CLOSE */
    expand(f->path, sizeof f->path);
    ST32(ph, handle_new(H_FILE, (uint32_t)fi));
    if (iosb) { ST32(iosb, 0); ST32(iosb + 4, info); }
    return STATUS_SUCCESS;
}

uint32_t k_NtOpenFile(CPU *c, const uint32_t *a) {              /* Handle*, Access, ObjAttr*, IoStatus*, Share, Options */
    (void)c; return open_common(a[0], a[1], a[2], a[3], 1, a[5]);
}
uint32_t k_NtCreateFile(CPU *c, const uint32_t *a) {            /* Handle*, Access, ObjAttr*, IoStatus*, AllocSize*, Attr, Share, Disp, Options */
    (void)c; return open_common(a[0], a[1], a[2], a[3], (int)a[7], a[8]);
}

static FileObj *fobj(uint32_t h) {
    uint32_t i = handle_obj(h);
    return (i && i < MAXF && files[i].path[0]) ? &files[i] : NULL;
}
static uint32_t io_done(uint32_t iosb, uint32_t status, uint32_t info) {
    if (iosb) { ST32(iosb, status); ST32(iosb + 4, info); }
    return status;
}

/* Asynchronous completion: the read/write already happened; signal the event and queue the completion APC for
 * this thread (delivered when it next waits alertably), as the kernel would when the I/O finishes. */
static void io_complete(CPU *c, uint32_t event, uint32_t apc, uint32_t apc_ctx, uint32_t iosb) {
    if (event) kobj_signal_handle(event);
    if (apc) kobj_queue_apc(kobj_cur_thread(c), apc, apc_ctx, iosb, 0);
}

uint32_t k_NtReadFile(CPU *c, const uint32_t *a) {   /* Handle, Event, Apc, ApcCtx, IoStatus*, Buffer, Length, ByteOffset* */
    FileObj *f = fobj(a[0]);
    if (!f || f->fd < 0) return STATUS_INVALID_HANDLE;
    ssize_t n;
    if (a[7]) n = pread(f->fd, G + a[5], a[6], (off_t)(LD32(a[7]) | ((uint64_t)LD32(a[7] + 4) << 32)));
    else n = read(f->fd, G + a[5], a[6]);
    if (n < 0) return io_done(a[4], STATUS_ACCESS_DENIED, 0);
    if (n == 0 && a[6]) { io_complete(c, a[1], a[2], a[3], a[4]); return io_done(a[4], STATUS_END_OF_FILE, 0); }
    io_complete(c, a[1], a[2], a[3], a[4]);
    return io_done(a[4], STATUS_SUCCESS, (uint32_t)n);
}
uint32_t k_NtWriteFile(CPU *c, const uint32_t *a) {
    FileObj *f = fobj(a[0]);
    if (!f || f->fd < 0) return STATUS_INVALID_HANDLE;
    ssize_t n;
    if (a[7]) n = pwrite(f->fd, G + a[5], a[6], (off_t)(LD32(a[7]) | ((uint64_t)LD32(a[7] + 4) << 32)));
    else n = write(f->fd, G + a[5], a[6]);
    if (n < 0) return io_done(a[4], STATUS_ACCESS_DENIED, 0);
    io_complete(c, a[1], a[2], a[3], a[4]);
    return io_done(a[4], STATUS_SUCCESS, (uint32_t)n);
}
uint32_t k_NtFlushBuffersFile(CPU *c, const uint32_t *a) {
    (void)c;
    FileObj *f = fobj(a[0]);
    if (!f) return STATUS_INVALID_HANDLE;
    if (f->fd >= 0) fsync(f->fd);
    return io_done(a[1], STATUS_SUCCESS, 0);
}

/* Closing a file handle releases the host resources; other handle kinds are handled in kobj.c. */
void kfile_close(uint32_t idx) {
    if (idx >= MAXF) return;
    if (files[idx].fd >= 0) close(files[idx].fd);
    if (files[idx].dir) closedir(files[idx].dir);
    if (files[idx].del && files[idx].host[0]) {                       /* delete requested through the handle */
        int rc = files[idx].isdir ? rmdir(files[idx].host) : unlink(files[idx].host);
        if (ftrace()) fprintf(stderr, "[file] delete %s -> %d\n", files[idx].host, rc);
    }
    memset(&files[idx], 0, sizeof files[idx]);
    files[idx].fd = -1;
}

static void fill_basic(uint32_t p, const struct stat *st) {         /* FILE_NETWORK_OPEN_INFORMATION */
    put64(p, ft_from(st->st_birthtime)); put64(p + 8, ft_from(st->st_atime));
    put64(p + 16, ft_from(st->st_mtime)); put64(p + 24, ft_from(st->st_mtime));
    put64(p + 32, (st->st_size + 4095) & ~4095ll); put64(p + 40, st->st_size);
    ST32(p + 48, S_ISDIR(st->st_mode) ? 0x10 : 0x80);
}

uint32_t k_NtQueryInformationFile(CPU *c, const uint32_t *a) {      /* Handle, IoStatus*, Info*, Length, Class */
    (void)c;
    FileObj *f = fobj(a[0]);
    if (!f) return STATUS_INVALID_HANDLE;
    struct stat st;
    char host[1200]; int ex;
    if (f->fd >= 0 ? fstat(f->fd, &st) : (to_host(f->path, host, sizeof host, &ex) || stat(host, &st)))
        return io_done(a[1], STATUS_OBJECT_NAME_NOT_FOUND, 0);
    uint32_t p = a[2];
    switch (a[4]) {
    case 4: {                                                        /* FileBasicInformation */
        put64(p, ft_from(st.st_birthtime)); put64(p + 8, ft_from(st.st_atime));
        put64(p + 16, ft_from(st.st_mtime)); put64(p + 24, ft_from(st.st_mtime));
        ST32(p + 32, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
        return io_done(a[1], 0, 36);
    }
    case 5:                                                          /* FileStandardInformation */
        put64(p, (st.st_size + 4095) & ~4095ll); put64(p + 8, st.st_size);
        ST32(p + 16, 1); ST8(p + 20, 0); ST8(p + 21, S_ISDIR(st.st_mode));
        return io_done(a[1], 0, 24);
    case 14: {                                                       /* FilePositionInformation */
        off_t pos = f->fd >= 0 ? lseek(f->fd, 0, SEEK_CUR) : 0;
        put64(p, (uint64_t)pos);
        return io_done(a[1], 0, 8);
    }
    case 34:                                                         /* FileNetworkOpenInformation */
        fill_basic(p, &st);
        return io_done(a[1], 0, 56);
    default:
        fprintf(stderr, "stw: NtQueryInformationFile class %u unsupported\n", a[4]);
        return io_done(a[1], STATUS_NOT_IMPLEMENTED, 0);
    }
}
uint32_t k_NtSetInformationFile(CPU *c, const uint32_t *a) {        /* Handle, IoStatus*, Info*, Length, Class */
    (void)c;
    FileObj *f = fobj(a[0]);
    if (!f) return STATUS_INVALID_HANDLE;
    switch (a[4]) {
    case 14:                                                         /* FilePositionInformation */
        if (f->fd >= 0) lseek(f->fd, (off_t)(LD32(a[2]) | ((uint64_t)LD32(a[2] + 4) << 32)), SEEK_SET);
        return io_done(a[1], 0, 0);
    case 20:                                                         /* FileEndOfFileInformation */
        if (f->fd >= 0) ftruncate(f->fd, (off_t)(LD32(a[2]) | ((uint64_t)LD32(a[2] + 4) << 32)));
        return io_done(a[1], 0, 0);
    case 13:                                                         /* FileDispositionInformation: { BOOLEAN DeleteFile } */
        f->del = G[a[2]] != 0;
        if (ftrace()) fprintf(stderr, "[file] disposition delete=%d on %s\n", f->del, f->host);
        return io_done(a[1], 0, 0);
    case 4: return io_done(a[1], 0, 0);                              /* basic info: accepted, ignored */
    default:
        fprintf(stderr, "stw: NtSetInformationFile class %u unsupported\n", a[4]);
        return io_done(a[1], STATUS_NOT_IMPLEMENTED, 0);
    }
}
uint32_t k_NtQueryFullAttributesFile(CPU *c, const uint32_t *a) {   /* ObjAttr*, FILE_NETWORK_OPEN_INFORMATION* */
    (void)c;
    char nt[1100], host[1200]; int ex;
    struct stat st;
    if (obj_path(a[0], nt, sizeof nt) || to_host(nt, host, sizeof host, &ex)) return STATUS_OBJECT_PATH_NOT_FOUND;
    if (ftrace()) fprintf(stderr, "[file] attrs %s -> %s\\n", nt, host);
    if (!ex || stat(host, &st)) return pathmiss ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_OBJECT_NAME_NOT_FOUND;
    fill_basic(a[1], &st);
    return 0;
}
uint32_t k_NtQueryVolumeInformationFile(CPU *c, const uint32_t *a) { /* Handle, IoStatus*, Info*, Length, Class */
    (void)c;
    uint32_t p = a[2];
    FileObj *f = fobj(a[0]);
    int cd = f && !strncasecmp(f->path, "\\Device\\CdRom0", 14);
    if (ftrace()) fprintf(stderr, "[file] volinfo class %u on %s\n", a[4], f ? f->path : "?");
    switch (a[4]) {
    case 3:                                                          /* FileFsSizeInformation */
        if (cd) { put64(p, 0x380000); put64(p + 8, 0); ST32(p + 16, 1); ST32(p + 20, 2048); }       /* DVD: 2 KB sectors */
        else { put64(p, 0x18000); put64(p + 8, 0x10000); ST32(p + 16, 32); ST32(p + 20, 512); }   /* FATX: 16 KB clusters, 1.5 GB with 1 GB free (kept under 2 GB: the game does 32-bit signed size maths) */
        return io_done(a[1], 0, 24);
    case 4:                                                          /* FileFsDeviceInformation */
        ST32(p, 2); ST32(p + 4, 0);                                   /* FILE_DEVICE_CD_ROM_FILE_SYSTEM */
        return io_done(a[1], 0, 8);
    default:
        fprintf(stderr, "stw: NtQueryVolumeInformationFile class %u unsupported\n", a[4]);
        return io_done(a[1], STATUS_NOT_IMPLEMENTED, 0);
    }
}
uint32_t k_NtFsControlFile(CPU *c, const uint32_t *a) {              /* handle, event, apc, ctx, iosb, code, in, inlen, out, outlen */
    (void)c;
    fprintf(stderr, "stw: NtFsControlFile code %#x (ignored)\n", a[5]);
    return io_done(a[4], 0, 0);
}

/* FILE_*_DIRECTORY_INFORMATION classes: 1 directory, 2 full, 3 both, 12 names */
uint32_t k_NtQueryDirectoryFile(CPU *c, const uint32_t *a) {
    (void)c;   /* Handle, Event, Apc, ApcCtx, IoStatus*, Info*, Length, Class, FileName*, RestartScan */
    FileObj *f = fobj(a[0]);
    if (!f || !f->isdir) return STATUS_INVALID_HANDLE;
    char host[1200]; int ex;
    if (to_host(f->path, host, sizeof host, &ex) || !ex) return STATUS_OBJECT_PATH_NOT_FOUND;
    if ((a[9] & 0xFF) && f->dir) { closedir(f->dir); f->dir = NULL; f->dir_started = 0; }
    if (!f->dir) f->dir = opendir(host);
    if (!f->dir) return STATUS_OBJECT_PATH_NOT_FOUND;
    char pat[256] = "";
    if (a[8] && !f->dir_started) gcopy(pat, sizeof pat, a[8]);
    f->dir_started = 1;
    struct dirent *e;
    while ((e = readdir(f->dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;   /* FATX directories have no dot entries; the game recurses into them forever */
        if (pat[0] && strcmp(pat, "*") && strcasecmp(pat, e->d_name)) continue;
        char full[1500];
        snprintf(full, sizeof full, "%s/%s", host, e->d_name);
        struct stat st;
        if (stat(full, &st)) continue;
        uint32_t nl = (uint32_t)strlen(e->d_name), p = a[5], cls = a[7];
        uint32_t hdr = cls == 1 ? 64 : cls == 2 ? 68 : cls == 3 ? 94 : 12;
        if (hdr + nl > a[6]) return io_done(a[4], STATUS_INFO_LENGTH_MISMATCH, 0);
        memset(G + p, 0, hdr + nl);
        if (cls != 12) {
            put64(p + 8, ft_from(st.st_birthtime)); put64(p + 16, ft_from(st.st_atime));
            put64(p + 24, ft_from(st.st_mtime)); put64(p + 32, ft_from(st.st_mtime));
            put64(p + 40, st.st_size); put64(p + 48, (st.st_size + 2047) & ~2047ll);
            ST32(p + 56, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
            ST32(p + 60, nl);
        } else ST32(p + 8, nl);
        memcpy(G + p + hdr, e->d_name, nl);                         /* Xbox uses ANSI names here */
        ST32(p, 0);                                                 /* NextEntryOffset */
        return io_done(a[4], 0, hdr + nl);
    }
    return io_done(a[4], STATUS_NO_MORE_FILES, 0);
}

/* ---- symbolic links ---- */
uint32_t k_IoCreateSymbolicLink(CPU *c, const uint32_t *a) {         /* LinkName*, DeviceName* (ANSI_STRING) */
    (void)c;
    char n[256], t[256];
    gcopy(n, sizeof n, a[0]); gcopy(t, sizeof t, a[1]);
    init_links();
    add_link(n, t);
    return 0;
}
uint32_t k_IoDeleteSymbolicLink(CPU *c, const uint32_t *a) {
    (void)c;
    char n[256];
    gcopy(n, sizeof n, a[0]);
    for (int i = 0; i < nlinks; i++)
        if (!strcasecmp(links[i].name, n)) { links[i] = links[--nlinks]; return 0; }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}
uint32_t k_NtOpenSymbolicLinkObject(CPU *c, const uint32_t *a) {     /* Handle*, ObjAttr* */
    (void)c;
    char nt[1100];
    if (obj_path(a[1], nt, sizeof nt)) return STATUS_OBJECT_PATH_NOT_FOUND;
    init_links();
    for (int i = 0; i < nlinks; i++)
        if (!strcasecmp(links[i].name, nt)) { ST32(a[0], handle_new(3, (uint32_t)i)); return 0; }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}
uint32_t k_NtQuerySymbolicLinkObject(CPU *c, const uint32_t *a) {    /* Handle, LinkTarget* (ANSI_STRING), ReturnedLength* */
    (void)c;
    uint32_t i = handle_obj(a[0]);
    if (i >= (uint32_t)nlinks) return STATUS_INVALID_HANDLE;
    uint32_t len = (uint32_t)strlen(links[i].target), cap = LD16(a[1] + 2);
    if (len > cap) return STATUS_BUFFER_OVERFLOW;
    memcpy(G + LD32(a[1] + 4), links[i].target, len);
    ST16(a[1], len);
    if (a[2]) ST32(a[2], len);
    return 0;
}

/* NtUserIoApcDispatcher(ApcContext, IoStatusBlock, Reserved): the kernel's APC routine for ReadFileEx/WriteFileEx.
 * ApcContext is the game's LPOVERLAPPED_COMPLETION_ROUTINE; the OVERLAPPED starts with the IO_STATUS_BLOCK. */
uint32_t k_NtUserIoApcDispatcher(CPU *c, const uint32_t *a) {
    uint32_t status = LD32(a[1]), info = LD32(a[1] + 4);
    uint32_t err = status == 0 ? 0 : status == STATUS_END_OF_FILE ? 38 : 317;
    uint32_t args[3] = { err, info, a[1] };
    if (a[0]) guest_call(c, a[0], 3, args);
    return 0;
}
