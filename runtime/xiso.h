/* Reader / extractor for original-Xbox disc images (XDVDFS): redump-style full images (game partition at 0x18300000) and plain XISOs. */
#ifndef STW_XISO_H
#define STW_XISO_H
#include <stdint.h>
typedef struct XIso XIso;
XIso *xiso_open(const char *path);                                  /* NULL if the file is not an Xbox disc image */
void xiso_close(XIso *x);
uint8_t *xiso_read_file(XIso *x, const char *path, uint32_t *size);  /* malloc'd contents of e.g. "default.xbe", NULL if missing */
uint64_t xiso_total_bytes(XIso *x);                                  /* bytes of file data on the disc */
/* progress(done_bytes, total_bytes, current_file, ctx): return non-zero to cancel. Returns 0 on success, -1 on error / cancel (errmsg filled). */
typedef int (*xiso_progress)(uint64_t done, uint64_t total, const char *name, void *ctx);
int xiso_extract(XIso *x, const char *outdir, xiso_progress cb, void *ctx, char *errmsg, int errlen);
#endif
