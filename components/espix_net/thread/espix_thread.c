/*
 * Thread (IEEE 802.15.4 / OpenThread) over the S31's own radio.
 *
 * The shape is deliberately un-Linux: there is no radio co-processor, no
 * Spinel stream and no second host. One SoC has the 802.15.4 radio and the
 * application core, so OpenThread runs here in-process:
 *
 *     radio_mode        = RADIO_MODE_NATIVE
 *     host_connection   = HOST_CONNECTION_MODE_NONE
 *
 * esp_openthread_start() spawns the OT task, initialises the stack, attaches
 * the esp_netif glue and (CONFIG_OPENTHREAD_CLI) its own CLI, then returns;
 * the main loop runs on the OT task. That is why there is nothing to poll here.
 *
 * Reuse is the point: the radio driver, the FTD stack, the netif glue, the
 * border router and the CLI all come from IDF's openthread component. What
 * espix adds is a Linux name for the interface and a command to drive it.
 */
#include <string.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_cli.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/thread.h"

#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
#include "esp_coexist.h"
#endif

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_net.h"
#include "espix_net_priv.h"
#include "espix_thread.h"

#define TAG             "thread"
#define THREAD_CONF     "/etc/thread.conf"
#define THREAD_BACKBONE "eth0"

static bool s_started;
static bool s_border_router;

/*
 * Native radio, no host: the one shape that makes sense when the radio and the
 * stack share a die. storage_partition_name is the dataset's home; see the note
 * in cmd_thread.c about keeping that in /etc instead.
 */
static const esp_openthread_config_t s_config = {
    .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
    .platform_config = {
        .radio_config = {
            .radio_mode = RADIO_MODE_NATIVE,
        },
        .host_config = {
            .host_connection_mode = HOST_CONNECTION_MODE_NONE,
        },
        .port_config = {
            .storage_partition_name = "nvs",
            .netif_queue_size       = 10,
            .task_queue_size        = 10,
        },
    },
};

bool espix_thread_is_started(void)
{
    return s_started;
}

esp_err_t espix_thread_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

#if CONFIG_ESPIX_THREAD_BORDER_ROUTER
    /*
     * The backbone must be named before esp_openthread_init() -- the BR code
     * reads it during init and cannot be told later. eth0 is the sane default on
     * an S31 (1000M, no self-interference with the 2.4 GHz radio); a WiFi
     * backbone is possible, but then the radio is shared with coexistence.
     */
    char bb[ESPIX_IF_NAME_MAX] = THREAD_BACKBONE;
    (void)espix_fs_conf_get(THREAD_CONF, "backbone", bb, sizeof(bb));

    const espix_if_entry_t *e = espix_net_find_if(bb);
    if (e != NULL && e->netif != NULL) {
        esp_openthread_set_backbone_netif(e->netif);
    } else {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "backbone %s is not up; border routing will be unavailable", bb);
    }
#endif

    const esp_err_t err = esp_openthread_start(&s_config);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "OpenThread start failed: %s",
                   esp_err_to_name(err));
        return err;
    }
    s_started = true;

    esp_netif_t *netif = esp_openthread_get_netif();
    if (netif != NULL) {
        (void)espix_net_register_if(ESPIX_THREAD_IF_NAME, ESPIX_IF_THREAD, netif);
    } else {
        espix_klog(ESPIX_KLOG_WARN, TAG, "OpenThread returned no netif");
    }

#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    /* 15.4 and WiFi share the 2.4 GHz front end; ask coexistence to arbitrate. */
    (void)esp_coex_wifi_i154_enable();
#endif

    espix_klog(ESPIX_KLOG_INFO, TAG, "OpenThread up as %s", ESPIX_THREAD_IF_NAME);
    return ESP_OK;
}

esp_err_t espix_thread_stop(void)
{
    if (!s_started) {
        return ESP_OK;
    }

    /* The IDF contract: leave Thread and the interface first, then the BR. */
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *instance = esp_openthread_get_instance();
    if (instance != NULL) {
        otThreadSetEnabled(instance, false);
        otIp6SetEnabled(instance, false);
    }
    esp_openthread_lock_release();

    if (s_border_router) {
        (void)espix_thread_border_router(false);
    }

    const esp_err_t err = esp_openthread_stop();
    if (err == ESP_OK) {
        s_started = false;
    }
    return err;
}

esp_err_t espix_thread_attach(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * The OpenThread lock. acquire() takes both the stack mutex and the
     * task-switching mutex, and a task that reaches OT without the second
     * asserts in esp_openthread_task_switching_lock_release(). It is recursive,
     * so a callee that locks too is fine.
     */
    esp_openthread_lock_acquire(portMAX_DELAY);
    const esp_err_t err = esp_openthread_auto_start(NULL);
    esp_openthread_lock_release();
    return err;
}

