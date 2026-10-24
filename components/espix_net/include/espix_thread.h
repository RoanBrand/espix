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

typedef struct {
    bool     started;        /* the stack was started */
    bool     if_up;          /* thread0 is administratively up */
    bool     ip6_enabled;    /* otIp6SetEnabled(true) has been called */
    bool     attached;       /* child, router or leader */
    char     role[24];       /* "disabled", "detached", "child", ... */
    uint16_t rloc16;
    size_t   n_addrs;        /* how many of addrs[] are filled */
    char     addrs[4][40];   /* formatted IPv6 addresses */
    bool     border_router;  /* border routing initialised this boot */
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

/* One line of OpenThread CLI, e.g. "dataset active". Replies go to the console. */
esp_err_t espix_thread_cli(const char *line);

esp_err_t espix_thread_status(espix_thread_status_t *out);

#ifdef __cplusplus
}
#endif
