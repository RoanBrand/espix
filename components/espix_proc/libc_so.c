/* /lib/libc.so, the part of an app's libc every app shares: loaded for the first app, kept LINGER_MS after the last. */
/* Such an app's libc names are its own (linked hidden) or libc.so's, asked first; the rest are the kernel's. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <sys/lock.h>

#include "esp_elf.h"
#include "esp_heap_caps.h"

#include "espix_kernel.h"
#include "espix_proc_priv.h"

#define TAG        "libc"
#define LIBC_PATH  "/lib/libc.so"
#define LINGER_MS  30000

#define SHT_NOTE_  7
#define NOTE_ESPIX 1        /* espix's own: the app ABI and the libc build */
#define NOTE_GNU_ID 3       /* NT_GNU_BUILD_ID */


static SemaphoreHandle_t s_lock;
static esp_elf_t         s_elf;
static bool              s_loaded;
static esp_symtab_t     *s_more;        /* what the loader's table leaves out: data, and weak functions */
static unsigned          s_more_num;
static uint8_t           s_id[ESPIX_LIBC_ID_LEN];
static unsigned          s_refs;
static bool              s_lingering;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* The descriptor of the first note of `owner` and `type` in a note section's bytes. */
static const uint8_t *note_find(const uint8_t *p, size_t len, const char *owner, uint32_t type,
                                uint32_t *desc_len)
{
    const size_t olen = strlen(owner) + 1;
    while (len >= 12) {
        const uint32_t namesz = rd32(p), descsz = rd32(p + 4), t = rd32(p + 8);
        const size_t name_pad = (namesz + 3) & ~3u, desc_pad = (descsz + 3) & ~3u;
        if (name_pad > len - 12 || desc_pad > len - 12 - name_pad) {
            return NULL;
        }
        if (t == type && namesz == olen && memcmp(p + 12, owner, olen) == 0) {
            *desc_len = descsz;
            return p + 12 + name_pad;
        }
        p   += 12 + name_pad + desc_pad;
        len -= 12 + name_pad + desc_pad;
    }
    return NULL;
}

int espix_app_abi(const uint8_t *img, size_t len, uint8_t libc_id[ESPIX_LIBC_ID_LEN])
{
    if (len < 52) {
        return ESPIX_ABI_KERNEL;
    }
    const uint32_t shoff = rd32(img + 0x20);
    const uint16_t shentsize = rd16(img + 0x2E), shnum = rd16(img + 0x30);
    if (shentsize < 40 || shoff > len || (size_t)shnum * shentsize > len - shoff) {
        return ESPIX_ABI_KERNEL;
    }
    for (uint16_t i = 0; i < shnum; i++) {
        const uint8_t *const sh = img + shoff + (size_t)i * shentsize;
        const uint32_t off = rd32(sh + 16), size = rd32(sh + 20);
        if (rd32(sh + 4) != SHT_NOTE_ || off > len || size > len - off) {
            continue;
        }
        uint32_t dlen = 0;
        const uint8_t *const d = note_find(img + off, size, "espix", NOTE_ESPIX, &dlen);
        if (d != NULL && dlen == 4 + ESPIX_LIBC_ID_LEN) {
            memcpy(libc_id, d + 4, ESPIX_LIBC_ID_LEN);
            return (int)rd32(d);
        }
    }
    return ESPIX_ABI_KERNEL;
}