esp_err_t espix_thread_border_router(bool on)
{
#if CONFIG_ESPIX_THREAD_BORDER_ROUTER
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Both calls go under the OpenThread lock, as IDF's own border router
     * example does (ot_examples_br.c, ot_br_init): the API reaches OT from this
     * task, and a task that does so without the task-switching lock asserts in
     * esp_openthread_task_switching_lock_release() -- measured, as a panic in
     * the 'thread' task. The mutex is recursive, so the callee locking too is
     * fine.
     */
    esp_openthread_lock_acquire(portMAX_DELAY);

    if (!on) {
        if (!s_border_router) {
            esp_openthread_lock_release();
            return ESP_OK;
        }
        const esp_err_t err = esp_openthread_border_router_deinit();
        if (err == ESP_OK) {
            s_border_router = false;
        }
        esp_openthread_lock_release();
        return err;
    }

    if (s_border_router) {
        esp_openthread_lock_release();
        return ESP_OK;
    }

    /*
     * Border routing needs a live interface. Init before the Thread interface
     * is enabled is invalid and IDF asserts rather than returning, so answer
     * ESP_ERR_INVALID_STATE here instead of panicking; the command turns that
     * into "run 'thread attach' first".
     */
    otInstance *instance = esp_openthread_get_instance();
    if (instance == NULL || !otIp6IsEnabled(instance)) {
        esp_openthread_lock_release();
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = esp_openthread_border_router_init();
    if (err == ESP_OK) {
        s_border_router = true;
    }
    esp_openthread_lock_release();
    return err;
#else
    (void)on;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t espix_thread_cli(const char *line)
{
    if (!s_started || line == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_openthread_cli_input(line);
}

esp_err_t espix_thread_status(espix_thread_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->started       = s_started;
    out->border_router = s_border_router;

    if (!s_started) {
        strlcpy(out->role, "not started", sizeof(out->role));
        return ESP_OK;
    }

    otInstance *instance = esp_openthread_get_instance();
    if (instance == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);

    const otDeviceRole role = otThreadGetDeviceRole(instance);
    out->ip6_enabled = otIp6IsEnabled(instance);
    out->rloc16      = otThreadGetRloc16(instance);
    for (const otNetifAddress *a = otIp6GetUnicastAddresses(instance);
         a != NULL && out->n_addrs < 4; a = a->mNext) {
        otIp6AddressToString(&a->mAddress, out->addrs[out->n_addrs],
                             sizeof(out->addrs[0]));
        out->n_addrs++;
    }

    esp_openthread_lock_release();

    const char *name = "unknown";
    switch (role) {
    case OT_DEVICE_ROLE_DISABLED: name = "disabled"; break;
    case OT_DEVICE_ROLE_DETACHED: name = "detached"; break;
    case OT_DEVICE_ROLE_CHILD:    name = "child";    break;
    case OT_DEVICE_ROLE_ROUTER:   name = "router";   break;
    case OT_DEVICE_ROLE_LEADER:   name = "leader";   break;
    default: break;
    }
    strlcpy(out->role, name, sizeof(out->role));
    out->attached = (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER ||
                     role == OT_DEVICE_ROLE_LEADER);

    esp_netif_t *netif = esp_openthread_get_netif();
    out->if_up = (netif != NULL) && esp_netif_is_netif_up(netif);
    return ESP_OK;
}

esp_err_t espix_thread_init(void)
{
    char v[8];

    if (!(espix_fs_conf_get(THREAD_CONF, "enabled", v, sizeof(v)) &&
          strcmp(v, "yes") == 0)) {
        return ESP_OK;      /* not configured: the command can still start it */
    }

    const esp_err_t err = espix_thread_start();
    if (err != ESP_OK) {
        return err;
    }

    if (espix_fs_conf_get(THREAD_CONF, "border_router", v, sizeof(v)) &&
        strcmp(v, "yes") == 0) {
        const esp_err_t br = espix_thread_border_router(true);
        if (br != ESP_OK) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "border router: %s",
                       esp_err_to_name(br));
        }
    }

    if (espix_fs_conf_get(THREAD_CONF, "attach", v, sizeof(v)) &&
        strcmp(v, "yes") == 0) {
        const esp_err_t at = espix_thread_attach();
        if (at != ESP_OK) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "attach: %s", esp_err_to_name(at));
        }
    }
    return ESP_OK;
}
