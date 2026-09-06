/*
 * USB-NCM: an Ethernet link over the USB-OTG port, as usb0.
 *
 * Plug the board into a computer and it appears there as a USB Ethernet
 * adapter. No WiFi credentials, no access point, no router -- which makes it
 * both the easiest way to reach a fresh device and the most reliable way to
 * reach one on a bench where WiFi is the flakiest thing in the room.
 *
 * Two modes, in /etc/usb.conf:
 *
 *   server   espix takes a fixed address and runs DHCP, so the computer gets
 *            an address just by having the cable plugged in. The default.
 *   client   espix runs a DHCP client, for a computer that bridges or shares
 *            its own network over the link.
 *
 * Built on esp_tinyusb's tinyusb_net API rather than raw tud_network_*
 * callbacks, for one reason worth stating: tud_network_xmit() must run on
 * TinyUSB's own task, and tinyusb_net_send_async() marshals it there through
 * usbd_defer_func(). Hand-rolling the glue means rediscovering that, because
 * esp_netif calls transmit on whatever task the output path happens to be.
 *
 * Nothing here touches the console. espix's console is the UART with
 * USB-Serial-JTAG secondary; neither is the USB-OTG peripheral this claims, so
 * on a devkit with two sockets the console stays up while this comes and goes.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/esp_netif_net_stack.h"   /* ethernetif_init/_input: NCM is Ethernet to lwip */

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_net_priv.h"

#define TAG "usbnet"

#define USB_CONF_PATH   "/etc/usb.conf"
#define USB_IF_NAME     "usb0"

static esp_netif_t      *s_netif;
static espix_usb_mode_t  s_mode  = ESPIX_USB_MODE_SERVER;
static bool              s_attached;      /* host has enumerated us */
static bool              s_started;

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

esp_err_t espix_net_conf_write_usb(espix_usb_mode_t mode)
{
    FILE *f = fopen(USB_CONF_PATH, "w");
    if (f == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot write %s: %s",
                   USB_CONF_PATH, strerror(errno));
        return ESP_FAIL;
    }

    fprintf(f, "# espix USB-NCM configuration\n");
    fprintf(f, "mode=%s\n",
            (mode == ESPIX_USB_MODE_CLIENT) ? "client" : "server");
    fprintf(f, "# server: espix runs DHCP on usb0 and takes %s\n",
            CONFIG_ESPIX_USB_NCM_ADDRESS);
    fprintf(f, "# client: espix asks the computer's network for an address\n");
    fclose(f);

    /* Nothing secret in here, unlike wifi.conf -- but it is a device-wide
     * setting, so writing it stays root's. */
    (void)espix_fs_ensure_mode(USB_CONF_PATH, 0644);
    return ESP_OK;
}

static espix_usb_mode_t load_mode(void)
{
    char v[16];

    if (espix_fs_conf_get(USB_CONF_PATH, "mode", v, sizeof(v))) {
        if (strcmp(v, "client") == 0) {
            return ESPIX_USB_MODE_CLIENT;
        }
        if (strcmp(v, "server") == 0) {
            return ESPIX_USB_MODE_SERVER;
        }
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "%s: mode '%s' is neither server nor client; using the default",
                   USB_CONF_PATH, v);
    }

#if CONFIG_ESPIX_USB_NCM_MODE_CLIENT
    return ESPIX_USB_MODE_CLIENT;
#else
    return ESPIX_USB_MODE_SERVER;
#endif
}

/* ------------------------------------------------------------------ */
/* esp_netif <-> tinyusb_net glue                                      */
/* ------------------------------------------------------------------ */

/*
 * A frame from the host. `buffer` is TinyUSB's own receive buffer and is reused
 * the moment this returns, so the copy is not defensive: esp_netif_receive()
 * takes ownership of what it is given and frees it later through
 * driver_free_rx_buffer below.
 */
