/*
 * espix Ethernet (eth0).
 *
 * The S31 has a Gigabit MAC and no integrated PHY: an external PHY sits on
 * RGMII and is managed over SMI. This brings the MAC up, drives the PHY through
 * the generic 802.3 driver (which handles the standard registers and
 * auto-negotiation for any compliant PHY), gives it an esp_netif with a DHCP
 * client, and names it eth0 alongside wlan0 and usb0.
 *
 * The PHY on the S31 reference board is a Motorcomm YT8531, which needs two
 * non-standard fix-ups the generic driver cannot know: re-enabling
 * auto-negotiation after its reset disables it, and the RGMII Tx/Rx clock
 * delays. See ESPIX_ETH_PHY_YT8531. The pin defaults are that board's; a
 * different board overrides them in menuconfig.
 *
 * It is compiled only when CONFIG_ESPIX_ETH_ENABLED, which exists only where
 * SOC_EMAC_SUPPORTED does. On the S3 there is no MAC and no code.
 */

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "sdkconfig.h"

#include "esp_eth.h"

#include "espix_kernel.h"
#include "espix_net.h"
#include "espix_net_priv.h"

#define TAG "eth"

static esp_eth_handle_t            s_handle;
static esp_netif_t                *s_netif;
static esp_eth_netif_glue_handle_t s_glue;

static bool netif_has_addr(esp_netif_t *n)
{
    if (n == NULL) {
        return false;
    }
    esp_netif_ip_info_t ip;
    return esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr != 0;
}

/*
 * The route policy: Ethernet when it has an address, wireless otherwise.
 * Whichever comes first is never the rule -- a board with the cable plugged in
 * must use it even if WiFi associated first, and must fall back the moment the
 * link drops rather than waiting for the lease to expire. Both stacks call
 * esp_netif_set_default_netif() themselves as they come up, so this is
 * re-applied on every event that could change the answer.
 */
static void choose_default_route(void)
{
    if (netif_has_addr(s_netif)) {
        esp_netif_set_default_netif(s_netif);
        return;
    }

    const espix_if_entry_t *w = espix_net_find_if("wlan0");
    if (w != NULL && netif_has_addr(w->netif)) {
        esp_netif_set_default_netif(w->netif);
    }
}

#if CONFIG_ESPIX_ETH_PHY_YT8531
/*
 * YT8531 fix-ups, through the generic PHY driver's register ioctls. Both are
 * copied from Espressif's own S31 example, which is where this behaviour is
 * documented at all.
 */
static esp_err_t yt8531_fixups(void)
{
    esp_err_t err;
    uint32_t  val;
    esp_eth_phy_reg_rw_data_t reg = { .reg_addr = 0, .reg_value_p = &val };

    /* The chip disables auto-negotiation across a hardware reset; nothing in
     * the standard registers says so. */
    bool enable = true;
    err = esp_eth_ioctl(s_handle, ETH_CMD_S_AUTONEGO, &enable);
    if (err != ESP_OK) {
        return err;
    }

    /* Rx ~2 ns coarse delay (EXT_CHIP_CONFIG 0xA001, bit 8). */
    val = 0xA001;
    reg.reg_addr = 0x1E;   /* extended address register */
    err = esp_eth_ioctl(s_handle, ETH_CMD_WRITE_PHY_REG, &reg);
    if (err != ESP_OK) {
        return err;
    }
    reg.reg_addr = 0x1F;   /* extended data register */
    err = esp_eth_ioctl(s_handle, ETH_CMD_READ_PHY_REG, &reg);
    if (err != ESP_OK) {
        return err;
    }
    val |= (1U << 8);
    err = esp_eth_ioctl(s_handle, ETH_CMD_WRITE_PHY_REG, &reg);
    if (err != ESP_OK) {
        return err;
    }

    /* Tx ~2 ns delay (EXT_RGMII_CONFIG1 0xA003, bits 7:0), in 150 ps steps. */
    val = 0xA003;
    reg.reg_addr = 0x1E;
    err = esp_eth_ioctl(s_handle, ETH_CMD_WRITE_PHY_REG, &reg);
    if (err != ESP_OK) {
        return err;
    }
    reg.reg_addr = 0x1F;
    err = esp_eth_ioctl(s_handle, ETH_CMD_READ_PHY_REG, &reg);
    if (err != ESP_OK) {
        return err;
    }
    val = (val & ~0x00FFU) | (13U << 4) | (13U << 0);
    return esp_eth_ioctl(s_handle, ETH_CMD_WRITE_PHY_REG, &reg);
}
#endif /* CONFIG_ESPIX_ETH_PHY_YT8531 */

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    if (id == ETHERNET_EVENT_CONNECTED) {
        eth_speed_t speed = ETH_SPEED_10M;
        eth_duplex_t duplex = ETH_DUPLEX_HALF;
        esp_eth_ioctl(s_handle, ETH_CMD_G_SPEED, &speed);
        esp_eth_ioctl(s_handle, ETH_CMD_G_DUPLEX_MODE, &duplex);
        espix_klog(ESPIX_KLOG_INFO, TAG, "eth0: link up, %u Mbps %s duplex",
                   (unsigned)(speed == ETH_SPEED_10M ? 10
                              : speed == ETH_SPEED_100M ? 100 : 1000),
                   duplex == ETH_DUPLEX_FULL ? "full" : "half");
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "eth0: link down");
        choose_default_route();
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        char ip[ESPIX_IP4STR_MAX], gw[ESPIX_IP4STR_MAX];

        choose_default_route();

        espix_klog(ESPIX_KLOG_INFO, TAG, "eth0: %s/%d via %s",
                   espix_net_ip4str(ev->ip_info.ip.addr, ip, sizeof(ip)),
                   espix_net_prefix_len(ev->ip_info.netmask.addr),
                   espix_net_ip4str(ev->ip_info.gw.addr, gw, sizeof(gw)));

        uint32_t dns[2];
        const size_t n = espix_net_dns(dns, 2);
        for (size_t i = 0; i < n; i++) {
            char d[ESPIX_IP4STR_MAX];
            espix_klog(ESPIX_KLOG_INFO, TAG, "eth0: nameserver %s",
                       espix_net_ip4str(dns[i], d, sizeof(d)));
        }
    } else if (id == IP_EVENT_ETH_LOST_IP) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "eth0: lost address");
        choose_default_route();
    } else if (id == IP_EVENT_STA_GOT_IP) {
        /* Re-assert Ethernet if it also has an address: the WiFi bring-up sets
         * itself as the default, and that must not demote a live cable. */
        if (netif_has_addr(s_netif)) {
            esp_netif_set_default_netif(s_netif);
        }
    }
}

