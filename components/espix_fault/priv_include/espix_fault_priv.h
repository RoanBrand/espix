/* Internal to the espix_fault component. */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create the reaper task and its request queue. */
esp_err_t espix_fault_reaper_start(void);

/* Log a boot-time notice if the coredump partition holds a valid dump. */
void espix_fault_report_coredump(void);

#ifdef __cplusplus
}
#endif
