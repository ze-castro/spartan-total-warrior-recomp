/* Scanout: what the video encoder shows, and the host window it is presented in. */
#ifndef STW_DISPLAY_H
#define STW_DISPLAY_H
#include <stdint.h>

/* Called by the AV kernel functions / CRTC model. fb is a physical address. */
void display_set_mode(uint32_t mode, uint32_t format, uint32_t pitch, uint32_t fb);
void display_vblank(void);                           /* once per vertical blank: present the scanout buffer */
void display_set_start(uint32_t fb);                 /* PCRTC_START changed: a flip */

/* Host side (display_metal.m) */
void display_main_loop(void (*start_guest)(void));  /* runs on the main thread, never returns */
void display_present(uint32_t fb, uint32_t pitch, uint32_t width, uint32_t height, uint32_t format);
void display_present_tex(void *mtl_texture, uint32_t width, uint32_t height);   /* a GPU texture (id<MTLTexture>) shown as is */
void display_present_ptr(const void *px, uint32_t pitch, uint32_t width, uint32_t height);   /* host-memory pixels (BGRA) */
void display_set_aspect(double aspect);   /* shape of the picture about to be presented: 4/3, or 16/9 for the widescreen game picture */
#endif
