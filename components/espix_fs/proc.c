/*
 * The /proc tree: kernel state as files, generated on every read.
 *
 * Nothing is stored and nothing is generated until something reads it, which is
 * the whole point -- reading meminfo costs a formatting pass and the boot costs
 * nothing. The engine is synth.c.
 *
 * The files are read-only and world-readable: they expose no secret (heap
 * figures, the chip, the version, uptime), and Linux does the same. Values are
 * formatted from the same sources free and uname use, so the two cannot
 * disagree about what a figure means.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_syscall.h"

#include "espix_fs_priv.h"

/* The two pools espix reports, and the same masks free uses. */
#define PROC_INTERNAL_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define PROC_PSRAM_CAPS    (MALLOC_CAP_SPIRAM)

/* ------------------------------------------------------------ cache */
/*
 * /proc is generated on every read, and htop reads a process's stat, statm,
 * comm and status back to back.  Without a cache each of those files takes its
 * own uxTaskGetSystemState() and espix_proc_snapshot(); a scan of eleven
 * processes becomes dozens of full task walks.  One 500 ms snapshot shared by
 * all of them is enough for the /proc contract (a process may be up to half a
 * second stale) and removes the repeated walks.
 *
 * The arrays live in PSRAM, not internal RAM: they are read often but are not
 * touched by an ISR.  The task array is sized from the live task count and
 * grown when needed; the process array is the process table's fixed size.
 */
#define PROC_CACHE_TTL_US 200000

static SemaphoreHandle_t s_cache_lock;
static TaskStatus_t     *s_task_cache;
static UBaseType_t       s_task_cap, s_task_n;
static int64_t           s_task_at;
static uint32_t          s_task_generation;
static espix_proc_info_t *s_info_cache;
static size_t            s_info_n;
static int64_t           s_info_at;
static uint32_t          s_info_generation;

static void *proc_cache_alloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, PROC_PSRAM_CAPS | MALLOC_CAP_8BIT);
    return (p != NULL) ? p : calloc(1, n);
}

static bool proc_cache_tasks_locked(int64_t now)
{
    const uint32_t generation = espix_proc_generation();
    if (s_task_cache != NULL && generation == s_task_generation &&
        now - s_task_at < PROC_CACHE_TTL_US) {
        return true;
    }

    const UBaseType_t cap = uxTaskGetNumberOfTasks() + 8;
    if (cap > s_task_cap) {
        TaskStatus_t *p = heap_caps_realloc(s_task_cache, cap * sizeof(*p),
                                            PROC_PSRAM_CAPS | MALLOC_CAP_8BIT);
        if (p == NULL) {
            return false;
        }
        s_task_cache = p;
        s_task_cap   = cap;
    }

    s_task_n          = uxTaskGetSystemState(s_task_cache, s_task_cap, NULL);
    s_task_at         = now;
    s_task_generation = generation;
    return true;
}

static bool proc_cache_infos_locked(int64_t now)
{
    const uint32_t generation = espix_proc_generation();
    if (s_info_cache != NULL && generation == s_info_generation &&
        now - s_info_at < PROC_CACHE_TTL_US) {
        return true;
    }
    if (s_info_cache == NULL) {
        return false;
    }
    s_info_n          = espix_proc_snapshot(s_info_cache, CONFIG_ESPIX_PROC_MAX);
    s_info_at         = now;
    s_info_generation = generation;
    return true;
}

