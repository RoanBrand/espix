/*
 * The role split, in one place.
 *
 * The OTG peripheral is a singleton on the S3 -- SOC_USB_OTG_PERIPH_NUM is 1 --
 * so a build is a host or a device and there is no runtime switch to fall back
 * on. espix_net/Kconfig asks which, and this file answers the question for code
 * that must not care: usb.c is compiled either way so that `lsblk` in a
 * device-role build can say which build it is rather than looking like a host
 * driver that failed to find a device.
 *
 * The naming tables live here too, for the same reason: they are data about the
 * USB spec rather than about the host stack, and `lsusb` asks for them in a
 * build where no host code is compiled.
 */

#include "sdkconfig.h"

#include <string.h>

#include "usb/usb_types_ch9.h"

#include "espix_usb.h"
#include "espix_usb_priv.h"

bool espix_usb_host_built(void)
{
#if CONFIG_ESPIX_USB_ROLE_HOST
    return true;
#else
    return false;
#endif
}

esp_err_t espix_usb_init(void)
{
#if CONFIG_ESPIX_USB_ROLE_HOST
    return espix_usb_host_init();
#else
    /*
     * The port belongs to TinyUSB in this build. Reporting an error here would
     * make a correct configuration look like a failure at boot, and the boot
     * sequence's job is to report what is actually wrong.
     */
    return ESP_OK;
#endif
}

bool espix_usb_present(void)
{
#if CONFIG_ESPIX_USB_ROLE_HOST
    return espix_usb_host_present();
#else
    return false;
#endif
}

size_t espix_usb_devlist(espix_usb_dev_t *out, size_t n)
{
#if CONFIG_ESPIX_USB_ROLE_HOST
    return espix_usb_host_devlist(out, n);
#else
    (void)out;
    (void)n;
    return 0;
#endif
}

void espix_usb_host_status(espix_usb_host_status_t *out)
{
    if (out == NULL) {
        return;
    }
#if CONFIG_ESPIX_USB_ROLE_HOST
    espix_usb_host_status_query(out);
#else
    memset(out, 0, sizeof(*out));
#endif
}

size_t espix_usb_host_devices(espix_usb_desc_t *out, size_t n)
{
#if CONFIG_ESPIX_USB_ROLE_HOST
    return espix_usb_host_device_list(out, n);
#else
    (void)out;
    (void)n;
    return 0;
#endif
}

esp_err_t espix_usb_host_probe(uint8_t addr)
{
#if CONFIG_ESPIX_USB_ROLE_HOST
    return espix_usb_host_probe_addr(addr);
#else
    (void)addr;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

size_t espix_usb_host_scan(void)
{
#if CONFIG_ESPIX_USB_ROLE_HOST
    return espix_usb_host_scan_pool();
#else
    return 0;
#endif
}

const char *espix_usb_class_name(uint8_t class)
{
    switch (class) {
    case USB_CLASS_PER_INTERFACE:        return "per-interface";
    case USB_CLASS_AUDIO:                return "audio";
    case USB_CLASS_COMM:                 return "communications";
    case USB_CLASS_HID:                  return "human interface";
    case USB_CLASS_PHYSICAL:             return "physical";
    case USB_CLASS_STILL_IMAGE:          return "imaging";
    case USB_CLASS_PRINTER:              return "printer";
    case USB_CLASS_MASS_STORAGE:         return "mass storage";
    case USB_CLASS_HUB:                  return "hub";
    case USB_CLASS_CDC_DATA:             return "cdc data";
    case USB_CLASS_CSCID:                return "smart card";
    case USB_CLASS_CONTENT_SEC:          return "content security";
    case USB_CLASS_VIDEO:                return "video";
    case USB_CLASS_PERSONAL_HEALTHCARE:  return "healthcare";
    case USB_CLASS_AUDIO_VIDEO:          return "audio/video";
    case USB_CLASS_BILLBOARD:            return "billboard";
    case USB_CLASS_USB_TYPE_C_BRIDGE:    return "type-c bridge";
    case USB_CLASS_WIRELESS_CONTROLLER:  return "wireless";
    case USB_CLASS_MISC:                 return "miscellaneous";
    case USB_CLASS_APP_SPEC:             return "application specific";
    default:                             return "";
    }
}

bool espix_usb_iface_is_msc_bot(uint8_t class, uint8_t subclass, uint8_t protocol)
{
    /* The same three values msc_host.c's find_msc_interface() demands. */
    return class == USB_CLASS_MASS_STORAGE && subclass == 0x06 && protocol == 0x50;
}