/* libc.so's build ID, read from the file's note sections alone. */
static bool file_build_id(FILE *f, uint8_t id[ESPIX_LIBC_ID_LEN])
{
    uint8_t eh[52], sh[40], notes[64];
    if (fread(eh, 1, sizeof(eh), f) != sizeof(eh) || memcmp(eh, "\177ELF", 4) != 0) {
        return false;
    }
    const uint32_t shoff = rd32(eh + 0x20);
    const uint16_t shentsize = rd16(eh + 0x2E), shnum = rd16(eh + 0x30);
    for (uint16_t i = 0; i < shnum && shentsize >= sizeof(sh); i++) {
        if (fseek(f, (long)(shoff + (uint32_t)i * shentsize), SEEK_SET) != 0 ||
            fread(sh, 1, sizeof(sh), f) != sizeof(sh) || rd32(sh + 4) != SHT_NOTE_) {
            continue;
        }
        const uint32_t size = rd32(sh + 20);
        if (size > sizeof(notes) || fseek(f, (long)rd32(sh + 16), SEEK_SET) != 0 ||
            fread(notes, 1, size, f) != size) {
            continue;
        }
        uint32_t dlen = 0;
        const uint8_t *const d = note_find(notes, size, "GNU", NOTE_GNU_ID, &dlen);
        if (d != NULL && dlen == ESPIX_LIBC_ID_LEN) {
            memcpy(id, d, ESPIX_LIBC_ID_LEN);
            return true;
        }
    }
    return false;
}

/* What libc.so calls in the kernel beyond the rest of the app ABI: its locks (below), and libgcc. */
extern void __ashldi3(void), __ashrdi3(void), __lshrdi3(void), __umoddi3(void), __ffsdi2(void),
            __fixdfdi(void), __floatundidf(void), __nedf2(void);
extern _Complex double __muldc3(double, double, double, double);
extern _Complex float  __mulsc3(float, float, float, float);
#if CONFIG_IDF_TARGET_ARCH_XTENSA
extern void __ieee754_sqrtf(void), __xtensa_libgcc_window_spill(void);
#endif

/* The loader's table holds global functions only; add libc.so's data and weak functions from its .dynsym. */
static bool more_symbols(const uint8_t *img, size_t len)
{
    const uint32_t shoff = rd32(img + 0x20);
    const uint16_t shentsize = rd16(img + 0x2E), shnum = rd16(img + 0x30);
    if (shentsize < 40 || shoff > len || (size_t)shnum * shentsize > len - shoff) {
        return false;
    }
    for (uint16_t i = 0; i < shnum; i++) {
        const uint8_t *const sh = img + shoff + (size_t)i * shentsize;
        if (rd32(sh + 4) != 11 /* SHT_DYNSYM */) {
            continue;
        }
        const uint32_t off = rd32(sh + 16), size = rd32(sh + 20), link = rd32(sh + 24);
        const uint8_t *const strsh = img + shoff + (size_t)link * shentsize;
        const uint32_t stroff = rd32(strsh + 16), strsize = rd32(strsh + 20);
        if (link >= shnum || off > len || size > len - off || stroff > len || strsize > len - stroff) {
            return false;
        }
        /* A weak function's executable address is a global one's, moved by their distance in .text. */
        uintptr_t ref_addr = 0;
        uint32_t  ref_value = 0;
        s_more = calloc(size / 16, sizeof(*s_more));
        if (s_more == NULL) {
            return false;
        }
        for (uint32_t k = 0; k < 2; k++) {
            for (uint32_t at = off; at + 16 <= off + size; at += 16) {
                const uint8_t *const sym = img + at;
                const uint32_t name = rd32(sym), value = rd32(sym + 4);
                const uint8_t bind = sym[12] >> 4, type = sym[12] & 0xf;
                const uint16_t shndx = rd16(sym + 14);
                if (shndx == 0 || name >= strsize || (bind != 1 && bind != 2)) {
                    continue;
                }
                const char *const nm = (const char *)img + stroff + name;
                if (k == 0) {
                    /* Pass one: a global function the loader knows, as the reference. */
                    if (ref_addr == 0 && bind == 1 && type == 2) {
                        for (uint16_t j = 0; j < s_elf.num; j++) {
                            if (strcmp(s_elf.symtab[j].name, nm) == 0) {
                                ref_addr  = (uintptr_t)s_elf.symtab[j].addr;
                                ref_value = value;
                                break;
                            }
                        }
                    }
                    continue;
                }
                if (bind == 1 && type == 2) {
                    continue;                       /* the loader's own */
                }
                const uintptr_t addr = (type == 2) ? ref_addr + (value - ref_value) : esp_elf_map_sym(&s_elf, value);
                char *const copy = strdup(nm);
                if (addr == 0 || (type == 2 && ref_addr == 0) || copy == NULL) {
                    free(copy);
                    continue;
                }
                s_more[s_more_num].name = copy;
                s_more[s_more_num].addr = (void *)addr;
                s_more_num++;
            }
        }
        return true;
    }
    return false;
}