static bool proc_cache_acquire_tasks(const TaskStatus_t **out, UBaseType_t *n)
{
    if (s_cache_lock == NULL ||
        xSemaphoreTake(s_cache_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (!proc_cache_tasks_locked(esp_timer_get_time())) {
        xSemaphoreGive(s_cache_lock);
        return false;
    }
    *out = s_task_cache;
    *n   = s_task_n;
    return true;
}

static bool proc_cache_acquire_infos(const espix_proc_info_t **out, size_t *n)
{
    if (s_cache_lock == NULL ||
        xSemaphoreTake(s_cache_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (!proc_cache_infos_locked(esp_timer_get_time())) {
        xSemaphoreGive(s_cache_lock);
        return false;
    }
    *out = s_info_cache;
    *n   = s_info_n;
    return true;
}

static void proc_cache_release(void)
{
    xSemaphoreGive(s_cache_lock);
}


/* Linux spells meminfo as "Key: value kB". The pool names are espix's, because
 * a single total would hide which pool is nearly gone -- the thing this file
 * exists to make scriptable. */
static size_t proc_meminfo(char *buf, size_t cap)
{
    multi_heap_info_t in, ps;

    heap_caps_get_info(&in, PROC_INTERNAL_CAPS);
    heap_caps_get_info(&ps, PROC_PSRAM_CAPS);

    const size_t in_total = heap_caps_get_total_size(PROC_INTERNAL_CAPS);
    const size_t ps_total = heap_caps_get_total_size(PROC_PSRAM_CAPS);
    const size_t total    = in_total + ps_total;
    const size_t free_b   = in.total_free_bytes + ps.total_free_bytes;

    return (size_t)snprintf(
        buf, cap,
        "MemTotal:        %8u kB\n"
        "MemFree:         %8u kB\n"
        "MemAvailable:    %8u kB\n"
        "Buffers:             0 kB\n"
        "Cached:              0 kB\n"
        "SwapCached:          0 kB\n"
        "SwapTotal:           0 kB\n"
        "SwapFree:            0 kB\n"
        "InternalTotal:   %8u kB\n"
        "InternalFree:    %8u kB\n"
        "InternalLargest: %8u kB\n"
        "InternalMinFree: %8u kB\n"
        "PsramTotal:      %8u kB\n"
        "PsramFree:       %8u kB\n"
        "PsramLargest:    %8u kB\n",
        (unsigned)(total / 1024),
        (unsigned)(free_b / 1024),
        (unsigned)(free_b / 1024),
        (unsigned)(in_total / 1024),
        (unsigned)(in.total_free_bytes / 1024),
        (unsigned)(in.largest_free_block / 1024),
        (unsigned)(heap_caps_get_minimum_free_size(PROC_INTERNAL_CAPS) / 1024),
        (unsigned)(ps_total / 1024),
        (unsigned)(ps.total_free_bytes / 1024),
        (unsigned)(ps.largest_free_block / 1024));
}

static size_t proc_cpuinfo(char *buf, size_t cap)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);

    uint8_t mac[6] = { 0 };
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);

    const unsigned cores = (ci.cores > 0) ? (unsigned)ci.cores : 1;
    const int      hz    = esp_clk_cpu_freq();
    const unsigned mhz     = (hz > 0) ? (unsigned)(hz / 1000000)
                                     : (unsigned)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    const unsigned mhz_frac = (hz > 0) ? (unsigned)((hz / 1000) % 1000) : 0;

    size_t len = 0;
    for (unsigned i = 0; i < cores && len < cap; i++) {
        const int w = snprintf(
            buf + len, cap - len,
            "processor\t: %u\n"
            "model name\t: %s (rev v%d.%d)\n"
            "cpu MHz\t\t: %u.%03u\n",
            i, espix_chip_model(),
            (int)(ci.revision / 100), (int)(ci.revision % 100),
            mhz, mhz_frac);
        if (w < 0) {
            break;
        }
        len += (size_t)w;
        if (len > cap) {
            len = cap;
        }
    }

    if (len < cap) {
        const int w = snprintf(
            buf + len, cap - len,
            "cores\t\t: %u\n"
            "features\t:%s%s%s\n"
            "idf version\t: %s\n"
            "mac\t\t: %02x:%02x:%02x:%02x:%02x:%02x\n",
            cores,
            (ci.features & CHIP_FEATURE_WIFI_BGN) ? " wifi" : "",
            (ci.features & CHIP_FEATURE_BT) ? " bt" : "",
            (ci.features & CHIP_FEATURE_BLE) ? " ble" : "",
            esp_get_idf_version(),
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        if (w > 0) {
            len += (size_t)w;
            if (len > cap) {
                len = cap;
            }
        }
    }
    return len;
}

/*
 * /proc/stat: cumulative CPU time in USER_HZ ticks.
 *
 * The tick is a userspace ABI (ESPIX_USER_HZ), not the FreeRTOS tick. The
 * counters come from the same place top's per-core figures do: each idle task
 * is pinned to one core, so what that core did not spend idle is what it spent
 * busy. There is no per-mode split available, so all busy time is "user" and
 * the system/irq fields are zero.
 */
static uint64_t proc_ticks(int64_t us)
{
    if (us <= 0) {
        return 0;
    }
    return ((uint64_t)us * ESPIX_USER_HZ) / 1000000u;
}

static int proc_task_core(const TaskStatus_t *t)
{
#if configTASKLIST_INCLUDE_COREID
    return (t->xCoreID == tskNO_AFFINITY) ? 0 : (int)t->xCoreID;
#else
    (void)t;
    return 0;
#endif
}

static size_t proc_stat(char *buf, size_t cap)
{
    const TaskStatus_t *tasks;
    UBaseType_t         got;
    if (!proc_cache_acquire_tasks(&tasks, &got)) {
        return 0;
    }

    const int64_t  now_us = esp_timer_get_time();
    const uint64_t    total  = proc_ticks(now_us);

    uint64_t idle_us[portNUM_PROCESSORS] = { 0 };
    unsigned running = 0, blocked = 0;

    for (UBaseType_t i = 0; i < got; i++) {
        const bool idle = strncmp(tasks[i].pcTaskName, "IDLE", 4) == 0;
        if (idle) {
            const int core = proc_task_core(&tasks[i]);
            if (core >= 0 && core < portNUM_PROCESSORS) {
                idle_us[core] += (uint64_t)tasks[i].ulRunTimeCounter;
            }
            continue;
        }
        if (tasks[i].eCurrentState == eRunning ||
            tasks[i].eCurrentState == eReady) {
            running++;
        } else if (tasks[i].eCurrentState == eBlocked) {
            blocked++;
        }
    }

    uint64_t user[portNUM_PROCESSORS] = { 0 };
    uint64_t system[portNUM_PROCESSORS] = { 0 };
    uint64_t idle[portNUM_PROCESSORS] = { 0 };
    uint64_t all_user = 0, all_system = 0, all_idle = 0;

    for (int c = 0; c < portNUM_PROCESSORS; c++) {
        idle[c] = proc_ticks((int64_t)idle_us[c]);
        if (idle[c] > total) {
            idle[c] = total;
        }
        const uint64_t busy = total - idle[c];
        system[c] = proc_ticks((int64_t)espix_cpu_system_us(c));
        if (system[c] > busy) {
            system[c] = busy;
        }
        user[c] = busy - system[c];
        all_user += user[c];
        all_system += system[c];
        all_idle += idle[c];
    }

    size_t len = 0;
#define PROC_APPEND(...)                                                   \
    do {                                                                   \
        if (len < cap) {                                                   \
            const int w_ = snprintf(buf + len, cap - len, __VA_ARGS__);    \
            if (w_ > 0) {                                                  \
                len += (size_t)w_;                                         \
            }                                                              \
            if (len > cap) {                                               \
                len = cap;                                                 \
            }                                                              \
        }                                                                  \
    } while (0)

    PROC_APPEND("cpu  %llu 0 %llu %llu 0 0 0 0 0 0\n",
                (unsigned long long)all_user,
                (unsigned long long)all_system,
                (unsigned long long)all_idle);
    for (int c = 0; c < portNUM_PROCESSORS; c++) {
        PROC_APPEND("cpu%d %llu 0 %llu %llu 0 0 0 0 0 0\n", c,
                    (unsigned long long)user[c],
                    (unsigned long long)system[c],
                    (unsigned long long)idle[c]);
    }
    const time_t    wall   = time(NULL);
    const long long uptime = (long long)(now_us / 1000000);
    const long long btime  = (wall > uptime) ? (long long)wall - uptime : 0;
    PROC_APPEND("btime %lld\n", btime);
    PROC_APPEND("procs_running %u\n", running);
    PROC_APPEND("procs_blocked %u\n", blocked);

#undef PROC_APPEND

    proc_cache_release();
    return len;
}

/* Linux prints "<uptime> <idle>". espix tracks no idle total, so both fields
 * are uptime -- a reader that takes the first gets the right answer, and one
 * that wants idle should ask /proc/stat once that exists rather than trust
 * this. */
static size_t proc_uptime(char *buf, size_t cap)
{
    const int64_t us  = esp_timer_get_time();
    const int64_t sec = us / 1000000;
    const int64_t cs  = (us / 10000) % 100;

    return (size_t)snprintf(buf, cap, "%lld.%02lld %lld.%02lld\n",
                            (long long)sec, (long long)cs,
                            (long long)sec, (long long)cs);
}

/*
 * /proc/loadavg: the 1, 5 and 15 minute averages, sampled every five seconds
 * like Linux's, in fixed point 1/2048. The runnable count excludes the idle
 * tasks, which are always ready and would otherwise be a constant +2.
 */
#define LOAD_SCALE 2048u
#define LOAD_1M    1884u    /* exp(-5/60)  * LOAD_SCALE */
#define LOAD_5M    2014u    /* exp(-5/300) * LOAD_SCALE */
#define LOAD_15M   2037u    /* exp(-5/900) * LOAD_SCALE */

static uint32_t s_load1, s_load5, s_load15;
static unsigned s_load_running;

static void proc_load_sample(void *arg)
{
    (void)arg;
    const TaskStatus_t *ts;
    UBaseType_t         got;
    if (!proc_cache_acquire_tasks(&ts, &got)) {
        return;
    }
    unsigned n = 0;
    for (UBaseType_t i = 0; i < got; i++) {
        if (strncmp(ts[i].pcTaskName, "IDLE", 4) == 0) {
            continue;
        }
        if (ts[i].eCurrentState == eRunning || ts[i].eCurrentState == eReady) {
            n++;
        }
    }
    proc_cache_release();
    s_load_running = n;
    s_load1  = (uint32_t)(((uint64_t)s_load1  * LOAD_1M)  / LOAD_SCALE +
                          (uint64_t)n * (LOAD_SCALE - LOAD_1M));
    s_load5  = (uint32_t)(((uint64_t)s_load5  * LOAD_5M)  / LOAD_SCALE +
                          (uint64_t)n * (LOAD_SCALE - LOAD_5M));
    s_load15 = (uint32_t)(((uint64_t)s_load15 * LOAD_15M) / LOAD_SCALE +
                          (uint64_t)n * (LOAD_SCALE - LOAD_15M));
}

static esp_timer_handle_t s_load_timer;

void espix_fs_proc_init(void)
{
    if (s_cache_lock == NULL) {
        s_cache_lock = xSemaphoreCreateMutex();
    }
    if (s_info_cache == NULL) {
        s_info_cache = proc_cache_alloc(CONFIG_ESPIX_PROC_MAX * sizeof(*s_info_cache));
    }

    if (s_load_timer != NULL) {
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = proc_load_sample,
        .name     = "loadavg",
    };
    if (esp_timer_create(&args, &s_load_timer) == ESP_OK) {
        (void)esp_timer_start_periodic(s_load_timer, 5 * 1000 * 1000);
    }
}

/* Hundredths of a load unit, so the file needs no floating-point printf. */
static unsigned load_hundredths(uint32_t s)
{
    return (unsigned)(((uint64_t)s * 100 + LOAD_SCALE / 2) / LOAD_SCALE);
}

static size_t proc_loadavg(char *buf, size_t cap)
{
    const unsigned l1 = load_hundredths(s_load1);
    const unsigned l5 = load_hundredths(s_load5);
    const unsigned l15 = load_hundredths(s_load15);
    return (size_t)snprintf(
        buf, cap, "%u.%02u %u.%02u %u.%02u %u/%u 0\n",
        l1 / 100, l1 % 100, l5 / 100, l5 % 100, l15 / 100, l15 % 100,
        s_load_running, (unsigned)uxTaskGetNumberOfTasks());
}

static size_t proc_version(char *buf, size_t cap)
{
    return (size_t)snprintf(buf, cap, "espix %s (%s) %s\n",
                            espix_version(), espix_build_id(),
                            espix_chip_model());
}

static const synth_node_t s_proc_nodes[] = {
    { "/proc/meminfo", SYNTH_TEXT, S_IFREG | 0444, 0, 0, proc_meminfo, NULL, 0 },
    { "/proc/cpuinfo", SYNTH_TEXT, S_IFREG | 0444, 0, 0, proc_cpuinfo, NULL, 0 },
    { "/proc/stat",    SYNTH_TEXT, S_IFREG | 0444, 0, 0, proc_stat,    NULL, 0 },
    { "/proc/loadavg", SYNTH_TEXT, S_IFREG | 0444, 0, 0, proc_loadavg, NULL, 0 },
    { "/proc/uptime",  SYNTH_TEXT, S_IFREG | 0444, 0, 0, proc_uptime, NULL, 0 },
    { "/proc/version", SYNTH_TEXT, S_IFREG | 0444, 0, 0, proc_version, NULL, 0 },
};

/* ------------------------------------------------------------ /proc/<pid> */

/*
 * /proc/<pid>/status and /proc/<pid>/task/<tid>/status, generated from the
 * process table like the rest. A pid directory exists while the process does;
 * task lists the process's own pid and each of its threads.
 */

static const char *kind_str(espix_proc_kind_t kind)
{
    switch (kind) {
    case ESPIX_PROC_APP:     return "app";
    case ESPIX_PROC_BUILTIN: return "command";
    case ESPIX_PROC_DAEMON:  return "daemon";
    case ESPIX_PROC_KTHREAD: return "kernel";
    case ESPIX_PROC_SESSION: return "shell";
    default:                 return "?";
    }
}

static bool proc_info(espix_pid_t pid, espix_proc_info_t *out)
{
    const espix_proc_info_t *live;
    size_t                   n;
    if (!proc_cache_acquire_infos(&live, &n)) {
        return false;
    }
    bool found = false;
    for (size_t i = 0; i < n && !found; i++) {
        if (live[i].pid == pid) {
            *out  = live[i];
            found = true;
        }
    }
    proc_cache_release();
    return found;
}

/* Up to `max` tasks of `pid`: its own, then its threads, with their tids. */
static size_t proc_tasks(espix_pid_t pid, TaskHandle_t own, espix_pid_t *tids,
                         TaskHandle_t *tasks, size_t max)
{
    size_t n = 0;
    if (max > 0) {
        tids[n]  = pid;
        tasks[n] = own;
        n++;
    }

    const TaskStatus_t *ts;
    UBaseType_t         got;
    if (!proc_cache_acquire_tasks(&ts, &got)) {
        return n;
    }
    for (UBaseType_t i = 0; i < got && n < max; i++) {
        espix_pid_t owner = ESPIX_PID_NONE, tid = ESPIX_PID_NONE;
        if (espix_proc_thread_of_task(ts[i].xHandle, &owner, &tid) &&
            owner == pid) {
            tids[n]  = tid;
            tasks[n] = ts[i].xHandle;
            n++;
        }
    }
    proc_cache_release();
    return n;
}

#define PROC_TASKS_MAX 16

static size_t pid_status(char *buf, size_t cap, int32_t pid)
{
    espix_proc_info_t info;
    if (!proc_info(pid, &info)) {
        return 0;
    }
    espix_pid_t  tids[PROC_TASKS_MAX];
    TaskHandle_t tasks[PROC_TASKS_MAX];
    const size_t threads = proc_tasks(pid, info.task, tids, tasks, PROC_TASKS_MAX);

    return (size_t)snprintf(
        buf, cap,
        "Name:\t%s\n"
        "Kind:\t%s\n"
        "State:\t%s\n"
        "Pid:\t%d\n"
        "PPid:\t%d\n"
        "Pgid:\t%d\n"
        "Sid:\t%d\n"
        "Uid:\t%u\n"
        "Gid:\t%u\n"
        "Threads:\t%u\n",
        info.name, kind_str(info.kind), espix_proc_state_str(info.state),
        (int)info.pid, (info.ppid > 0) ? (int)info.ppid : 0, (int)info.pgid,
        (int)info.sid, (unsigned)info.uid, (unsigned)info.gid,
        (unsigned)threads);
}

/* A thread's own: its task's name and state, and the process it belongs to. */
static size_t tid_status(char *buf, size_t cap, int32_t tid)
{
    const TaskStatus_t *ts;
    UBaseType_t         got;
    if (!proc_cache_acquire_tasks(&ts, &got)) {
        return 0;
    }
    size_t len = 0;
    for (UBaseType_t i = 0; i < got; i++) {
        espix_pid_t owner = ESPIX_PID_NONE, t = ESPIX_PID_NONE;
        if (espix_proc_thread_of_task(ts[i].xHandle, &owner, &t) && t == tid) {
            len = (size_t)snprintf(buf, cap,
                                   "Name:\t%s\nTgid:\t%d\nPid:\t%d\n",
                                   ts[i].pcTaskName, (int)owner, (int)tid);
            break;
        }
    }
    proc_cache_release();
    return len;
}

/*
 * /proc/<pid>/stat and /proc/<pid>/task/<tid>/stat.
 *
 * The field order is Linux's, and htop parses exactly the first 39: it skips
 * 23-38 with strchr() and then reads 39 (processor). Everything espix does not
 * count -- page faults, exit signal, wchan -- is 0, not invented.
 *
 * utime and stime are the task's cumulative run time in USER_HZ ticks. The
 * split into user and system is not available; all of it is reported as user,
 * which is what a CPU meter needs and what TIME+ adds up.
 */
static bool proc_task_status(TaskHandle_t task, TaskStatus_t *out)
{
    if (task == NULL) {
        return false;
    }
    const TaskStatus_t *ts;
    UBaseType_t         got;
    if (!proc_cache_acquire_tasks(&ts, &got)) {
        return false;
    }
    bool found = false;
    for (UBaseType_t i = 0; i < got; i++) {
        if (ts[i].xHandle == task) {
            *out  = ts[i];
            found = true;
            break;
        }
    }
    proc_cache_release();
    return found;
}

/* The task for a tid, and the process it belongs to. */
static bool proc_tid_task(espix_pid_t tid, TaskStatus_t *out, espix_pid_t *pid)
{
    const TaskStatus_t *ts;
    UBaseType_t         got;
    if (!proc_cache_acquire_tasks(&ts, &got)) {
        return false;
    }
    bool found = false;
    for (UBaseType_t i = 0; i < got; i++) {
        const espix_pid_t owner = espix_proc_pid_of_task(ts[i].xHandle);
        if (owner == ESPIX_PID_NONE) {
            continue;
        }
        espix_pid_t opid = ESPIX_PID_NONE, t = ESPIX_PID_NONE;
        if (owner == tid) {
            *pid   = owner;
            *out   = ts[i];
            found  = true;
            break;
        }
        if (espix_proc_thread_of_task(ts[i].xHandle, &opid, &t) && t == tid) {
            *pid   = opid;
            *out   = ts[i];
            found  = true;
            break;
        }
    }
    proc_cache_release();
    return found;
}

static char proc_state_char(const TaskStatus_t *ts)
{
    switch (ts->eCurrentState) {
    case eRunning:
    case eReady:     return 'R';
    case eBlocked:   return 'S';
    case eSuspended: return 'T';
    case eDeleted:   return 'X';
    default:         return '?';
    }
}

static size_t proc_thread_count(espix_pid_t pid, TaskHandle_t own)
{
    espix_pid_t  tids[PROC_TASKS_MAX];
    TaskHandle_t tasks[PROC_TASKS_MAX];
    return proc_tasks(pid, own, tids, tasks, PROC_TASKS_MAX);
}

static size_t proc_stat_line(char *buf, size_t cap, espix_pid_t pid,
                             const char *name, const espix_proc_info_t *info,
                             const TaskStatus_t *ts, size_t threads)
{
    char comm[ESPIX_PROC_NAME_MAX];
    strlcpy(comm, name, sizeof(comm));
    for (char *p = comm; *p != '\0'; p++) {
        if (*p == ')' || *p == '\n') {
            *p = '_';
        }
    }

    const uint64_t utime = proc_ticks((int64_t)ts->ulRunTimeCounter);
    const uint64_t start = proc_ticks(info->started_us);
    const unsigned vsize = (info->image_bytes > UINT_MAX)
                               ? UINT_MAX : (unsigned)info->image_bytes;
    const int      core  = proc_task_core(ts);

    return (size_t)snprintf(
        buf, cap,
        "%d (%s) %c %d %d %d 0 -1 0 0 0 0 0 %llu 0 0 0 20 0 %u 0 %llu %u "
        "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 %d 0 0\n",
        (int)pid, comm, proc_state_char(ts),
        (info->ppid > 0) ? (int)info->ppid : 0, (int)info->pgid, (int)info->sid,
        (unsigned long long)utime, (unsigned)threads, (unsigned long long)start,
        vsize, core);
}

static size_t pid_stat(char *buf, size_t cap, int32_t pid)
{
    espix_proc_info_t info;
    if (!proc_info(pid, &info)) {
        return 0;
    }
    TaskStatus_t ts;
    if (!proc_task_status(info.task, &ts)) {
        return 0;
    }
    return proc_stat_line(buf, cap, pid, info.name, &info, &ts,
                          proc_thread_count(pid, info.task));
}

static size_t tid_stat(char *buf, size_t cap, int32_t tid)
{
    TaskStatus_t ts;
    espix_pid_t  owner;
    if (!proc_tid_task(tid, &ts, &owner)) {
        return 0;
    }
    espix_proc_info_t info;
    if (!proc_info(owner, &info)) {
        return 0;
    }
    return proc_stat_line(buf, cap, tid, ts.pcTaskName, &info, &ts,
                          proc_thread_count(owner, info.task));
}

static size_t pid_comm(char *buf, size_t cap, int32_t pid)
{
    espix_proc_info_t info;
    if (!proc_info(pid, &info)) {
        return 0;
    }
    return (size_t)snprintf(buf, cap, "%s\n", info.name);
}

static size_t tid_comm(char *buf, size_t cap, int32_t tid)
{
    TaskStatus_t ts;
    espix_pid_t  owner;
    if (!proc_tid_task(tid, &ts, &owner)) {
        (void)owner;
        return 0;
    }
    return (size_t)snprintf(buf, cap, "%s\n", ts.pcTaskName);
}

/* Linux's NUL-separated argv block, trailing NUL included. */
static size_t pid_cmdline(char *buf, size_t cap, int32_t pid)
{
    return espix_proc_cmdline(pid, buf, cap);
}

/* The exe link's target as text; readlink() uses espix_proc_exe() directly. */
static size_t pid_exe(char *buf, size_t cap, int32_t pid)
{
    size_t n = espix_proc_exe(pid, buf, cap > 1 ? cap - 1 : 0);
    if (n > 0 && n + 1 < cap) {
        buf[n++] = '\n';
        buf[n]   = '\0';
    }
    return n;
}

/*
 * /proc/<pid>/statm: the seven numbers Linux reports, in pages.
 *
 * htop treats a missing statm as "this process cannot be read" and skips it,
 * so this is not optional in practice even though the information is thin.
 * espix knows the image size, not which of its pages are resident; reporting
 * the image as both size and resident is closer than zero for an app, and
 * kernel tasks have no image at all.
 */
static size_t proc_statm(char *buf, size_t cap, const espix_proc_info_t *info)
{
    const unsigned long pages =
        (unsigned long)((info->image_bytes + 4095) / 4096);
    return (size_t)snprintf(buf, cap, "%lu %lu 0 0 0 0 0\n", pages, pages);
}

static size_t pid_statm(char *buf, size_t cap, int32_t pid)
{
    espix_proc_info_t info;
    if (!proc_info(pid, &info)) {
        return 0;
    }
    return proc_statm(buf, cap, &info);
}

static size_t tid_statm(char *buf, size_t cap, int32_t tid)
{
    TaskStatus_t ts;
    espix_pid_t  owner;
    if (!proc_tid_task(tid, &ts, &owner)) {
        return 0;
    }
    espix_proc_info_t info;
    if (!proc_info(owner, &info)) {
        return 0;
    }
    return proc_statm(buf, cap, &info);
}

/*
 * A path below /proc, taken apart: /proc/<pid>[/status | /task[/<tid>[/status]]].
 * `what` is the part after the last number, "" for a directory itself.
 */
typedef struct {
    espix_pid_t pid;
    espix_pid_t tid;            /* NONE above /task/<tid> */
    bool        task_dir;       /* /proc/<pid>/task */
    const char *what;
} proc_path_t;

static bool parse_num(const char **p, espix_pid_t *out)
{
    char      *end = NULL;
    const long v   = strtol(*p, &end, 10);
    if (end == *p || v <= 0 || (*end != '\0' && *end != '/')) {
        return false;
    }
    *out = (espix_pid_t)v;
    *p   = end;
    return true;
}

static bool parse(const char *abs_path, proc_path_t *pp)
{
    const char *p = abs_path + 6;               /* past "/proc/" */
    *pp = (proc_path_t){ .tid = ESPIX_PID_NONE, .what = "" };

    if (!parse_num(&p, &pp->pid)) {
        return false;
    }
    if (*p == '\0') {
        return true;
    }
    p++;
    if (strncmp(p, "task", 4) != 0 || (p[4] != '\0' && p[4] != '/')) {
        pp->what = p;
        return true;
    }
    p += 4;
    pp->task_dir = true;
    if (*p == '\0') {
        return true;
    }
    p++;
    if (!parse_num(&p, &pp->tid)) {
        return false;
    }
    pp->task_dir = false;
    pp->what     = (*p == '/') ? p + 1 : "";
    return true;
}

/* Whether tid is pid itself or one of its threads. */
static bool tid_of(espix_pid_t pid, espix_pid_t tid)
{
    espix_proc_info_t info;
    if (!proc_info(pid, &info)) {
        return false;
    }
    espix_pid_t  tids[PROC_TASKS_MAX];
    TaskHandle_t tasks[PROC_TASKS_MAX];
    const size_t n = proc_tasks(pid, info.task, tids, tasks, PROC_TASKS_MAX);
    for (size_t i = 0; i < n; i++) {
        if (tids[i] == tid) {
            return true;
        }
    }
    return false;
}

/*
 * The nodes lookups hand out. An open file keeps its node, so they are a pool
 * reused least recently first rather than one scratch node.
 */
#define PROC_NODES 16

static struct {
    synth_node_t node;
    char         path[40];
    uint32_t     used;
} s_pool[PROC_NODES];
static uint32_t     s_pool_clock;
static portMUX_TYPE s_pool_lock = portMUX_INITIALIZER_UNLOCKED;

static const synth_node_t *pool_node(const char *path, synth_kind_t kind,
                                     mode_t mode,
                                     size_t (*gen_key)(char *, size_t, int32_t),
                                     int32_t key)
{
    portENTER_CRITICAL(&s_pool_lock);
    int victim = 0;
    for (int i = 0; i < PROC_NODES; i++) {
        if (strcmp(s_pool[i].path, path) == 0) {
            victim = i;
            break;
        }
        if (s_pool[i].used < s_pool[victim].used) {
            victim = i;
        }
    }
    s_pool[victim].used = ++s_pool_clock;
    strlcpy(s_pool[victim].path, path, sizeof(s_pool[victim].path));
    s_pool[victim].node = (synth_node_t){
        .path = s_pool[victim].path, .kind = kind, .mode = mode,
        .gen_key = gen_key, .key = key,
    };
    portEXIT_CRITICAL(&s_pool_lock);
    return &s_pool[victim].node;
}

static const synth_node_t *proc_dyn_lookup(const char *abs_path)
{
    proc_path_t pp;
    if (!parse(abs_path, &pp)) {
        return NULL;
    }
    const bool is_status  = strcmp(pp.what, "status") == 0;
    const bool is_stat    = strcmp(pp.what, "stat") == 0;
    const bool is_statm   = strcmp(pp.what, "statm") == 0;
    const bool is_comm    = strcmp(pp.what, "comm") == 0;
    const bool is_cmdline = strcmp(pp.what, "cmdline") == 0;
    const bool is_exe     = strcmp(pp.what, "exe") == 0;
    if (!is_status && !is_stat && !is_statm && !is_comm &&
        !is_cmdline && !is_exe) {
        return NULL;
    }

    if (pp.tid != ESPIX_PID_NONE) {
        if (!tid_of(pp.pid, pp.tid)) {
            return NULL;
        }
        const bool main_task = (pp.tid == pp.pid);
        size_t (*gen)(char *, size_t, int32_t) =
            is_status  ? (main_task ? pid_status : tid_status)
          : is_stat    ? (main_task ? pid_stat   : tid_stat)
          : is_statm   ? tid_statm
          : is_cmdline ? pid_cmdline
          : is_exe     ? pid_exe
                       : (main_task ? pid_comm   : tid_comm);
        return pool_node(abs_path, SYNTH_TEXT, S_IFREG | 0444, gen, pp.pid);
    }

    espix_proc_info_t info;
    if (pp.task_dir || !proc_info(pp.pid, &info)) {
        return NULL;
    }
    size_t (*gen)(char *, size_t, int32_t) =
        is_status  ? pid_status
      : is_stat    ? pid_stat
      : is_statm   ? pid_statm
      : is_cmdline ? pid_cmdline
      : is_exe     ? pid_exe
                   : pid_comm;
    return pool_node(abs_path, SYNTH_TEXT, S_IFREG | 0444, gen, pp.pid);
}

static bool proc_dyn_isdir(const char *abs_path)
{
    proc_path_t pp;
    if (!parse(abs_path, &pp) || pp.what[0] != '\0') {
        return false;
    }
    if (pp.tid != ESPIX_PID_NONE) {
        return tid_of(pp.pid, pp.tid);
    }
    espix_proc_info_t info;
    return proc_info(pp.pid, &info);
}

/* Scratch nodes for listings: readdir copies the name and type out under
 * synth's lock before the next call. */
static synth_node_t s_list_node;
static char         s_list_path[40];

static const synth_node_t *list_node(const char *dir, const char *name,
                                     synth_kind_t kind, mode_t mode)
{
    snprintf(s_list_path, sizeof(s_list_path), "%s/%s", dir, name);
    s_list_node = (synth_node_t){ .path = s_list_path, .kind = kind, .mode = mode };
    return &s_list_node;
}

/* /proc itself: one directory per live process, after the static files. */
static const synth_node_t *proc_dyn_at(int nth)
{
    const espix_proc_info_t *live;
    size_t                   n;
    if (!proc_cache_acquire_infos(&live, &n)) {
        return NULL;
    }
    const synth_node_t *out = NULL;
    if (nth >= 0 && (size_t)nth < n) {
        char num[12];
        snprintf(num, sizeof(num), "%d", (int)live[nth].pid);
        out = list_node("/proc", num, SYNTH_DIR, S_IFDIR | 0555);
    }
    proc_cache_release();
    return out;
}

static const synth_node_t *proc_dyn_dir_at(const char *abs_path, int nth)
{
    proc_path_t pp;
    if (!parse(abs_path, &pp)) {
        return NULL;
    }
    if (pp.tid != ESPIX_PID_NONE) {
        switch (nth) {
        case 0:  return list_node(abs_path, "status", SYNTH_TEXT, S_IFREG | 0444);
        case 1:  return list_node(abs_path, "stat",   SYNTH_TEXT, S_IFREG | 0444);
        case 2:  return list_node(abs_path, "statm",  SYNTH_TEXT, S_IFREG | 0444);
        case 3:  return list_node(abs_path, "comm",   SYNTH_TEXT, S_IFREG | 0444);
        case 4:  return list_node(abs_path, "cmdline", SYNTH_TEXT, S_IFREG | 0444);
        case 5:  return list_node(abs_path, "exe",    SYNTH_TEXT, S_IFREG | 0444);
        default: return NULL;
        }
    }
    if (!pp.task_dir) {
        switch (nth) {
        case 0:  return list_node(abs_path, "status", SYNTH_TEXT, S_IFREG | 0444);
        case 1:  return list_node(abs_path, "stat",   SYNTH_TEXT, S_IFREG | 0444);
        case 2:  return list_node(abs_path, "statm",  SYNTH_TEXT, S_IFREG | 0444);
        case 3:  return list_node(abs_path, "comm",   SYNTH_TEXT, S_IFREG | 0444);
        case 4:  return list_node(abs_path, "cmdline", SYNTH_TEXT, S_IFREG | 0444);
        case 5:  return list_node(abs_path, "exe",    SYNTH_TEXT, S_IFREG | 0444);
        case 6:  return list_node(abs_path, "task", SYNTH_DIR, S_IFDIR | 0555);
        default: return NULL;
        }
    }

    espix_proc_info_t info;
    if (!proc_info(pp.pid, &info)) {
        return NULL;
    }
    espix_pid_t  tids[PROC_TASKS_MAX];
    TaskHandle_t tasks[PROC_TASKS_MAX];
    const size_t n = proc_tasks(pp.pid, info.task, tids, tasks, PROC_TASKS_MAX);
    if (nth < 0 || (size_t)nth >= n) {
        return NULL;
    }
    char num[12];
    snprintf(num, sizeof(num), "%d", (int)tids[nth]);
    return list_node(abs_path, num, SYNTH_DIR, S_IFDIR | 0555);
}

/*
 * A process's directory is owned by the process, not by root. htop fills its
 * USER column by stat()ing /proc/<pid>, so this is the difference between a
 * truthful listing and every row saying root.
 */
static bool proc_dyn_stat(const char *abs_path, struct stat *st)
{
    proc_path_t pp;
    if (!parse(abs_path, &pp) || pp.what[0] != '\0') {
        return false;
    }
    espix_proc_info_t info;
    if (!proc_info(pp.pid, &info)) {
        return false;
    }
    if (pp.tid != ESPIX_PID_NONE && !tid_of(pp.pid, pp.tid)) {
        return false;
    }

    memset(st, 0, sizeof(*st));
    st->st_mode    = S_IFDIR | 0555;
    st->st_nlink   = 2;
    st->st_blksize = 4096;
    st->st_uid     = info.uid;
    st->st_gid     = info.gid;
    return true;
}

static const synth_tree_t s_proc_tree = {
    .prefix     = "/proc",
    .prefix_len = 5,
    .nodes      = s_proc_nodes,
    .count      = (int)(sizeof(s_proc_nodes) / sizeof(s_proc_nodes[0])),
    .dyn_at     = proc_dyn_at,
    .dyn_lookup = proc_dyn_lookup,
    .dyn_isdir  = proc_dyn_isdir,
    .dyn_dir_at = proc_dyn_dir_at,
    .dyn_stat   = proc_dyn_stat,
};

const synth_tree_t *espix_fs_proc_tree(void)
{
    return &s_proc_tree;
}
