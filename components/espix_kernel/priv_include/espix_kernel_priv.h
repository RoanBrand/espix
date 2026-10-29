/* Internal to the espix_kernel component. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the esp_log_set_vprintf() hook that mirrors ESP_LOGx into the
 * kernel log ring. No-op when CONFIG_ESPIX_KLOG_CAPTURE_ESP_LOG is disabled. */
void espix_klog_install_esp_log_hook(void);

/* The task espix_task_end() hands a task to. */
void espix_reaper_start(void);

#ifdef __cplusplus
}
#endif
