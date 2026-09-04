/*
 * espix exec: read an ELF off the rootfs, relocate it, and run it as a process.
 *
 * This is the centerpiece path — an app cross-compiled on a PC, deployed as a
 * file, and executed without being linked into the firmware.
 *
 * We read the file ourselves rather than using esp_elf_open(), which resolves
 * names against CONFIG_ELF_FILE_SYSTEM_BASE_PATH only; espix needs to run an
 * ELF at any absolute path the shell hands it.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_proc_priv.h"
#include "espix_shell.h"

#define TAG "exec"

/* Largest app image we will read into RAM. Guards against `run`ning something
 * that is not an app at all. */
#define EXEC_IMAGE_MAX (2 * 1024 * 1024)

static void *image_alloc(size_t size)
{
#if CONFIG_ESPIX_PROC_IMAGE_IN_PSRAM
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != NULL) {
        return p;
    }
#endif
    return malloc(size);
}

static esp_err_t load_image(const char *path, uint8_t **out_buf, size_t *out_size)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!S_ISREG(st.st_mode)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (st.st_size == 0 || st.st_size > EXEC_IMAGE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    const size_t size = (size_t)st.st_size;
    uint8_t *buf = image_alloc(size);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    const size_t got = fread(buf, 1, size, f);
    fclose(f);

    if (got != size) {
        free(buf);
        return ESP_FAIL;
    }

    /* Cheapest possible sanity check before handing it to the relocator. */
    if (memcmp(buf, "\177ELF", 4) != 0) {
        free(buf);
        return ESP_ERR_NOT_SUPPORTED;
    }

    *out_buf  = buf;
    *out_size = size;
    return ESP_OK;
}

/*
 * Pack argv into a single allocation: the char* array, then the strings.
 * One free() releases the lot, which matters on the kill path.
 */
static esp_err_t copy_argv(int argc, char **argv, espix_proc_slot_t *slot)
{
    size_t bytes = sizeof(char *) * (size_t)(argc + 1);
    for (int i = 0; i < argc; i++) {
        bytes += strlen(argv[i]) + 1;
    }

    char **vec = malloc(bytes);
    if (vec == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char *strings = (char *)(vec + argc + 1);
    for (int i = 0; i < argc; i++) {
        const size_t len = strlen(argv[i]) + 1;
        memcpy(strings, argv[i], len);
        vec[i] = strings;
        strings += len;
    }
    vec[argc] = NULL;

    slot->argv_block = vec;
    slot->argv       = vec;
    slot->argc       = argc;
    return ESP_OK;
}

static void proc_task(void *arg)
{
    espix_proc_slot_t *slot = arg;

    /*
     * Wait until the parent has finished admitting us to the process table.
     *
     * xTaskCreate() starts this task at CONFIG_ESPIX_PROC_PRIORITY, which is
     * *higher* than the console session's -- that runs on main_task at
     * priority 1, whatever the Kconfig help used to claim -- so the child
     * preempts the parent the instant it is created, before espix_proc_spawn_elf()
     * has stored the task handle in the slot. An app that called getpid() or
     * installed a signal handler in that window found no table entry for itself
     * at all: its slot still read task=NULL, state=READY.
     *
     * The parent holds this lock across the whole admission, so taking it here
     * is precisely "wait until I am fully in the table". The mutex carries
     * priority inheritance, so the lower-priority parent is boosted to finish
     * rather than left behind.
     */
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    xSemaphoreGive(g_espix_proc_lock);

    /* Inherit the launching session so the app's stdio and any espix_printf()
     * from this task reach whoever ran it. */
    espix_shell_set_current(slot->info.session);

    /*
     * Point this task's stdout and stderr at the session, so an app's own
     * printf() lands where the user is rather than on the serial console.
     *
     * This works because ESP-IDF gives every task its own struct _reent whose
     * streams are pre-pointed at the global ones (esp_reent_init), so the
     * assignment affects this task alone. It is the same trick ESP-IDF's own
     * console REPL uses.
     *
     * Two separate streams, and neither is closed here: task teardown calls
     * esp_cleanup_r(), which fcloses whichever of stdin/stdout/stderr differ
     * from the global ones. One object shared between stdout and stderr would
     * be closed twice, which asserts inside the second fclose.
     *
     * Not applied to `>` redirection: that FILE is closed when the command
     * returns, and a backgrounded app would outlive it.
     */
    espix_session_t *const session = slot->info.session;
    const bool own_streams = (session != NULL && session->open_stream != NULL);

    if (own_streams) {
        FILE *const out = session->open_stream(session);
        FILE *const err = session->open_stream(session);
        if (out != NULL) {
            stdout = out;
        }
        if (err != NULL) {
            stderr = err;
        }
    }

    int  status = -1;
    bool ran    = false;

    esp_err_t err = load_image(slot->info.path, &slot->image,
                               &slot->info.image_bytes);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "pid %d: cannot load %s: %s",
                   (int)slot->info.pid, slot->info.path, esp_err_to_name(err));
        espix_printf(slot->info.session, "espix: %s: %s\n",
                     slot->info.path, esp_err_to_name(err));
        goto done;
    }

    if (esp_elf_init(&slot->elf) != 0) {
        espix_printf(slot->info.session, "espix: %s: ELF init failed\n",
                     slot->info.path);
        goto done;
    }
    slot->elf_valid = true;

    if (esp_elf_relocate(&slot->elf, slot->image) != 0) {
        espix_printf(slot->info.session, "espix: %s: relocation failed\n",
                     slot->info.path);
        goto done;
    }

    /* Debug, not info: every `run` would otherwise print a line above the
     * app's own output. Still recorded, so `dmesg` can show it. */
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "pid %d: %s relocated (%u bytes)",
               (int)slot->info.pid, slot->info.name,
               (unsigned)slot->info.image_bytes);

    if (slot->elf.entry == NULL) {
        espix_printf(slot->info.session, "espix: %s: no entry point\n",
                     slot->info.path);
        goto done;
    }

    /*
     * Called directly rather than through esp_elf_request(), which is
     * "elf->entry(argc, argv); return 0;" — it throws the app's return value
     * away, so an exit status could never reach the shell. This replicates its
     * only other behaviour (the NULL check above). Do not "fix" this back.
     */
    ran = true;
    status = slot->elf.entry(slot->argc, slot->argv);

