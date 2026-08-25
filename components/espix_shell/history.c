/*
 * Per-session command history.
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

#include "espix_shell.h"

void espix_history_push(espix_history_t *h, const char *line)
{
    if (h == NULL || line == NULL || line[0] == '\0') {
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
