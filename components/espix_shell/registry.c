/*
 * espix command registry.
 *
 * A singly-linked list of caller-owned static structs, kept sorted by name so
 * `help` is alphabetical for free. Registration happens once at boot under a
 * mutex; lookup afterwards is lock-free, which is what lets several sessions
 * dispatch concurrently.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "espix_shell.h"

static espix_cmd_t       *s_head;
static SemaphoreHandle_t  s_lock;

static void registry_lazy_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

esp_err_t espix_shell_register(espix_cmd_t *cmd)
{
    if (cmd == NULL || cmd->name == NULL || cmd->fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    registry_lazy_init();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t err = ESP_OK;
    espix_cmd_t **link = &s_head;

    while (*link != NULL) {
        const int cmp = strcmp((*link)->name, cmd->name);
        if (cmp == 0) {
            err = ESP_ERR_INVALID_STATE;    /* already registered */
            goto out;
        }
        if (cmp > 0) {
            break;                          /* insertion point */
        }
        link = &(*link)->next;
    }

    cmd->next = *link;
    *link = cmd;

out:
    xSemaphoreGive(s_lock);
    return err;
}

const espix_cmd_t *espix_shell_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }

    for (const espix_cmd_t *c = s_head; c != NULL; c = c->next) {
        const int cmp = strcmp(c->name, name);
        if (cmp == 0) {
            return c;
        }
        if (cmp > 0) {
            break;                          /* sorted: cannot appear later */
        }
    }
    return NULL;
}

void espix_shell_foreach(espix_cmd_iter_fn cb, void *ctx)
{
    if (cb == NULL) {
        return;
    }
    for (const espix_cmd_t *c = s_head; c != NULL; c = c->next) {
        if (!cb(ctx, c)) {
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Line-editor callbacks                                               */
/* ------------------------------------------------------------------ */

/*
 * Completion and hints are pure registry lookups, so they live here rather than
 * in either transport — the console and an SSH session pass the same two
 * function pointers to their own editor instances, which is what makes TAB and
 * the usage hint behave identically over both.
 */

typedef struct {
    const char                   *prefix;
    size_t                        prefix_len;
    void                         *cb_ctx;
    esp_linenoise_completion_cb_t cb;
} completion_ctx_t;

static bool completion_visit(void *ctx, const espix_cmd_t *cmd)
{
    completion_ctx_t *c = ctx;

    if (strncmp(cmd->name, c->prefix, c->prefix_len) == 0) {
        c->cb(c->cb_ctx, cmd->name);
    }
    return true;
}

void espix_shell_completion(const char *buf, void *cb_ctx,
                            esp_linenoise_completion_cb_t cb)
{
    /* Only the command word completes; argument completion needs per-command
     * knowledge and can come later. */
    if (buf == NULL || strchr(buf, ' ') != NULL) {
        return;
    }

    completion_ctx_t ctx = {
        .prefix     = buf,
        .prefix_len = strlen(buf),
        .cb_ctx     = cb_ctx,
        .cb         = cb,
    };
    espix_shell_foreach(completion_visit, &ctx);
}

char *espix_shell_hint(const char *buf, int *color, int *bold)
{
    if (buf == NULL || buf[0] == '\0' || strchr(buf, ' ') != NULL) {
        return NULL;
    }

    const espix_cmd_t *cmd = espix_shell_find(buf);
    if (cmd == NULL || cmd->usage == NULL) {
        return NULL;
    }

    /* Printed verbatim and, with no free-hints callback registered, not owned
     * by the editor — so a static buffer is safe here. */
    static char hint[ESPIX_LINE_MAX];
    snprintf(hint, sizeof(hint), " %s", cmd->usage);

    *color = 33;    /* dim yellow */
    *bold  = 0;
    return hint;
}
