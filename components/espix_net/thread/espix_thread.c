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
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_cli.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"        /* inet_pton/inet_ntop, AF_INET6 */
#include "openthread/cli.h"
#include "openthread/dataset.h"
#include "openthread/instance.h"
#include "openthread/nat64.h"
#include "openthread/ip6.h"
#include "openthread/thread.h"

#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
#include "esp_coexist.h"
#endif

#if CONFIG_ESPIX_THREAD_BORDER_ROUTER
#include <stddef.h>
#include "lwip/netif.h"
#endif

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_net.h"
#include "espix_net_priv.h"
#include "espix_thread.h"

#define TAG             "thread"
#define THREAD_CONF     "/etc/thread.conf"
#define THREAD_BACKBONE "eth0"

#if CONFIG_ESPIX_THREAD_BORDER_ROUTER
/*
 * libopenthread_br.a is prebuilt, and touches struct netif at the offsets its
 * own esp_openthread_check_netif_*() expect. Anything else corrupts every netif
 * it reads or allocates; tools/patch-lwip.py is what makes espix's layout match.
 */
#define BR_NETIF_LAYOUT "struct netif does not match libopenthread_br.a (tools/patch-lwip.py)"
_Static_assert(offsetof(struct netif, state) == 492, BR_NETIF_LAYOUT);
_Static_assert(offsetof(struct netif, hostname) == 512, BR_NETIF_LAYOUT);
_Static_assert(offsetof(struct netif, mtu) == 516, BR_NETIF_LAYOUT);
_Static_assert(offsetof(struct netif, hwaddr) == 520, BR_NETIF_LAYOUT);
_Static_assert(offsetof(struct netif, hwaddr_len) == 526, BR_NETIF_LAYOUT);
_Static_assert(offsetof(struct netif, flags) == 527, BR_NETIF_LAYOUT);
_Static_assert(offsetof(struct netif, name) == 528, BR_NETIF_LAYOUT);
_Static_assert(offsetof(struct netif, num) == 530, BR_NETIF_LAYOUT);
_Static_assert(sizeof(struct netif) <= 560, BR_NETIF_LAYOUT);
#endif

static bool s_started;
static bool s_border_router;

/*
 * start, stop, attach, join and border routing change the stack's state, and any
 * session can ask for any of them at once. One recursive lock, because stop
 * turns border routing off on its way down.
 */
static StaticSemaphore_t s_ctl_buf;
static SemaphoreHandle_t s_ctl;

static void ctl_create(void)
{
    if (s_ctl == NULL) {
        s_ctl = xSemaphoreCreateRecursiveMutexStatic(&s_ctl_buf);
    }
}

static void ctl_lock(void)
{
    ctl_create();
    xSemaphoreTakeRecursive(s_ctl, portMAX_DELAY);
}

static void ctl_unlock(void)
{
    xSemaphoreGiveRecursive(s_ctl);
}

/* OpenThread spells every group out ("fe80:0:0:0:9ca1:..."); lwIP and Linux
 * compress and print lower case. Re-render so thread0 reads like the rest. */
static void ip6_tidy(char *text, size_t len)
{
    struct in6_addr a6;
    char            out[INET6_ADDRSTRLEN];

    if (inet_pton(AF_INET6, text, &a6) != 1 ||
        inet_ntop(AF_INET6, &a6, out, sizeof(out)) == NULL) {
        return;
    }
    for (char *c = out; *c != '\0'; c++) {
        if (*c >= 'A' && *c <= 'F') {
            *c = (char)(*c - 'A' + 'a');
        }
    }
    strlcpy(text, out, len);
}

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

static int cli_output(void *context, const char *format, va_list args);

esp_err_t espix_thread_start(void)
{
    ctl_lock();
    if (s_started) {
        ctl_unlock();
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
        ctl_unlock();
        return err;
    }
    s_started = true;

    /*
     * Point the OpenThread CLI's output at espix's sessions rather than the
     * console: espix_thread_cli() collects it. IDF installs its own callback
     * before esp_openthread_start() returns, so this replaces it; under the
     * lock, because the OT task is already running.
     */
    esp_openthread_lock_acquire(portMAX_DELAY);
    otCliInit(esp_openthread_get_instance(), cli_output, NULL);
    esp_openthread_lock_release();

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
    ctl_unlock();
    return ESP_OK;
}

