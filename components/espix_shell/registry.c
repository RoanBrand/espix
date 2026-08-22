/*
 * espix command registry.
 *
 * A singly-linked list of caller-owned static structs, kept sorted by name so
 * `help` is alphabetical for free. Registration happens once at boot under a
 * mutex; lookup afterwards is lock-free, which is what lets several sessions
 * dispatch concurrently.
 */

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
