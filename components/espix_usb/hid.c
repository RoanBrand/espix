/*
 * Local keyboard and mouse, over the USB host port.
 *
 * The console was always meant to have these: `docs/DISPLAY.md` says the input
 * path exists so that "a viewer says where the pointer is, a local mouse says how
 * far it moved". This is the second source. It needs no new plumbing --
 * espix_display_input() is the one entry point -- only the decoding.
 *
 * Two things about HID that decide the shape of this file:
 *
 *  - **A report is state, not an event.** The boot keyboard report is the set of
 *    keys held *now*: eight bytes, a modifier bitmap and six usage slots. A
 *    keystroke only exists as the difference between this report and the last,
 *    so the previous report is kept and every report is compared against it --
 *    both directions, because a key that left the set is a key released. Reading
 *    it as an event stream gets you a character per report, including the
 *    reports the keyboard sends while nothing changes.
 *  - **Modifiers are ordinary keys with their own keysyms.** A viewer delivers
 *    Shift as a key event and the shifted keysym on the key itself, and so must
 *    this: the console tracks Control to turn ^C into 0x03, and a desktop that
 *    only ever saw 'A' could not tell Shift-A from Caps Lock. So modifier
 *    transitions are emitted first, then the key.
 *
 * Only boot-protocol interfaces are decoded -- bInterfaceSubClass 1, which is
 * the interface telling us the fixed report layout is available. Anything else
 * is logged with that as the reason rather than ignored: a device that does not
 * work should say why. Report-descriptor parsing would lift that, and is not
 * here.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "usb/hid.h"
#include "usb/hid_host.h"

#include "espix_display.h"
#include "espix_kernel.h"

#include "espix_usb_priv.h"

#define TAG "usb:hid"

/* Byte 0 of an 8-byte boot keyboard report, in bit order. */
static const uint32_t s_mod_keysym[8] = {
    0xFFE3,   /* Left Control  */
    0xFFE1,   /* Left Shift    */
    0xFFE9,   /* Left Alt      */
    0xFFEB,   /* Left GUI      */
    0xFFE4,   /* Right Control */
    0xFFE6,   /* Right Shift   */
    0xFFE7,   /* Right Alt     */
    0xFFE8,   /* Right GUI     */
};

static hid_host_device_handle_t s_kb;
static uint8_t s_kb_mods;
static uint8_t s_kb_usage[6];    /* what is held, in the report's six slots */
static uint32_t s_kb_keysym[6];  /* what was emitted for it, to release the same */

static hid_host_device_handle_t s_mouse;
static uint8_t s_mouse_buttons;

static void hid_emit_key(uint32_t keysym, bool down)
{
    const espix_input_event_t ev = {
        .kind   = ESPIX_INPUT_KEY,
        .keysym = keysym,
        .down   = down,
    };
    espix_display_input(&ev);
}

/*
 * HID usage to X11 keysym, which is the currency here because it is what a
 * viewer sends and what espix_keysym_char() reads. Only the page 7 (keyboard)
 * usages a keyboard actually has are listed; 0 means "no text meaning here".
 */
static uint32_t usage_keysym(uint8_t u)
{
    if (u >= 0x04 && u <= 0x1D) {
        return (uint32_t)('a' + (u - 0x04));
    }
    if (u >= 0x1E && u <= 0x26) {
        return (uint32_t)('1' + (u - 0x1E));
    }
    if (u >= 0x3A && u <= 0x45) {
        return 0xFFBE + (uint32_t)(u - 0x3A);          /* F1..F12 */
    }

    switch (u) {
    case 0x27: return '0';
    case 0x28: return 0xFF0D;   /* Return */
    case 0x29: return 0xFF1B;   /* Escape */
    case 0x2A: return 0xFF08;   /* BackSpace */
    case 0x2B: return 0xFF09;   /* Tab */
    case 0x2C: return ' ';
    case 0x2D: return '-';
    case 0x2E: return '=';
    case 0x2F: return '[';
    case 0x30: return ']';
    case 0x31: return '\\';
    case 0x33: return ';';
    case 0x34: return '\'';
    case 0x35: return '`';
    case 0x36: return ',';
    case 0x37: return '.';
    case 0x38: return '/';
    case 0x39: return 0xFFE5;   /* Caps_Lock */
    case 0x49: return 0xFF63;   /* Insert */
    case 0x4A: return 0xFF50;   /* Home */
    case 0x4B: return 0xFF55;   /* Page_Up */
    case 0x4C: return 0xFFFF;   /* Delete */
    case 0x4D: return 0xFF57;   /* End */
    case 0x4E: return 0xFF56;   /* Page_Down */
    case 0x4F: return 0xFF53;   /* Right */
    case 0x50: return 0xFF51;   /* Left */
    case 0x51: return 0xFF54;   /* Down */
    case 0x52: return 0xFF52;   /* Up */
    default:   return 0;
    }
}

