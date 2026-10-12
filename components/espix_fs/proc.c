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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espix_kernel.h"
#include "espix_proc.h"

#include "espix_fs_priv.h"

/* The two pools espix reports, and the same masks free uses. */
#define PROC_INTERNAL_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define PROC_PSRAM_CAPS    (MALLOC_CAP_SPIRAM)

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

    return (size_t)snprintf(
        buf, cap,
        "processor\t: 0\n"
        "model name\t: %s (rev v%d.%d)\n"
        "cores\t\t: %d\n"
        "cpu MHz\t\t: %d\n"
        "features\t:%s%s%s\n"
        "idf version\t: %s\n"
        "mac\t\t: %02x:%02x:%02x:%02x:%02x:%02x\n",
        espix_chip_model(), (int)(ci.revision / 100), (int)(ci.revision % 100),
        (int)ci.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        (ci.features & CHIP_FEATURE_WIFI_BGN) ? " wifi" : "",
        (ci.features & CHIP_FEATURE_BT) ? " bt" : "",
        (ci.features & CHIP_FEATURE_BLE) ? " ble" : "",
        esp_get_idf_version(),
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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

static size_t proc_version(char *buf, size_t cap)
{
    return (size_t)snprintf(buf, cap, "espix %s (%s) %s\n",
                            espix_version(), espix_build_id(),
                            espix_chip_model());
}

static const synth_node_t s_proc_nodes[] = {
    { "/proc/meminfo", SYNTH_TEXT, S_IFREG | 0444, 0, 0, proc_meminfo, NULL, 0 },
    { "/proc/cpuinfo", SYNTH_TEXT, S_IFREG | 0444, 0, 0, proc_cpuinfo, NULL, 0 },
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
    espix_proc_info_t *const live = calloc(CONFIG_ESPIX_PROC_MAX, sizeof(*live));
    if (live == NULL) {
        return false;
    }
    const size_t n = espix_proc_snapshot(live, CONFIG_ESPIX_PROC_MAX);
    bool found = false;
    for (size_t i = 0; i < n && !found; i++) {
        if (live[i].pid == pid) {
            *out  = live[i];
            found = true;
        }
    }
    free(live);
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

    const UBaseType_t cap = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t     *ts  = calloc(cap, sizeof(*ts));
    if (ts == NULL) {
        return n;
    }
    const UBaseType_t got = uxTaskGetSystemState(ts, cap, NULL);
    for (UBaseType_t i = 0; i < got && n < max; i++) {
        espix_pid_t owner = ESPIX_PID_NONE, tid = ESPIX_PID_NONE;
        if (espix_proc_thread_of_task(ts[i].xHandle, &owner, &tid) &&
            owner == pid) {
            tids[n]  = tid;
            tasks[n] = ts[i].xHandle;
            n++;
        }
    }
    free(ts);
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
    const UBaseType_t cap_t = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t     *ts    = calloc(cap_t, sizeof(*ts));
    if (ts == NULL) {
        return 0;
    }
    size_t            len = 0;
    const UBaseType_t got = uxTaskGetSystemState(ts, cap_t, NULL);
    for (UBaseType_t i = 0; i < got; i++) {
        espix_pid_t owner = ESPIX_PID_NONE, t = ESPIX_PID_NONE;
        if (espix_proc_thread_of_task(ts[i].xHandle, &owner, &t) && t == tid) {
            len = (size_t)snprintf(buf, cap,
                                   "Name:\t%s\nTgid:\t%d\nPid:\t%d\n",
                                   ts[i].pcTaskName, (int)owner, (int)tid);
            break;
        }
    }
    free(ts);
    return len;
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
    if (!parse(abs_path, &pp) || strcmp(pp.what, "status") != 0) {
        return NULL;
    }
    if (pp.tid != ESPIX_PID_NONE) {
        if (!tid_of(pp.pid, pp.tid)) {
            return NULL;
        }
        return pool_node(abs_path, SYNTH_TEXT, S_IFREG | 0444,
                         (pp.tid == pp.pid) ? pid_status : tid_status, pp.tid);
    }
    espix_proc_info_t info;
    if (pp.task_dir || !proc_info(pp.pid, &info)) {
        return NULL;
    }
    return pool_node(abs_path, SYNTH_TEXT, S_IFREG | 0444, pid_status, pp.pid);
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
    espix_proc_info_t *const live = calloc(CONFIG_ESPIX_PROC_MAX, sizeof(*live));
    if (live == NULL) {
        return NULL;
    }
    const size_t        n   = espix_proc_snapshot(live, CONFIG_ESPIX_PROC_MAX);
    const synth_node_t *out = NULL;
    if (nth >= 0 && (size_t)nth < n) {
        char num[12];
        snprintf(num, sizeof(num), "%d", (int)live[nth].pid);
        out = list_node("/proc", num, SYNTH_DIR, S_IFDIR | 0555);
    }
    free(live);
    return out;
}

static const synth_node_t *proc_dyn_dir_at(const char *abs_path, int nth)
{
    proc_path_t pp;
    if (!parse(abs_path, &pp)) {
        return NULL;
    }
    if (pp.tid != ESPIX_PID_NONE) {
        return (nth == 0) ? list_node(abs_path, "status", SYNTH_TEXT,
                                      S_IFREG | 0444) : NULL;
    }
    if (!pp.task_dir) {
        switch (nth) {
        case 0:  return list_node(abs_path, "status", SYNTH_TEXT, S_IFREG | 0444);
        case 1:  return list_node(abs_path, "task", SYNTH_DIR, S_IFDIR | 0555);
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

static const synth_tree_t s_proc_tree = {
    .prefix     = "/proc",
    .prefix_len = 5,
    .nodes      = s_proc_nodes,
    .count      = (int)(sizeof(s_proc_nodes) / sizeof(s_proc_nodes[0])),
    .dyn_at     = proc_dyn_at,
    .dyn_lookup = proc_dyn_lookup,
    .dyn_isdir  = proc_dyn_isdir,
    .dyn_dir_at = proc_dyn_dir_at,
};

const synth_tree_t *espix_fs_proc_tree(void)
{
    return &s_proc_tree;
}
