/*
 * The desktop: what is on the screen when something is.
 *
 * A client of the display service, not part of it -- the same relationship the
 * on-screen console has. espix_display owns the canvas, the surfaces, the
 * pointer and the backends; this owns what is drawn on them and claims the
 * screen as an owner when it is asked to.
 *
 * It lives in the kernel image for now, which is a decision about packaging
 * rather than about layering: the same code moves to apps/ once the client
 * surface it draws through has settled, because an ABI is easier to keep than
 * to change.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Claim the screen with the desktop, or give it back.
 *
 * Claiming takes over rather than being refused, so starting it twice is
 * harmless. ESP_ERR_INVALID_STATE means there is no canvas to claim, which is
 * what `desktop start` as the first command after a boot looks like.
 */
esp_err_t espix_desktop_start(void);
void      espix_desktop_stop(void);

#ifdef __cplusplus
}
#endif