/* The keysyms Shift turns into, US layout -- which is what a viewer sends the
 * shifted form of and what espix_keysym_char() therefore expects. Caps Lock is
 * deliberately not folded in: the host applies it, and a keyboard that has it on
 * sends the shifted usage for letter keys anyway. */
static uint32_t shifted(uint32_t ks)
{
    if (ks >= 'a' && ks <= 'z') {
        return ks - 'a' + 'A';
    }
    switch (ks) {
    case '1': return '!';   case '2': return '@';   case '3': return '#';
    case '4': return '$';   case '5': return '%';   case '6': return '^';
    case '7': return '&';   case '8': return '*';   case '9': return '(';
    case '0': return ')';
    case '-': return '_';   case '=': return '+';   case '[': return '{';
    case ']': return '}';   case '\\': return '|';  case ';': return ':';
    case '\'': return '"';  case '`': return '~';   case ',': return '<';
    case '.': return '>';   case '/': return '?';
    default:  return ks;
    }
}

/*
 * The diff. Releases are emitted before presses so that a key rolled from one
 * to another -- which for one report is both at once -- is never left stuck
 * down, and modifiers before either so that Control is known before the key it
 * modifies arrives.
 */
static void hid_keyboard(const uint8_t *r, size_t n)
{
    if (n < 8) {
        return;
    }

    const uint8_t mods = r[0];
    const uint8_t changed = (uint8_t)(mods ^ s_kb_mods);
    for (int b = 0; b < 8 && changed != 0; b++) {
        const uint8_t bit = (uint8_t)(1u << b);
        if ((changed & bit) != 0) {
            hid_emit_key(s_mod_keysym[b], (mods & bit) != 0);
        }
    }
    s_kb_mods = mods;

    const bool shift = (mods & 0x22) != 0;     /* either Shift */

    for (int i = 0; i < 6; i++) {
        const uint8_t held = s_kb_usage[i];
        if (held == 0) {
            continue;
        }
        bool still = false;
        for (int j = 2; j < 8; j++) {
            if (r[j] == held) {
                still = true;
            }
        }
        if (!still) {
            /* The keysym that was pressed, not the one the current modifiers
             * would produce -- a release has to match its own press. */
            hid_emit_key(s_kb_keysym[i], false);
            s_kb_usage[i]  = 0;
            s_kb_keysym[i] = 0;
        }
    }

    for (int j = 2; j < 8; j++) {
        const uint8_t u = r[j];
        if (u == 0) {
            continue;
        }
        bool had = false;
        for (int i = 0; i < 6; i++) {
            if (s_kb_usage[i] == u) {
                had = true;
            }
        }
        if (had) {
            continue;
        }
        uint32_t ks = usage_keysym(u);
        if (ks == 0) {
            continue;
        }
        ks = shift ? shifted(ks) : ks;
        hid_emit_key(ks, true);

        /* Into the first free slot, so the report's own slot order does not
         * have to be preserved across reports. */
        for (int i = 0; i < 6; i++) {
            if (s_kb_usage[i] == 0) {
                s_kb_usage[i]  = u;
                s_kb_keysym[i] = ks;
                break;
            }
        }
    }
}