static esp_err_t ncm_recv(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;

    void *copy = malloc(len);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;      /* the peer will retransmit */
    }
    memcpy(copy, buffer, len);
    esp_netif_receive(s_netif, copy, len, NULL);
    return ESP_OK;
}

static void ncm_free_rx(void *h, void *buffer)
{
    (void)h;
    free(buffer);
}

/* Once the deferred send has resolved, however it resolved. */
static void ncm_free_tx(void *buffer, void *ctx)
{
    (void)ctx;
    free(buffer);
}

/*
 * esp_netif's transmit, called from whatever task is driving the output path.
 *
 * Copied because esp_netif may reuse its buffer the moment this returns, and
 * the send is asynchronous. Freed here rather than in ncm_free_tx() when the
 * send is refused: tinyusb_net_send_async() only arranges that callback once it
 * has accepted the packet, so on an error the copy is ours and would leak.
 *
 * A refusal means TinyUSB could not take it -- the host is not reading, or the
 * NTB buffers are full. Dropping is the right answer and the one every link
 * layer gives; TCP retransmits exactly as it would for a dropped Ethernet
 * frame, and a retry loop here would only add latency to a link that is
 * already behind.
 */
static esp_err_t ncm_transmit(void *h, void *buffer, size_t len)
{
    (void)h;

    void *copy = malloc(len);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, buffer, len);

    const esp_err_t err = tinyusb_net_send_async(copy, (uint16_t)len, copy);
    if (err != ESP_OK) {
        free(copy);
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* Link state                                                          */
/* ------------------------------------------------------------------ */

/*
 * There is no PHY on a USB link and so no link-up event, and esp_netif will not
 * start DHCP on its own: esp_netif_action_start() only brings the interface up
 * administratively, and it is action_connected() that starts the client or the
 * server. Enumeration is the closest thing to a carrier a gadget has, so that
 * is what drives it.
 *
 * This is also what makes `ip link` honest -- usb0 is listed whenever the
 * feature is built in, and reads UP or DOWN according to whether a computer is
 * on the other end. Without it the interface would claim to be up with no cable
 * in it, and DHCP would fire into a void at boot.
 *
 * Runs on TinyUSB's task. esp_netif_action_* post to the event loop rather than
 * doing the work inline, so there is nothing here that wants a task of its own.
 */
static void on_usb_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;

    if (s_netif == NULL) {
        return;
    }

    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        s_attached = true;
        espix_klog(ESPIX_KLOG_INFO, TAG, "%s: host attached", USB_IF_NAME);

        /*
         * action_connected() brings the netif up and, for a DHCP *client*,
         * starts it. It does nothing for a server: esp_netif only starts one
         * from esp_netif_start(), and only if the netif is already up -- which
         * needs AUTOUP, which is exactly what we do not want. So server mode
         * starts it here, once there is a host to answer.
         */
        esp_netif_action_connected(s_netif, NULL, 0, NULL);
        if (s_mode == ESPIX_USB_MODE_SERVER) {
            const esp_err_t err = esp_netif_dhcps_start(s_netif);
            if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
                espix_klog(ESPIX_KLOG_WARN, TAG, "%s: dhcp server: %s",
                           USB_IF_NAME, esp_err_to_name(err));
            }
        }
        break;

    case TINYUSB_EVENT_DETACHED:
        s_attached = false;
        espix_klog(ESPIX_KLOG_INFO, TAG, "%s: host detached", USB_IF_NAME);
        if (s_mode == ESPIX_USB_MODE_SERVER) {
            (void)esp_netif_dhcps_stop(s_netif);
        }
        esp_netif_action_disconnected(s_netif, NULL, 0, NULL);
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

/*
 * Server mode offers an address and a netmask, and deliberately no router and
 * no DNS.
 *
 * espix is not a router. A DHCP offer carrying a gateway is taken by most
 * systems as a candidate default route, so plugging a board into a laptop could
 * silently cost that laptop its internet -- a failure that looks like the
 * network breaking at the instant a cable went in, which nobody debugs quickly.
 * The computer still reaches espix directly, because the two are on-link, and
 * that is the whole job.
 */
