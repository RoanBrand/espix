/* Internal to the espix_fault component. */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Log a boot-time notice if the coredump partition holds a valid dump. */
void espix_fault_report_coredump(void);

#ifdef __cplusplus
}
#endif