static void more_free(void)
{
    for (unsigned i = 0; i < s_more_num; i++) {
        free(s_more[i].name);
    }
    free(s_more);
    s_more     = NULL;
    s_more_num = 0;
}

/* Read, check and relocate the file, with s_lock held. NULL, or why it could not be had. */
static const char *load(const uint8_t id[ESPIX_LIBC_ID_LEN])
{
    FILE *const f = fopen(LIBC_PATH, "rb");
    if (f == NULL) {
        return "cannot open it";
    }
    const bool have = file_build_id(f, s_id);
    long size = -1;
    if (have && fseek(f, 0, SEEK_END) == 0) {
        size = ftell(f);
    }
    uint8_t *const buf = (size > 0) ? heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    const bool read = buf != NULL && fseek(f, 0, SEEK_SET) == 0 && fread(buf, 1, (size_t)size, f) == (size_t)size;
    fclose(f);

    const char *why = NULL;
    if (!have) {
        why = "it has no build ID";
    } else if (memcmp(s_id, id, ESPIX_LIBC_ID_LEN) != 0) {
        why = "a different build from the one this app was linked against";
    } else if (!read) {
        why = "cannot read it";
    } else if (esp_elf_init(&s_elf) != 0 || esp_elf_relocate(&s_elf, buf, (size_t)size) != 0) {
        esp_elf_deinit(&s_elf);
        why = "it does not load";
    } else if (!more_symbols(buf, (size_t)size)) {
        more_free();
        esp_elf_deinit(&s_elf);
        why = "its symbol table does not read";
    } else {
        s_loaded = true;
        espix_klog(ESPIX_KLOG_INFO, TAG, "%s loaded: %u names", LIBC_PATH,
                   (unsigned)(s_elf.num + s_more_num));
    }
    free(buf);      /* the loaded image is its own copy, names included */
    return why;
}

static void unload(void)
{
    more_free();
    esp_elf_deinit(&s_elf);
    s_loaded = false;
    espix_klog(ESPIX_KLOG_INFO, TAG, "%s unloaded", LIBC_PATH);
}

/* exit()'s destructors, for an app whose own libc calls __libc_fini_array (libespix). */
static void espix_app_fini(void)
{
    espix_proc_app_fini(espix_proc_self());
}

/*
 * An app's libc locks. Each is a mutex of its process, freed with it, so a process
 * ended while holding one stops no other. The libc-wide lock an app names is a
 * stand-in for its own process's.
 */
struct espix_libc_lock {
    struct espix_libc_lock *next;
    StaticSemaphore_t       sem;
};

static portMUX_TYPE   s_locks_mux = portMUX_INITIALIZER_UNLOCKED;
static struct __lock  s_app_libc_lock;
static SemaphoreHandle_t s_stray;       /* the libc-wide lock for a caller with none of its own */

