/*
 * Command history, owned by the user rather than the session.
 *
 * Unix puts history on the user — ~/.bash_history follows you between logins —
 * and espix does the same, in memory. A session borrows the list belonging to
 * whoever is logged in, so reconnecting over SSH as the same user finds what
 * was typed last time. The console keys on the empty name: it has no login
 * step, so it is its own principal and keeps its own list. Nothing is freed at
 * logout; that is the point.
 *
 * Persisting to ~/.espix_history later is load-on-first-use and save-on-logout
 * behind this same lookup, with no change to how sessions consume it.
 *
 * espix keeps its own list and hands a rebuilt copy to the line editor, rather
 * than letting the editor accumulate one, because of how esp_linenoise walks
 * its array. Navigation indexes history[history_index] and counts upward from
 * zero, where upstream linenoise indexes history[len - 1 - index] and counts
 * back from the end. With entries appended oldest-first — which is what
 * happens when an application adds each line as it is accepted — "previous"
 * therefore walks towards *newer* entries and runs into the editor's own
 * placeholder after one step, and the oldest entry is overwritten by the
 * in-progress line.
 *
 * Laying the array out as [placeholder, newest, ..., oldest] makes that same
 * forward walk come out in the order a user expects. Since history_add() only
 * appends, inserting a new newest means rewriting the list — cheap, because it
 * happens once per accepted command, not per keystroke.
 */

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"

#include "freertos/semphr.h"

#include "espix_shell.h"

/*
 * Enough for the console plus a few logins. If a further user appears the
 * least recently claimed bucket is recycled — losing an old list is a far
 * better outcome than refusing the login.
 */
#define HISTORY_USERS 4

static struct {
    char            user[ESPIX_SESSION_USER_MAX];
    bool            claimed;
    uint32_t        seq;        /* claim order, for recycling the oldest */
    espix_history_t hist;
} s_users[HISTORY_USERS];

static uint32_t          s_seq;
static SemaphoreHandle_t s_lock;

static void history_lazy_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

espix_history_t *espix_history_for(const char *user)
{
    if (user == NULL) {
        user = "";
    }

    history_lazy_init();
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }

    int slot = -1;

    for (int i = 0; i < HISTORY_USERS; i++) {
        if (s_users[i].claimed && strcmp(s_users[i].user, user) == 0) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        for (int i = 0; i < HISTORY_USERS; i++) {
            if (!s_users[i].claimed) {
                slot = i;
                break;
            }
        }
    }

    if (slot < 0) {
        /* Table full: recycle the least recently claimed. */
        slot = 0;
        for (int i = 1; i < HISTORY_USERS; i++) {
            if (s_users[i].seq < s_users[slot].seq) {
                slot = i;
            }
        }
        espix_history_free(&s_users[slot].hist);
    }

    if (!s_users[slot].claimed || strcmp(s_users[slot].user, user) != 0) {
        strlcpy(s_users[slot].user, user, sizeof(s_users[slot].user));
        s_users[slot].claimed = true;
        s_users[slot].seq     = ++s_seq;
    }

    espix_history_t *h = &s_users[slot].hist;

    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    return h;
}

/*
 * Is this a line we should decline to remember?
 *
 * A list that outlives the session is also what a future ~/.espix_history gets
 * written from, and espix takes the new password as a command *argument*, so
 * `passwd` would otherwise leave a secret sitting one arrow-press away for the
 * next login. A leading space is bash's HISTCONTROL=ignorespace convention,
 * kept as a general escape hatch for anything else worth forgetting.
 */
static bool history_is_private(const char *line)
{
    if (line[0] == ' ') {
        return true;
    }
    return strncmp(line, "passwd", 6) == 0 &&
           (line[6] == '\0' || line[6] == ' ');
}

void espix_history_push(espix_history_t *h, const char *line)
{
    if (h == NULL || line == NULL || line[0] == '\0') {
        return;
    }

    if (history_is_private(line)) {
        return;
    }

    /* A command repeated straight away is noise in the list, not history. */
    if (h->count > 0 && strcmp(h->entries[0], line) == 0) {
        return;
    }

    char *copy = strdup(line);
    if (copy == NULL) {
        return;             /* history is a convenience; never fail a command */
    }

    if (h->count == ESPIX_HISTORY_MAX) {
        free(h->entries[h->count - 1]);
        h->count--;
    }

    memmove(h->entries + 1, h->entries, sizeof(h->entries[0]) * h->count);
    h->entries[0] = copy;
    h->count++;
}

void espix_history_apply(const espix_history_t *h, esp_linenoise_handle_t ed)
{
    if (h == NULL || ed == NULL) {
        return;
    }

    esp_linenoise_history_free(ed);

    /* Index 0 is where the editor stores the line being typed when the user
     * starts scrolling, so it must exist and must not be one of ours. */
    esp_linenoise_history_add(ed, "");

    for (size_t i = 0; i < h->count; i++) {
        esp_linenoise_history_add(ed, h->entries[i]);
    }
}

void espix_history_free(espix_history_t *h)
{
    if (h == NULL) {
        return;
    }
    for (size_t i = 0; i < h->count; i++) {
        free(h->entries[i]);
    }
    h->count = 0;
}

/* ------------------------------------------------------------------ */

void espix_pace(int64_t *last_us)
{
    if (last_us == NULL) {
        return;
    }

    if (esp_timer_get_time() - *last_us < ESPIX_PACE_MIN_US) {
        vTaskDelay(pdMS_TO_TICKS(ESPIX_PACE_MS));
    }
    *last_us = esp_timer_get_time();
}
