/* Internal to the espix_usb component. */
#pragma once

#include "espix_usb.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * host.c: the whole host role. Declared unconditionally and built only when the
 * role is host -- a declaration nothing calls costs nothing, and guarding it
 * would mean every caller repeating the same #if.
 */
esp_err_t espix_usb_host_init(void);
bool      espix_usb_host_present(void);
size_t    espix_usb_host_devlist(espix_usb_dev_t *out, size_t n);

/* host.c, for the role dispatchers in usb.c. */
void   espix_usb_host_status_query(espix_usb_host_status_t *out);
size_t espix_usb_host_device_list(espix_usb_desc_t *out, size_t n);
esp_err_t espix_usb_host_probe_addr(uint8_t addr);
size_t    espix_usb_host_scan_pool(void);

#ifdef __cplusplus
}
#endif