static struct espix_libc_lock *lock_new(espix_proc_app_t *app, bool recursive)
{
    struct espix_libc_lock *const l = heap_caps_malloc(sizeof(*l), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (l == NULL) {
        return NULL;
    }
    if (recursive) {
        xSemaphoreCreateRecursiveMutexStatic(&l->sem);
    } else {
        xSemaphoreCreateMutexStatic(&l->sem);
    }
    portENTER_CRITICAL(&s_locks_mux);
    l->next = app->libc_locks;
    app->libc_locks = l;
    portEXIT_CRITICAL(&s_locks_mux);
    return l;
}

static void locks_free(espix_proc_app_t *app)
{
    portENTER_CRITICAL(&s_locks_mux);
    struct espix_libc_lock *l = app->libc_locks;
    app->libc_locks = NULL;
    app->libc_mutex = NULL;
    portEXIT_CRITICAL(&s_locks_mux);
    while (l != NULL) {
        struct espix_libc_lock *const next = l->next;
        vSemaphoreDelete((SemaphoreHandle_t)&l->sem);
        free(l);
        l = next;
    }
}

static espix_proc_app_t *self_app(void)
{
    espix_proc_slot_t *const self = espix_proc_self();
    return self != NULL ? self->app : NULL;
}

static SemaphoreHandle_t lock_of(_LOCK_T lock)
{
    if (lock != (_LOCK_T)&s_app_libc_lock) {
        return (SemaphoreHandle_t)lock;
    }
    espix_proc_app_t *const app = self_app();
    return app != NULL && app->libc_mutex != NULL ? (SemaphoreHandle_t)&app->libc_mutex->sem : s_stray;
}

static void lock_init(_LOCK_T *lock, bool recursive)
{
    espix_proc_app_t *const app = self_app();
    struct espix_libc_lock *const l = app != NULL ? lock_new(app, recursive) : NULL;
    *lock = l != NULL ? (_LOCK_T)&l->sem : NULL;
}

static void app_lock_init(_LOCK_T *lock)           { lock_init(lock, false); }
static void app_lock_init_recursive(_LOCK_T *lock) { lock_init(lock, true); }

static void app_lock_close(_LOCK_T lock)
{
    espix_proc_app_t *const app = self_app();
    if (lock == NULL || lock == (_LOCK_T)&s_app_libc_lock || app == NULL) {
        return;
    }
    struct espix_libc_lock *found = NULL;
    portENTER_CRITICAL(&s_locks_mux);
    for (struct espix_libc_lock **p = &app->libc_locks; *p != NULL; p = &(*p)->next) {
        if ((_LOCK_T)&(*p)->sem == lock) {
            found = *p;
            *p = found->next;
            break;
        }
    }
    portEXIT_CRITICAL(&s_locks_mux);
    if (found != NULL) {
        vSemaphoreDelete((SemaphoreHandle_t)&found->sem);
        free(found);
    }
}

/* A NULL lock is one that could not be made: the app runs unlocked rather than the kernel asserting. */
static void app_lock_acquire(_LOCK_T lock)
{
    SemaphoreHandle_t const s = lock_of(lock);
    if (s != NULL) {
        xSemaphoreTake(s, portMAX_DELAY);
    }
}

static void app_lock_release(_LOCK_T lock)
{
    SemaphoreHandle_t const s = lock_of(lock);
    if (s != NULL) {
        xSemaphoreGive(s);
    }
}

static void app_lock_acquire_recursive(_LOCK_T lock)
{
    SemaphoreHandle_t const s = lock_of(lock);
    if (s != NULL) {
        xSemaphoreTakeRecursive(s, portMAX_DELAY);
    }
}

static void app_lock_release_recursive(_LOCK_T lock)
{
    SemaphoreHandle_t const s = lock_of(lock);
    if (s != NULL) {
        xSemaphoreGiveRecursive(s);
    }
}

static const struct esp_elfsym s_libc_hooks[] = {
    ESP_ELFSYM_EXPORT(espix_app_fini),
    ESP_ELFSYM_EXPORT_NAMED("__retarget_lock_init", app_lock_init),
    ESP_ELFSYM_EXPORT_NAMED("__retarget_lock_init_recursive", app_lock_init_recursive),
    ESP_ELFSYM_EXPORT_NAMED("__retarget_lock_close", app_lock_close),
    ESP_ELFSYM_EXPORT_NAMED("__retarget_lock_close_recursive", app_lock_close),
    ESP_ELFSYM_EXPORT_NAMED("__retarget_lock_acquire", app_lock_acquire),
    ESP_ELFSYM_EXPORT_NAMED("__retarget_lock_acquire_recursive", app_lock_acquire_recursive),
    ESP_ELFSYM_EXPORT_NAMED("__retarget_lock_release", app_lock_release),
    ESP_ELFSYM_EXPORT_NAMED("__retarget_lock_release_recursive", app_lock_release_recursive),
    ESP_ELFSYM_EXPORT_NAMED("__lock___libc_recursive_mutex", s_app_libc_lock),
    ESP_ELFSYM_EXPORT(__ashldi3),
    ESP_ELFSYM_EXPORT(__ashrdi3),
    ESP_ELFSYM_EXPORT(__lshrdi3),
    ESP_ELFSYM_EXPORT(__umoddi3),
    ESP_ELFSYM_EXPORT(__ffsdi2),
    ESP_ELFSYM_EXPORT(__fixdfdi),
    ESP_ELFSYM_EXPORT(__floatundidf),
    ESP_ELFSYM_EXPORT(__nedf2),
    ESP_ELFSYM_EXPORT(__muldc3),
    ESP_ELFSYM_EXPORT(__mulsc3),
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    ESP_ELFSYM_EXPORT(__ieee754_sqrtf),
    ESP_ELFSYM_EXPORT(__xtensa_libgcc_window_spill),
#endif
    ESP_ELFSYM_END
};

void espix_libc_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_stray = xSemaphoreCreateRecursiveMutex();
    if (esp_elf_register_symbol(s_libc_hooks) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "libc.so's kernel names not published");
    }
}