static void hid_mouse(const uint8_t *r, size_t n)
{
    if (n < 3) {
        return;
    }

    const int8_t dx = (int8_t)r[1];
    const int8_t dy = (int8_t)r[2];

    if (dx != 0 || dy != 0) {
        /* A delta, because that is what a mouse sends and what the service
         * wants: it owns the position, and two sources feed it. */
        const espix_input_event_t ev = {
            .kind = ESPIX_INPUT_MOTION,
            .x    = dx,
            .y    = dy,
        };
        espix_display_input(&ev);
    }

    /*
     * Buttons travel on a POINTER event, which carries the position as well --
     * so this one is sent with the position the service already has, and exists
     * to carry the mask. Nothing consumes it yet: there is no window manager to
     * click at, and inventing a meaning for a click before there is one would be
     * guessing.
     */
    /*
     * HID's button byte is buttons 1, 2, 3 in bits 0, 1, 2 -- left, *right*,
     * middle. The event model carries the RFB mask, whose bit 1 is the middle
     * button and bit 2 the right, and Doom's button numbering follows that
     * (mouseb_fire is 0, mouseb_strafe 1, mouseb_forward 2). So the two low bits
     * are swapped on the way in; without it a local mouse's right and middle
     * buttons change place the moment an owner cares which is which.
     */
    const uint8_t mask = (uint8_t)((r[0] & 0x01) |
                                   ((r[0] & 0x02) ? 0x04 : 0x00) |
                                   ((r[0] & 0x04) ? 0x02 : 0x00));

    if (mask != s_mouse_buttons) {
        s_mouse_buttons = mask;
        int x = 0, y = 0;
        espix_display_pointer(&x, &y);
        const espix_input_event_t ev = {
            .kind    = ESPIX_INPUT_POINTER,
            .x       = (int16_t)x,
            .y       = (int16_t)y,
            .buttons = s_mouse_buttons,
        };
        espix_display_input(&ev);
    }
}

static void hid_interface_cb(hid_host_device_handle_t dev,
                             const hid_host_interface_event_t event, void *arg)
{
    (void)arg;

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        uint8_t     report[16];
        size_t      len = 0;
        hid_host_dev_params_t params;

        if (hid_host_device_get_raw_input_report_data(dev, report, sizeof(report),
                                                      &len) != ESP_OK ||
            hid_host_device_get_params(dev, &params) != ESP_OK || len == 0) {
            return;
        }
        if (params.proto == HID_PROTOCOL_KEYBOARD) {
            hid_keyboard(report, len);
        } else if (params.proto == HID_PROTOCOL_MOUSE) {
            hid_mouse(report, len);
        }
        break;
    }

    case HID_HOST_INTERFACE_EVENT_DISCONNECTED: {
        (void)hid_host_device_close(dev);

        /* Whatever it was holding is not held any more: releasing it is the
         * difference between an unplugged keyboard and a stuck Control key.
         * Compared against the handles we still have rather than through the
         * device, whose handle this is not. */
        if (dev == s_kb) {
            for (int b = 0; b < 8; b++) {
                if ((s_kb_mods & (1u << b)) != 0) {
                    hid_emit_key(s_mod_keysym[b], false);
                }
            }
            for (int i = 0; i < 6; i++) {
                if (s_kb_usage[i] != 0) {
                    hid_emit_key(s_kb_keysym[i], false);
                    s_kb_usage[i]  = 0;
                    s_kb_keysym[i] = 0;
                }
            }
            s_kb_mods = 0;
            s_kb      = NULL;
            espix_klog(ESPIX_KLOG_INFO, TAG, "keyboard gone");
        }
        if (dev == s_mouse) {
            s_mouse = NULL;
            espix_klog(ESPIX_KLOG_INFO, TAG, "mouse gone");
        }
        break;
    }

    default:
        break;
    }
}

