/*
 * The periodic heap check of CONFIG_ESPIX_HEAP_CHECK_MS: dates heap corruption
 * to one period rather than to whichever free() trips over it later.
 */

#include "sdkconfig.h"

#include "espix_kernel.h"

#if CONFIG_ESPIX_HEAP_CHECK_MS > 0

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_rom_sys.h"

#include "espix_task.h"

#if CONFIG_HEAP_POISONING_COMPREHENSIVE || CONFIG_HEAP_POISONING_LIGHT
#define HEAD_CANARY 0xABBA1234u     /* multi_heap_poisoning.c's */
#define TAIL_CANARY 0xBAAD5678u
#define AROUND      128             /* bytes shown either side of a damaged tail */

/* A used block whose canaries are wrong, copied out under the heap's lock and printed after it. */
static struct {
    const uint8_t *data;
    uint32_t       size;
    void          *owner;
    const uint8_t *from;
    uint32_t       len;
    uint8_t        bytes[2 * AROUND];
} s_bad[4];
static int s_nbad;

static bool find_damage(walker_heap_into_t heap, walker_block_info_t b, void *arg)
{
    (void)arg;
    if (!b.used || b.size < 12 || s_nbad == (int)(sizeof(s_bad) / sizeof(s_bad[0]))) {
        return true;
    }
    const uint32_t *const head = b.ptr;
    const uint8_t *const  data = (const uint8_t *)b.ptr + 8;
    const uint32_t        size = head[1];
    uint32_t              tail = 0;
    const bool sane = head[0] == HEAD_CANARY && size + 12 <= b.size;
    if (sane) {
        memcpy(&tail, data + size, sizeof(tail));
    }
    if (sane && tail == TAIL_CANARY) {
        return true;
    }
    const uint8_t *const end  = sane ? data + size : data;
    const uint8_t       *from = (end - AROUND > data) ? end - AROUND : data;
    const uint8_t *const to   = ((intptr_t)(end + AROUND) < heap.end) ? end + AROUND : (const uint8_t *)heap.end;
    s_bad[s_nbad].data  = data;
    s_bad[s_nbad].size  = sane ? size : 0;
#if CONFIG_HEAP_TASK_TRACKING
    s_bad[s_nbad].owner = sane ? *(void *const *)data : NULL;
#else
    s_bad[s_nbad].owner = NULL;
#endif
    s_bad[s_nbad].from  = from;
    s_bad[s_nbad].len   = (uint32_t)(to - from);
    memcpy(s_bad[s_nbad].bytes, from, s_bad[s_nbad].len);
    s_nbad++;
    return true;
}

static TaskStatus_t s_tasks[48];

static const char *task_name(void *handle, UBaseType_t ntasks)
{
    for (UBaseType_t i = 0; handle != NULL && i < ntasks; i++) {
        if (s_tasks[i].xHandle == handle) {
            return s_tasks[i].pcTaskName;
        }
    }
    return "(gone)";
}

static void show_damage(void)
{
    s_nbad = 0;
    heap_caps_walk_all(find_damage, NULL);
    const UBaseType_t ntasks = uxTaskGetSystemState(s_tasks, sizeof(s_tasks) / sizeof(s_tasks[0]), NULL);
#if CONFIG_HEAP_TASK_TRACKING
    const uint32_t owner_word = sizeof(void *);     /* what malloc returned starts after it */
#else
    const uint32_t owner_word = 0;
#endif
    for (int i = 0; i < s_nbad; i++) {
        const uint32_t size = (s_bad[i].size >= owner_word) ? s_bad[i].size - owner_word : 0;
        esp_rom_printf("heapcheck: block %p, %u bytes, allocated by task %p %s; around its end:\n",
                       s_bad[i].data + owner_word, (unsigned)size, s_bad[i].owner,
                       task_name(s_bad[i].owner, ntasks));
        for (uint32_t at = 0; at < s_bad[i].len; at += 16) {
            esp_rom_printf("  %p ", s_bad[i].from + at);
            for (uint32_t k = at; k < at + 16 && k < s_bad[i].len; k++) {
                esp_rom_printf(" %02x", s_bad[i].bytes[k]);
            }
            esp_rom_printf("  ");
            for (uint32_t k = at; k < at + 16 && k < s_bad[i].len; k++) {
                const uint8_t c = s_bad[i].bytes[k];
                esp_rom_printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
            }
            esp_rom_printf("\n");
        }
    }
}
#else
static void show_damage(void)
{
}
#endif

static void heapcheck_task(void *arg)
{
    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_ESPIX_HEAP_CHECK_MS));
        if (!heap_caps_check_integrity_all(false)) {
            /* The damaged blocks' owners and bytes first: IDF's own report prints under the heap lock, and can trip the watchdog. */
            show_damage();
            (void)heap_caps_check_integrity_all(true);
            abort();
        }
    }
}

void espix_heapcheck_start(void)
{
    (void)espix_kthread_start(&(const espix_kthread_t){
        .name = "heapcheck", .fn = heapcheck_task, .stack = 4096,
        .priority = 1, .core = tskNO_AFFINITY, .internal_stack = true }, NULL);
}

#else

void espix_heapcheck_start(void)
{
}

#endif
