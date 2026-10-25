/*
 * Thread (IEEE 802.15.4 / OpenThread) as espix presents it.
 *
 * One SoC, one radio: the stack runs in this firmware, not in a co-processor
 * and not in a loadable app. It is started by espix_net_init() like WiFi, and
 * the interface it creates is registered beside wlan0 and eth0 as "thread0",
 * so ip/ifconfig/route already know how to show it.
 *
 * The control surface is the \`thread\` command (cmd_thread.c). OpenThread's own
 * CLI is available through espix_thread_cli(): IDF has already initialised it
 * in esp_openthread_start(), and espix_thread_cli() posts a line to the OT task
 * queue. Its replies go to the OT task's stdout, i.e. the console; a session
 * gets the answer only for its own commands, which is why the command's own
 * verbs do not depend on the CLI.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPIX_THREAD_IF_NAME "thread0"

/* Four was not enough: the link-local is the address that proves the interface
 * has one at all, and it was the one being dropped. */
#define ESPIX_THREAD_ADDR_MAX 6

typedef struct {
    bool     started;        /* the stack was started */
    bool     if_up;          /* thread0 is administratively up */
    bool     ip6_enabled;    /* otIp6SetEnabled(true) has been called */
    bool     attached;       /* child, router or leader */
    char     role[24];       /* "disabled", "detached", "child", ... */
    uint16_t rloc16;
    size_t   n_addrs;        /* how many of addrs[] are filled */
    char     addrs[ESPIX_THREAD_ADDR_MAX][40];
    bool     border_router;  /* border routing initialised this boot */
    bool     nat64;          /* the NAT64 prefix manager is publishing */
} espix_thread_status_t;

/* Called by espix_net_init(): starts the stack only if /etc/thread.conf says
 * enabled=yes. Never fatal to boot. */
esp_err_t espix_thread_init(void);

esp_err_t espix_thread_start(void);
esp_err_t espix_thread_stop(void);
bool      espix_thread_is_started(void);

/* Join/attach using whatever dataset is stored (must /etc/thread.conf carry one
 * or have been commissioned already). Not the same as start: the stack can be
 * up and detached, exactly as Linux's wpan0 can be up with no Thread network. */
esp_err_t espix_thread_attach(void);

/* Border routing, when CONFIG_ESPIX_THREAD_BORDER_ROUTER is on. The backbone is
 * named in /etc/thread.conf (backbone=eth0) and must exist before start. */
esp_err_t espix_thread_border_router(bool on);

/* Where a CLI reply goes while the line runs. Called once per output chunk;
 * the text is not NUL-terminated. */
typedef void (*espix_thread_write_fn)(void *ctx, const char *text, size_t len);

/* One line of OpenThread CLI, e.g. "dataset active". Runs under the OpenThread
 * lock and answers through write(), so the session that asked sees it; commands
 * that finish later still answer on the console. */
esp_err_t espix_thread_cli(const char *line, espix_thread_write_fn write,
                           void *ctx);

/* The active dataset as hex TLVs -- the form 'thread join' takes -- and the
 * reverse. The dataset is OpenThread's, in OpenThread's NVS, not in /etc. */
esp_err_t espix_thread_dataset_hex(char *out, size_t out_len);
esp_err_t espix_thread_join(const char *hex);

esp_err_t espix_thread_status(espix_thread_status_t *out);

/* Rewrite /etc/thread.conf with the four policy keys (enabled, border_router,
 * backbone, attach). Runs as the caller, so writing it needs root; the kernel
 * creates the file the first time, which does not. */
esp_err_t espix_thread_conf_write(bool enabled, bool border_router,
                                  const char *backbone, bool attach);

#ifdef __cplusplus
}
#endif