static void configure_dhcp_server(void)
{
    esp_netif_ip_info_t ip = {0};

    ip.ip.addr = esp_ip4addr_aton(CONFIG_ESPIX_USB_NCM_ADDRESS);
    ip.gw.addr = ip.ip.addr;            /* our own address; not offered out */
    ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");

    (void)esp_netif_dhcps_stop(s_netif);
    if (esp_netif_set_ip_info(s_netif, &ip) != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: address %s refused",
                   USB_IF_NAME, CONFIG_ESPIX_USB_NCM_ADDRESS);
        return;
    }

    /*
     * Suppress the router option. Verified on the wire: no `router` appears in
     * the offer, and a laptop keeps its own default route with the cable in.
     *
     * Must happen while the server is stopped, which is why the stop above is
     * not merely tidiness, and the result is checked because the whole point of
     * the call is that something does *not* appear.
     */
    const uint8_t off = 0;
    const esp_err_t err = esp_netif_dhcps_option(
        s_netif, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS,
        (void *)&off, sizeof(off));

    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "%s: could not suppress the router option: %s",
                   USB_IF_NAME, esp_err_to_name(err));
    }

    /*
     * The DNS option cannot be suppressed, and espix does not try.
     *
     * ESP-IDF's DHCP server emits a DNS option unconditionally: when none is
     * configured it falls into an else branch that advertises *its own address*
     * (dhcpserver.c, "Add DNS option if either main or backup DNS is set" --
     * the else immediately below it). So `dhcps_dns = 0` means "advertise
     * myself", not "advertise nothing", and there is no value of the documented
     * option that removes it. Measured, not assumed: setting it returns ESP_OK
     * and the option still arrives on the wire. See docs/UPSTREAM.md.
     *
     * The consequence is that a computer is told espix is a name server, and
     * espix runs no resolver. It is mild -- the host keeps its own default
     * route and its own DNS, and both Linux and macOS scope a resolver to the
     * interface that supplied it -- but it is a pointer to something that is
     * not there, and it is not espix's choice.
     */
}

static esp_err_t create_netif(const uint8_t *mac)
{
    /*
     * route_prio 50, which is what IDF gives Ethernet, against WiFi's 100. With
     * both interfaces addressed the default route stays on wlan0, so plugging
     * in a cable adds a way to reach the board without redirecting everything
     * the board itself sends.
     */
    const bool server = (s_mode == ESPIX_USB_MODE_SERVER);

    /*
     * Deliberately no ESP_NETIF_FLAG_AUTOUP. With it, esp_netif_start() brings
     * the lwip netif up immediately and `ip link` reports usb0 UP with nothing
     * plugged in -- which is a lie, and the one a user would notice first.
     * Without it the interface is created and listed but stays down until
     * on_usb_event() sees the host.
     */
    esp_netif_inherent_config_t base = {
        .flags = (esp_netif_flags_t)(server ? ESP_NETIF_DHCP_SERVER
                                            : ESP_NETIF_DHCP_CLIENT),
        .ip_info       = NULL,
        .get_ip_event  = server ? 0 : IP_EVENT_ETH_GOT_IP,
        .lost_ip_event = server ? 0 : IP_EVENT_ETH_LOST_IP,
        .if_key        = "USB_NCM",
        .if_desc       = USB_IF_NAME,
        .route_prio    = 50,
        .bridge_info   = NULL,
    };

    static const esp_netif_driver_ifconfig_t driver = {
        .handle                = (void *)1,   /* singleton; must be non-NULL */
        .transmit              = ncm_transmit,
        .driver_free_rx_buffer = ncm_free_rx,
    };

    static const esp_netif_netstack_config_t stack = {
        .lwip = {
            .init_fn  = ethernetif_init,
            .input_fn = ethernetif_input,
        },
    };

    esp_netif_config_t cfg = { .base = &base, .driver = &driver, .stack = &stack };

    s_netif = esp_netif_new(&cfg);
    if (s_netif == NULL) {
        return ESP_FAIL;
    }
    if (esp_netif_set_mac(s_netif, (uint8_t *)mac) != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: could not set the MAC", USB_IF_NAME);
    }

    if (server) {
        configure_dhcp_server();
    }

    /*
     * Started but not connected: the interface exists and is listed, and stays
     * down until a host enumerates it. on_usb_event() does the rest.
     */
    esp_netif_action_start(s_netif, NULL, 0, NULL);
    return ESP_OK;
}

