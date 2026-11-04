#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Run once after board_init(), before tasks/audio start. No framebuffer or
 * task is allocated. Success means commands were sent, not LCD readback. */
int RunScreenBringupTest(void);

#ifdef __cplusplus
}
#endif