done:
    /*
     * An app that printf()'d without a trailing newline still has bytes sitting
     * in the stream buffer. Flush here rather than leaving it to the fclose in
     * esp_cleanup_r(): the shell is released the moment this process is marked
     * finished, so anything still buffered would land after its next prompt.
     */
    if (own_streams) {
        fflush(stdout);
        fflush(stderr);
    }

    /* Tear the ELF down before releasing the file buffer: the relocated image
     * can still reference it until deinit. */
    espix_proc_release_resources(slot);

    espix_proc_finish(slot,
                      ran ? ESPIX_PROC_EXITED : ESPIX_PROC_FAULTED,
                      status);

    espix_shell_set_current(NULL);
    vTaskDelete(NULL);
}

esp_err_t espix_proc_spawn_elf(const char *abs_path, int argc, char **argv,
                               espix_session_t *session, espix_pid_t *out_pid)
{
    if (abs_path == NULL || abs_path[0] != '/' || argc < 1 || argv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_espix_proc_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);

    espix_proc_slot_t *slot = espix_proc_alloc_slot();
    if (slot == NULL) {
        xSemaphoreGive(g_espix_proc_lock);
        return ESP_ERR_NO_MEM;      /* process table full of live processes */
    }

    esp_err_t err = copy_argv(argc, argv, slot);
    if (err != ESP_OK) {
        memset(slot, 0, sizeof(*slot));
        xSemaphoreGive(g_espix_proc_lock);
        return err;
    }

    const char *base = strrchr(abs_path, '/');
    base = (base != NULL) ? base + 1 : abs_path;

    strlcpy(slot->info.path, abs_path, sizeof(slot->info.path));
    strlcpy(slot->info.name, base, sizeof(slot->info.name));
    slot->info.pid        = espix_proc_next_pid();
    slot->info.state      = ESPIX_PROC_READY;
    slot->info.session    = session;

    /* Inherited from whoever spawned it, like a child's cwd everywhere else.
     * A session with no cwd, or no session at all, means "/". */
    strlcpy(slot->cwd,
            (session != NULL && session->cwd[0] != '\0') ? session->cwd : "/",
            sizeof(slot->cwd));
    slot->info.started_us = esp_timer_get_time();
    slot->info.exit_code  = 0;

    const int index = (int)(slot - g_espix_procs);
    xEventGroupClearBits(g_espix_proc_events, (EventBits_t)1 << index);

    /* FreeRTOS truncates task names to configMAX_TASK_NAME_LEN anyway; do it
     * explicitly so the app name, not the "app:" prefix, is what gets cut. */
    char task_name[configMAX_TASK_NAME_LEN];
    snprintf(task_name, sizeof(task_name), "app:%.*s",
             (int)(sizeof(task_name) - sizeof("app:")), slot->info.name);

    TaskHandle_t task = NULL;
    const BaseType_t ok = xTaskCreate(proc_task, task_name,
                                      CONFIG_ESPIX_PROC_STACK_SIZE, slot,
                                      CONFIG_ESPIX_PROC_PRIORITY, &task);
    if (ok != pdPASS) {
        free(slot->argv_block);
        memset(slot, 0, sizeof(*slot));
        xSemaphoreGive(g_espix_proc_lock);
        return ESP_ERR_NO_MEM;
    }

    slot->info.task  = task;
    slot->info.state = ESPIX_PROC_RUNNING;

    if (out_pid != NULL) {
        *out_pid = slot->info.pid;
    }

    xSemaphoreGive(g_espix_proc_lock);
    return ESP_OK;
}