void espix_libc_app_init(espix_proc_slot_t *slot)
{
    slot->app->libc_mutex = lock_new(slot->app, true);
}

const char *espix_libc_acquire(espix_proc_slot_t *slot, const uint8_t id[ESPIX_LIBC_ID_LEN])
{
    const char *why = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* A lingering copy nobody uses gives way to the file, which may have been replaced. */
    if (s_loaded && s_refs == 0 && memcmp(s_id, id, ESPIX_LIBC_ID_LEN) != 0) {
        unload();
    }
    if (!s_loaded) {
        why = load(id);
    } else if (memcmp(s_id, id, ESPIX_LIBC_ID_LEN) != 0) {
        why = "a different build from the one this app was linked against";
    }
    if (why == NULL) {
        s_refs++;
        slot->app->libc_held = true;
    }
    xSemaphoreGive(s_lock);
    return why;
}

static void linger_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(LINGER_MS));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_refs == 0 && s_loaded) {
        unload();
    }
    s_lingering = false;
    xSemaphoreGive(s_lock);
    vTaskDelete(NULL);
}

void espix_libc_release(espix_proc_slot_t *slot)
{
    if (slot->app == NULL) {
        return;
    }
    locks_free(slot->app);
    if (!slot->app->libc_held) {
        return;
    }
    slot->app->libc_held = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (--s_refs == 0 && !s_lingering &&
        xTaskCreate(linger_task, "libc:linger", 3072, NULL, tskIDLE_PRIORITY + 1, NULL) == pdPASS) {
        s_lingering = true;
    }
    xSemaphoreGive(s_lock);
}

uintptr_t espix_libc_resolve(const char *name, uintptr_t (*kernel)(const char *name))
{
    /* Stable while it runs: the app being relocated holds a reference. */
    if (s_loaded) {
        for (uint16_t i = 0; i < s_elf.num; i++) {
            if (strcmp(s_elf.symtab[i].name, name) == 0) {
                return (uintptr_t)s_elf.symtab[i].addr;
            }
        }
        for (unsigned i = 0; i < s_more_num; i++) {
            if (strcmp(s_more[i].name, name) == 0) {
                return (uintptr_t)s_more[i].addr;
            }
        }
    }
    return kernel(name);
}