esp_err_t espix_net_usb_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_mode = load_mode();

    /*
     * A USB Ethernet link has two MAC addresses, one at each end, and espix has
     * to supply both.
     *
     *   mac       espix's own end. What usb0 uses, and what `ip link` shows.
     *   host_mac  the *computer's* end. NCM's descriptor tells the host what
     *             address to give its interface, and the host adopts it -- so
     *             this is what appears in the host's own `ip link`/`ifconfig`,
     *             and what its interface naming and any bridge config key off.
     *
     * They must differ: they are two ends of one link, and giving both the same
     * address is a bug waiting for the first ARP.
     *
     * Both are derived from ESP_MAC_BASE through esp_derive_local_mac(), which
     * sets the locally-administered bit. Not ESP_MAC_ETH, which was the first
     * attempt and was wrong: IDF hands out four *universal* addresses from the
     * factory base -- WIFI_STA, WIFI_SOFTAP, BT and ETH -- and taking ETH for
     * usb0 spends the address the real eth0 will want on the S31 and P4, where
     * this feature and an Ethernet MAC will exist on the same board.
     *
     * Deriving a local address instead costs none of those four, is unique per
     * chip because the factory base is, and cannot collide with any of them
     * because the locally-administered bit puts it in a different space
     * entirely. That is what the function is for.
     */
    uint8_t base[6];
    esp_err_t err = esp_read_mac(base, ESP_MAC_BASE);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t mac[6];
    err = esp_derive_local_mac(mac, base);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t desc_mac[6];
    memcpy(desc_mac, mac, sizeof(desc_mac));
    desc_mac[5] ^= 0x01;            /* the other end of the same link */

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(on_usb_event, NULL);
    err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "tinyusb install failed: %s",
                   esp_err_to_name(err));
        return err;
    }

    tinyusb_net_config_t net_cfg = {
        .on_recv_callback = ncm_recv,
        .free_tx_buffer   = ncm_free_tx,
        .on_init_callback = NULL,
        .user_context     = NULL,
    };
    memcpy(net_cfg.mac_addr, desc_mac, sizeof(desc_mac));

    err = tinyusb_net_init(&net_cfg);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "tinyusb net init failed: %s",
                   esp_err_to_name(err));
        return err;
    }

    err = create_netif(mac);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: no netif", USB_IF_NAME);
        return err;
    }

    espix_net_register_if(USB_IF_NAME, ESPIX_IF_USB, s_netif);
    s_started = true;

    espix_klog(ESPIX_KLOG_INFO, TAG,
               "%s: %s mode, mac %02x:%02x:%02x:%02x:%02x:%02x, host %02x:%02x:%02x:%02x:%02x:%02x",
               USB_IF_NAME,
               (s_mode == ESPIX_USB_MODE_SERVER) ? "server" : "client",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
               desc_mac[0], desc_mac[1], desc_mac[2], desc_mac[3],
               desc_mac[4], desc_mac[5]);
    return ESP_OK;
}

void espix_net_usb_status(espix_usb_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    out->built    = true;
    out->started  = s_started;
    out->attached = s_attached;
    out->mode     = s_mode;

    if (s_netif != NULL) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            out->has_addr = true;
            out->ip       = ip.ip.addr;
            out->netmask  = ip.netmask.addr;
        }
    }
}