esp_err_t espix_net_eth_start(void)
{
    if (s_handle != NULL) {
        return ESP_OK;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr       = CONFIG_ESPIX_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_ESPIX_ETH_PHY_RST_GPIO;

    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac.smi_gpio.mdc_num  = CONFIG_ESPIX_ETH_MDC_GPIO;
    emac.smi_gpio.mdio_num = CONFIG_ESPIX_ETH_MDIO_GPIO;
    emac.interface         = EMAC_DATA_INTERFACE_RGMII;

    emac.clock_config.rgmii.clock_tx_gpio      = CONFIG_ESPIX_ETH_RGMII_TX_CLK_GPIO;
    emac.clock_config.rgmii.clock_rx_gpio      = CONFIG_ESPIX_ETH_RGMII_RX_CLK_GPIO;
    emac.clock_config.rgmii.clock_phy_ref_gpio = CONFIG_ESPIX_ETH_RGMII_PHY_REF_CLK_GPIO;

    emac.emac_dataif_gpio.rgmii.tx_ctl_num = CONFIG_ESPIX_ETH_RGMII_TX_CTL_GPIO;
    emac.emac_dataif_gpio.rgmii.txd0_num   = CONFIG_ESPIX_ETH_RGMII_TXD0_GPIO;
    emac.emac_dataif_gpio.rgmii.txd1_num   = CONFIG_ESPIX_ETH_RGMII_TXD1_GPIO;
    emac.emac_dataif_gpio.rgmii.txd2_num   = CONFIG_ESPIX_ETH_RGMII_TXD2_GPIO;
    emac.emac_dataif_gpio.rgmii.txd3_num   = CONFIG_ESPIX_ETH_RGMII_TXD3_GPIO;
    emac.emac_dataif_gpio.rgmii.rx_ctl_num = CONFIG_ESPIX_ETH_RGMII_RX_CTL_GPIO;
    emac.emac_dataif_gpio.rgmii.rxd0_num   = CONFIG_ESPIX_ETH_RGMII_RXD0_GPIO;
    emac.emac_dataif_gpio.rgmii.rxd1_num   = CONFIG_ESPIX_ETH_RGMII_RXD1_GPIO;
    emac.emac_dataif_gpio.rgmii.rxd2_num   = CONFIG_ESPIX_ETH_RGMII_RXD2_GPIO;
    emac.emac_dataif_gpio.rgmii.rxd3_num   = CONFIG_ESPIX_ETH_RGMII_RXD3_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac, &mac_config);
    if (mac == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot create the MAC");
        return ESP_FAIL;
    }

    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (phy == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot create the PHY");
        mac->del(mac);
        return ESP_FAIL;
    }

    const esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&config, &s_handle);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "driver install: %s", esp_err_to_name(err));
        mac->del(mac);
        phy->del(phy);
        return err;
    }

#if CONFIG_ESPIX_ETH_PHY_YT8531
    err = yt8531_fixups();
    if (err != ESP_OK) {
        /* The link may still come up; say so and carry on rather than losing
         * the interface over a register write. */
        espix_klog(ESPIX_KLOG_WARN, TAG, "YT8531 fix-ups failed: %s; link may not work",
                   esp_err_to_name(err));
    }
#endif

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_inherent_config_t port_cfg;
    if (espix_net_bridge_wants("eth0")) {
        /*
         * A bridge port carries frames and has no address of its own -- the
         * bridge does. IDF's bridge example asks for flags=0 and no IP, and
         * it is a creation-time choice, which is why membership needs a
         * reboot. The cable and this netif never go away.
         */
        port_cfg               = (esp_netif_inherent_config_t)ESP_NETIF_INHERENT_DEFAULT_ETH();
        port_cfg.flags         = 0;
        port_cfg.ip_info       = NULL;
        port_cfg.get_ip_event  = 0;
        port_cfg.lost_ip_event = 0;
        port_cfg.route_prio    = 0;
        netif_cfg.base         = &port_cfg;
    }
    s_netif = esp_netif_new(&netif_cfg);
    if (s_netif == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot create the netif");
        return ESP_FAIL;
    }

    s_glue = esp_eth_new_netif_glue(s_handle);
    err = esp_netif_attach(s_netif, s_glue);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "netif attach: %s", esp_err_to_name(err));
        return err;
    }

    espix_net_register_if("eth0", ESPIX_IF_ETH, s_netif);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL, NULL));

    err = esp_eth_start(s_handle);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "start: %s", esp_err_to_name(err));
        return err;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "eth0: started (RGMII, PHY at %s)",
               CONFIG_ESPIX_ETH_PHY_ADDR < 0 ? "auto" : "a fixed address");
    return ESP_OK;
}