esp_err_t espix_thread_stop(void)
{
    ctl_lock();
    if (!s_started) {
        ctl_unlock();
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

    /*
     * Out of the interface table first: the OT task destroys this netif on its
     * way out, and ip or ifconfig reading thread0 after that would read freed
     * memory. If the stack refuses to stop, it goes back in.
     */
    esp_netif_t *const netif = esp_openthread_get_netif();
    espix_net_unregister_if(ESPIX_THREAD_IF_NAME);

    const esp_err_t err = esp_openthread_stop();
    if (err == ESP_OK) {
        s_started = false;
        espix_klog(ESPIX_KLOG_INFO, TAG, "OpenThread down");
    } else if (netif != NULL) {
        (void)espix_net_register_if(ESPIX_THREAD_IF_NAME, ESPIX_IF_THREAD, netif);
    }
    ctl_unlock();
    return err;
}

esp_err_t espix_thread_attach(void)
{
    ctl_lock();
    if (!s_started) {
        ctl_unlock();
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
    ctl_unlock();
    return err;
}

static esp_err_t border_router(bool on);

esp_err_t espix_thread_border_router(bool on)
{
    ctl_lock();
    const esp_err_t err = border_router(on);
    ctl_unlock();
    return err;
}

static esp_err_t border_router(bool on)
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

    otInstance *instance = esp_openthread_get_instance();

    if (!on) {
        if (!s_border_router) {
            esp_openthread_lock_release();
            return ESP_OK;
        }
        /*
         * NAT64 first: it publishes a route into the Thread network data, and
         * that route has to go before the routing manager that owns it does.
         */
        if (instance != NULL) {
            otNat64SetEnabled(instance, false);
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
    if (instance == NULL || !otIp6IsEnabled(instance)) {
        esp_openthread_lock_release();
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = esp_openthread_border_router_init();
    if (err == ESP_OK) {
        s_border_router = true;

        /*
         * NAT64 is what lets the Thread side reach the IPv4 internet, and it is
         * off until told otherwise. With the prefix manager disabled,
         * Evaluate() returns immediately, and no NAT64 route appears in the
         * Thread network data for clients to synthesize addresses in. Measured
         * on the S31: without this call the BR reports a local prefix of its
         * own (br nat64prefix) but publishes no route, and 'nat64 state' says
         * Disabled; with it, the route appears in 'netdata show'.
         */
        otNat64SetEnabled(instance, true);
    }
    esp_openthread_lock_release();
    return err;
#else
    (void)on;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/*
 * The CLI's reply to one line, collected for the session that sent it.
 *
 * cli_output() runs on the OT task and espix_thread_cli() on the caller's, so
 * every field below is touched under s_cli_mux. s_cli_busy is the line in
 * flight: set when one is posted, cleared only by the prompt that ends its reply.
 * A caller that gave up waiting leaves it set, so the late remainder of that
 * reply cannot land in the next caller's buffer and its prompt cannot wake the
 * next caller early; the next line is refused until the prompt comes, or until
 * CLI_STALE_MS says the CLI is never going to print one.
 */
#define CLI_WAIT_MS  10000
#define CLI_STALE_MS 30000

static portMUX_TYPE  s_cli_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t  s_cli_wait;        /* collecting, or NULL: to the console */
static bool          s_cli_busy;
static TickType_t    s_cli_posted;
EXT_RAM_BSS_ATTR static char s_cli_buf[1024];     /* never touched from an ISR */
static size_t        s_cli_len;
static bool          s_cli_truncated;

/*
 * The CLI's output, wherever it runs. With a caller collecting it goes into
 * s_cli_buf, and the prompt -- which ProcessLine() prints last, and which IDF's
 * own console path waits for too -- wakes that caller. With nobody collecting it
 * goes to the console, which is where unsolicited output (a late scan result)
 * belongs anyway.
 */
static int cli_output(void *context, const char *format, va_list args)
{
    (void)context;

    char chunk[260];
    const int n = vsnprintf(chunk, sizeof(chunk), format, args);
    if (n <= 0) {
        return 0;
    }
    const size_t len    = ((size_t)n < sizeof(chunk)) ? (size_t)n : sizeof(chunk) - 1;
    const bool   prompt = (chunk[0] == '>' && chunk[1] == ' ');

    taskENTER_CRITICAL(&s_cli_mux);
    TaskHandle_t const waiter = s_cli_wait;
    if (prompt) {
        s_cli_busy = false;
    } else if (waiter != NULL) {
        if (s_cli_len + len <= sizeof(s_cli_buf)) {
            memcpy(s_cli_buf + s_cli_len, chunk, len);
            s_cli_len += len;
        } else {
            s_cli_truncated = true;
        }
    }
    taskEXIT_CRITICAL(&s_cli_mux);

    if (prompt) {
        if (waiter != NULL) {
            xTaskNotifyGive(waiter);
        }
    } else if (waiter == NULL) {
        fwrite(chunk, 1, len, stdout);
    }
    return 0;
}

/*
 * Session-correct CLI output.
 *
 * esp_openthread_cli_input() posts the line to the OpenThread task, and the CLI
 * answers there through the callback installed at start -- the two together are
 * what a command typed into an SSH session needs to see its own reply. Running
 * otCliInputLine() here instead would capture the output more directly but
 * deadlock on any command that waits for the stack: the OT task's mainloop
 * takes the same lock.
 *
 * The wait is bounded: a command that never answers (a scan with no network)
 * cannot hold its session open forever. Whatever it had printed is still
 * delivered, and what it prints afterwards goes to the console.
 */
esp_err_t espix_thread_cli(const char *line, espix_thread_write_fn write,
                           void *ctx)
{
    if (!s_started || line == NULL || write == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const TickType_t now = xTaskGetTickCount();
    taskENTER_CRITICAL(&s_cli_mux);
    const bool busy = s_cli_wait != NULL ||
                      (s_cli_busy && now - s_cli_posted < pdMS_TO_TICKS(CLI_STALE_MS));
    if (!busy) {
        s_cli_busy      = true;
        s_cli_posted    = now;
        s_cli_wait      = xTaskGetCurrentTaskHandle();
        s_cli_len       = 0;
        s_cli_truncated = false;
    }
    taskEXIT_CRITICAL(&s_cli_mux);
    if (busy) {
        return ESP_ERR_INVALID_STATE;       /* one line at a time */
    }

    /* A notification left over from anything earlier must not end this wait. */
    (void)xTaskNotifyStateClear(NULL);
    (void)ulTaskNotifyValueClear(NULL, UINT32_MAX);

    esp_err_t err = esp_openthread_cli_input(line);
    const bool posted = (err == ESP_OK);
    if (posted &&
        xTaskNotifyWait(0, UINT32_MAX, NULL, pdMS_TO_TICKS(CLI_WAIT_MS)) != pdTRUE) {
        err = ESP_ERR_TIMEOUT;
    }

    taskENTER_CRITICAL(&s_cli_mux);
    s_cli_wait = NULL;
    if (!posted) {
        s_cli_busy = false;
    }
    const size_t len       = s_cli_len;
    const bool   truncated = s_cli_truncated;
    taskEXIT_CRITICAL(&s_cli_mux);

    /* Nobody appends once s_cli_wait is clear, so the buffer is ours to read. */
    if (len > 0) {
        write(ctx, s_cli_buf, len);
    }
    if (truncated) {
        static const char k_more[] = "[... output truncated]\n";
        write(ctx, k_more, sizeof(k_more) - 1);
    }
    return err;
}

/*
 * The active dataset, as hex TLVs -- the same string 'thread join' takes, and
 * what OpenThread itself prints with 'dataset active -x'. It lives in
 * OpenThread's NVS; this only reads it out.
 */
esp_err_t espix_thread_dataset_hex(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ctl_lock();
    otInstance *instance = s_started ? esp_openthread_get_instance() : NULL;
    if (instance == NULL) {
        ctl_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    otOperationalDatasetTlvs tlvs;
    esp_openthread_lock_acquire(portMAX_DELAY);
    const otError e = otDatasetGetActiveTlvs(instance, &tlvs);
    esp_openthread_lock_release();
    ctl_unlock();

    if (e != OT_ERROR_NONE || tlvs.mLength == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (out_len < (size_t)tlvs.mLength * 2 + 1) {
        return ESP_ERR_INVALID_SIZE;
    }

    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < tlvs.mLength; i++) {
        out[i * 2]     = digits[tlvs.mTlvs[i] >> 4];
        out[i * 2 + 1] = digits[tlvs.mTlvs[i] & 0x0f];
    }
    out[tlvs.mLength * 2] = '\0';
    return ESP_OK;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

esp_err_t espix_thread_join(const char *hex)
{
    if (hex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    otInstance *instance = esp_openthread_get_instance();
    if (instance == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    otOperationalDatasetTlvs tlvs;
    memset(&tlvs, 0, sizeof(tlvs));

    size_t bytes = 0;
    for (size_t i = 0; hex[i] != '\0'; ) {
        if (hex[i] == ' ' || hex[i] == '\t' || hex[i] == '\n') {
            i++;
            continue;
        }
        const int hi = hex_nibble(hex[i]);
        const int lo = (hex[i + 1] != '\0') ? hex_nibble(hex[i + 1]) : -1;
        if (hi < 0 || lo < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        if (bytes >= sizeof(tlvs.mTlvs)) {
            return ESP_ERR_INVALID_SIZE;
        }
        tlvs.mTlvs[bytes++] = (uint8_t)((hi << 4) | lo);
        i += 2;
    }
    if (bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    tlvs.mLength = (uint8_t)bytes;

    /* auto_start() sets it active and brings the interface up; the same path
     * 'thread attach' takes, but with a dataset from elsewhere. */
    ctl_lock();
    if (!s_started) {
        ctl_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    const esp_err_t err = esp_openthread_auto_start(&tlvs);
    esp_openthread_lock_release();
    ctl_unlock();
    return err;
}

esp_err_t espix_thread_status(espix_thread_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    /*
     * Bounded, not forever: ip and ifconfig ask this while holding the interface
     * table, and stop holds this lock while it takes that table to unregister
     * thread0. Waiting a moment and giving up breaks the cycle; it also keeps a
     * status read off a stack that is half torn down.
     */
    ctl_create();
    if (xSemaphoreTakeRecursive(s_ctl, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    out->started       = s_started;
    out->border_router = s_border_router;

    if (!s_started) {
        strlcpy(out->role, "not started", sizeof(out->role));
        ctl_unlock();
        return ESP_OK;
    }

    otInstance *instance = esp_openthread_get_instance();
    if (instance == NULL) {
        ctl_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);

    const otDeviceRole role = otThreadGetDeviceRole(instance);
    out->ip6_enabled = otIp6IsEnabled(instance);
#if CONFIG_ESPIX_THREAD_BORDER_ROUTER
    out->nat64 = (otNat64GetPrefixManagerState(instance) == OT_NAT64_STATE_ACTIVE);
#endif
    out->rloc16      = otThreadGetRloc16(instance);
    for (const otNetifAddress *a = otIp6GetUnicastAddresses(instance);
         a != NULL && out->n_addrs < ESPIX_THREAD_ADDR_MAX; a = a->mNext) {
        otIp6AddressToString(&a->mAddress, out->addrs[out->n_addrs],
                             sizeof(out->addrs[0]));
        ip6_tidy(out->addrs[out->n_addrs], sizeof(out->addrs[0]));
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
    ctl_unlock();
    return ESP_OK;
}

/*
 * Write /etc/thread.conf with the caller's authority: the boot task is espix
 * itself, and a verb run by a user needs what any other /etc write needs, root.
 * A rewrite keeps the file's mode; only the creating call sets it.
 */
static esp_err_t conf_write(bool creating, bool enabled, bool border_router,
                            const char *backbone, bool attach)
{
    FILE *f = fopen(THREAD_CONF, "w");

    if (f == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot write %s: %s",
                   THREAD_CONF, strerror(errno));
        return ESP_FAIL;
    }

    fprintf(f, "# espix Thread configuration\n");
    fprintf(f, "#\n");
    fprintf(f, "# enabled=yes starts the OpenThread stack at boot. 'thread start'\n");
    fprintf(f, "# starts it by hand instead, and 'thread status' says what it is doing.\n");
    fprintf(f, "enabled=%s\n\n", enabled ? "yes" : "no");
    fprintf(f, "#\n");
    fprintf(f, "# border_router=yes enables border routing: the mesh reaches the backbone\n");
    fprintf(f, "# through NAT64. The backbone has to exist when Thread starts, while\n");
    fprintf(f, "# OpenThread is initialised.\n");
    fprintf(f, "border_router=%s\n\n", border_router ? "yes" : "no");
    fprintf(f, "#\n");
    fprintf(f, "# backbone=<interface> is what the border router routes to. eth0 avoids\n");
    fprintf(f, "# sharing the 2.4 GHz radio with WiFi; wlan0 is possible and coexists.\n");
    fprintf(f, "backbone=%s\n\n",
            (backbone != NULL && backbone[0] != '\0') ? backbone : THREAD_BACKBONE);
    fprintf(f, "#\n");
    fprintf(f, "# attach=yes joins the stored dataset at boot. The dataset itself is in\n");
    fprintf(f, "# OpenThread's own NVS storage, not here: this file is espix policy, not\n");
    fprintf(f, "# protocol state.\n");
    fprintf(f, "attach=%s\n", attach ? "yes" : "no");
    fclose(f);

    if (creating) {
        const esp_err_t err = espix_fs_chmod(THREAD_CONF, 0644);
        if (err != ESP_OK) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "cannot set the mode on %s: %s",
                       THREAD_CONF, esp_err_to_name(err));
        }
    }
    return ESP_OK;
}

esp_err_t espix_thread_conf_write(bool enabled, bool border_router,
                                  const char *backbone, bool attach)
{
    return conf_write(false, enabled, border_router, backbone, attach);
}

/*
 * The file has to exist before anyone can be told to edit it, and /etc is not
 * shipped -- the repo's fsroot is the dev overlay -- so the kernel creates it
 * at boot, from a task the permission check does not apply to. Absent means off, but an empty
 * file would be worse than none: this is also the only place the keys are
 * written down.
 */
static void ensure_conf(void)
{
    FILE *probe = fopen(THREAD_CONF, "r");
    const bool present = (probe != NULL);
    /*
     * A zero-byte file is what a failed redirect leaves behind -- a 2>/dev/null
     * style redirect creates before the command it prefixes runs -- and it is
     * worse than no file at all, so it gets the default too. A config of only
     * comments is left alone: that is somebody choosing the defaults.
     */
    const bool empty = present && (fgetc(probe) == EOF);
    if (present) {
        fclose(probe);
    }

    if (present && !empty) {
        return;
    }

    if (conf_write(true, false, false, THREAD_BACKBONE, false) == ESP_OK) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "created %s", THREAD_CONF);
    } else {
        espix_klog(ESPIX_KLOG_WARN, TAG, "cannot create %s", THREAD_CONF);
    }
}

esp_err_t espix_thread_init(void)
{
    char v[8];

    ctl_create();
    ensure_conf();

    char want_attach[8] = {0};
    char want_border[8] = {0};
    (void)espix_fs_conf_get(THREAD_CONF, "attach", want_attach,
                            sizeof(want_attach));
    (void)espix_fs_conf_get(THREAD_CONF, "border_router", want_border,
                            sizeof(want_border));

    if (!(espix_fs_conf_get(THREAD_CONF, "enabled", v, sizeof(v)) &&
          strcmp(v, "yes") == 0)) {
        return ESP_OK;      /* not configured: the command can still start it */
    }

    const esp_err_t err = espix_thread_start();
    if (err != ESP_OK) {
        return err;
    }

    /*
     * Attach before border routing. Border routing initialises against the
     * Thread interface, and that interface is not up until the stack has joined
     * something; the other order is refused (and once asserted).
     */
    const bool using_attach = (strcmp(want_attach, "yes") == 0);
    if (using_attach) {
        const esp_err_t at = espix_thread_attach();
        if (at != ESP_OK) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "attach: %s", esp_err_to_name(at));
        }
    }

    if (strcmp(want_border, "yes") == 0) {
        if (!using_attach) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "border_router=yes needs attach=yes; set both, or run "
                       "'thread attach' then 'thread br on'");
        } else {
            const esp_err_t br = espix_thread_border_router(true);
            if (br != ESP_OK) {
                espix_klog(ESPIX_KLOG_WARN, TAG, "border router: %s",
                           esp_err_to_name(br));
            }
        }
    }
    return ESP_OK;
}