static void hid_driver_cb(hid_host_device_handle_t dev,
                          const hid_host_driver_event_t event, void *arg)
{
    (void)arg;

    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) {
        return;
    }

    hid_host_dev_params_t params;
    if (hid_host_device_get_params(dev, &params) != ESP_OK) {
        return;
    }

    if (params.sub_class != 1) {
        /* Not a boot interface, so the report layout is a descriptor we would
         * have to parse. Say so once, per interface, with the reason. */
        espix_klog(ESPIX_KLOG_INFO, TAG,
                   "iface %u on address %u is HID subclass %u, not a boot "
                   "interface; not decoded",
                   (unsigned)params.iface_num, (unsigned)params.addr,
                   (unsigned)params.sub_class);
        return;
    }

    const hid_host_device_config_t dev_config = {
        .callback     = hid_interface_cb,
        .callback_arg = NULL,
    };
    const esp_err_t err = hid_host_device_open(dev, &dev_config);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "cannot open iface %u: %s",
                   (unsigned)params.iface_num, esp_err_to_name(err));
        return;
    }

    /*
     * Boot protocol, and this is the request the whole file turns on.
     *
     * It is sent even though the host will report it as failed on a Logitech
     * receiver, because the receiver obeys it anyway: the control transfer on
     * the keyboard interface sits out its full five-second timeout and the one
     * on the mouse interface cannot even be submitted, and then the mouse works.
     * Take the request out and the mouse stops dead -- not misread, silent --
     * which says the interface was in report protocol and sends nothing useful
     * there. The failure is in the host's status-stage handling, not in the
     * device's compliance.
     *
     * The timeout is why this runs *before* start() rather than after: the
     * interface is started afterwards, so the ten seconds are paid once, at
     * attach, and never on the report path. It is also not peculiar to this
     * receiver -- tinyusb carries the same behaviour open as
     * https://github.com/hathach/tinyusb/issues/2436, where toggling protocol
     * on the mouse interface stops the keyboard interface being serviced at all
     * while its endpoint goes on replying.
     *
     * Worth revisiting: five seconds is the library's timeout, and a device that
     * answers by not answering costs ten of them for a two-interface receiver.
     */
    const esp_err_t proto = hid_class_request_set_protocol(dev, HID_REPORT_PROTOCOL_BOOT);
    if (proto != ESP_OK) {
        espix_klog(ESPIX_KLOG_INFO, TAG,
                   "iface %u reported %s to the boot protocol request; the "
                   "receiver obeys it anyway",
                   (unsigned)params.iface_num, esp_err_to_name(proto));
    }

    if (hid_host_device_start(dev) != ESP_OK) {
        (void)hid_host_device_close(dev);
        return;
    }

    switch (params.proto) {
    case HID_PROTOCOL_KEYBOARD:
        s_kb = dev;
        espix_klog(ESPIX_KLOG_INFO, TAG, "keyboard on address %u, iface %u",
                   (unsigned)params.addr, (unsigned)params.iface_num);
        break;
    case HID_PROTOCOL_MOUSE:
        s_mouse = dev;
        espix_klog(ESPIX_KLOG_INFO, TAG, "mouse on address %u, iface %u",
                   (unsigned)params.addr, (unsigned)params.iface_num);
        break;
    default:
        espix_klog(ESPIX_KLOG_INFO, TAG,
                   "iface %u on address %u is a boot interface with no boot "
                   "protocol (%u); not decoded",
                   (unsigned)params.iface_num, (unsigned)params.addr,
                   (unsigned)params.proto);
        (void)hid_host_device_close(dev);
        break;
    }
}

esp_err_t espix_usb_hid_start(void)
{
    const hid_host_driver_config_t config = {
        /*
         * Its own task, deliberately, and not for convenience. The attach
         * callback below (hid_interface_cb) makes a boot-protocol control
         * transfer that can sit out the library's whole five-second timeout on
         * some receivers, twice over; running that inside usb:host would stall
         * the event loop that delivers the completions it is waiting for. MSC
         * is merged into usb:host because its callbacks only queue (R-P5.2);
         * HID cannot be, and this is why.
         */
        .create_background_task = true,
        .task_priority          = 4,
        .stack_size             = 4096,
        .core_id                = tskNO_AFFINITY,
        .callback               = hid_driver_cb,
        .callback_arg           = NULL,
    };

    const esp_err_t err = hid_host_install(&config);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "hid_host_install: %s",
                   esp_err_to_name(err));
        return err;
    }

    /* Said out loud, because the alternative is silence: a keyboard that is
     * plugged in and not decoded and a keyboard that is not plugged in look
     * exactly the same otherwise. */
    espix_klog(ESPIX_KLOG_INFO, TAG, "HID class driver up, watching for a "
                                    "keyboard or mouse");
    return ESP_OK;
}