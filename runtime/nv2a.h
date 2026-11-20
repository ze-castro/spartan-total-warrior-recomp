/* NV2A GPU model. The register window is plain guest memory at NV2A_BASE; a device thread plays the
 * hardware's role (status bits, timers, push buffer consumption) by watching that memory. */
#ifndef STW_NV2A_H
#define STW_NV2A_H
#include <stdint.h>

#define NV2A_BASE   0xFD000000u
#define NV2A_PRAMIN (NV2A_BASE + 0x700000u)    /* instance memory window */
#define NV2A_USER   (NV2A_BASE + 0x800000u)    /* channel control (DMA_PUT/GET at +0x40/+0x44) */

void nv2a_update_pmc(void);                     /* recompute PMC_INTR_0 from the engines' pending status */
void nv2a_start(void);
int nv2a_scene_3d(void);                        /* a 3D scene (gameplay/cutscene) is being drawn, as opposed to flat menus/movies */                          /* start the device thread */
#endif
