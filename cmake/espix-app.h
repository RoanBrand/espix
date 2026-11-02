/*
 * Force-included into every M-mode app (cmake/espix-app.cmake): names the
 * kernel publishes that IDF's and lwIP's headers do not declare. A U-mode app
 * finds the same in user/include.
 */
#pragma once

const char *gai_strerror(int err);

/* espix's own, after lwIP's 200-204 (components/espix_proc/include/espix_gai.h). */
#ifndef EAI_AGAIN
#define EAI_AGAIN    205
#define EAI_BADFLAGS 206
#define EAI_SOCKTYPE 207
#define EAI_OVERFLOW 208
#define EAI_SYSTEM   209
#endif
