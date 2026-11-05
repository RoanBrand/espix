/*
 * /sys: the Linux-shaped hardware view htop and libsensors expect.
 *
 * It is deliberately small. htop counts CPUs by listing
 * /sys/devices/system/cpu, so the two directories are what make it draw two
 * meters instead of one; it reads cpufreq/scaling_cur_freq when present and
 * falls back to /proc/cpuinfo otherwise. The thermal zone is the one file
 * libsensors-for-espix reads, in Linux's millidegree format.
 *
 * Nothing here is stored. Directories are answered positionally and files are
 * generated on read, like /proc.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if CONFIG_SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif
#include "esp_private/esp_clk.h"
#include "esp_log.h"
#if CONFIG_PM_ENABLE
#include "soc/rtc.h"
#endif

#include "espix_fs_priv.h"

static const char *TAG = "sysfs";

#define SYS_NODES 8

/* The owner of the one sensor, made once at init and read under the mutex. */
static SemaphoreHandle_t s_ts_lock;
#if CONFIG_SOC_TEMP_SENSOR_SUPPORTED
static temperature_sensor_handle_t s_ts;
static bool                        s_ts_ready;
static bool                        s_ts_tried;
#endif

void espix_fs_sys_init(void)
{
    if (s_ts_lock == NULL) {
        s_ts_lock = xSemaphoreCreateMutex();
    }
}

static size_t sys_online(char *buf, size_t cap)
{
    return (size_t)snprintf(buf, cap, "1\n");
}

static size_t sys_type(char *buf, size_t cap)
{
    return (size_t)snprintf(buf, cap, "soc-thermal\n");
}

static size_t sys_temp(char *buf, size_t cap)
{
#if CONFIG_SOC_TEMP_SENSOR_SUPPORTED
    if (s_ts_lock != NULL) {
        xSemaphoreTake(s_ts_lock, portMAX_DELAY);
    }
    if (!s_ts_ready && !s_ts_tried) {
        s_ts_tried = true;
        const temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &s_ts) != ESP_OK ||
            temperature_sensor_enable(s_ts) != ESP_OK) {
            s_ts = NULL;
            ESP_LOGW(TAG, "no temperature sensor; /sys/class/thermal stays empty");
        } else {
            s_ts_ready = true;
        }
    }
    float celsius = 0;
    const bool ok = s_ts_ready &&
                    temperature_sensor_get_celsius(s_ts, &celsius) == ESP_OK;
    if (s_ts_lock != NULL) {
        xSemaphoreGive(s_ts_lock);
    }
    if (!ok) {
        return 0;
    }
    return (size_t)snprintf(buf, cap, "%d\n", (int)(celsius * 1000.0f));
#else
    (void)buf;
    (void)cap;
    return 0;
#endif
}

static size_t sys_freq(char *buf, size_t cap)
{
#if CONFIG_PM_ENABLE
    /* esp_clk_cpu_freq() is the ROM's cached ticks-per-us, which DFS does not
     * necessarily refresh on this target.  The RTC clock configuration is the
     * source of truth for what the CPU is running at now. */
    rtc_cpu_freq_config_t cfg;
    rtc_clk_cpu_freq_get_config(&cfg);
    return (size_t)snprintf(buf, cap, "%u\n", (unsigned)(cfg.freq_mhz * 1000));
#else
    int hz = esp_clk_cpu_freq();
    if (hz <= 0) {
        hz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000;
    }
    return (size_t)snprintf(buf, cap, "%d\n", hz / 1000);
#endif
}

static size_t sys_freq_max(char *buf, size_t cap)
{
    return (size_t)snprintf(buf, cap, "%d\n", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000);
}

/* ------------------------------------------------------------ nodes */

static struct {
    synth_node_t node;
    char         path[64];
    uint32_t     used;
} s_pool[SYS_NODES];
static uint32_t     s_clock;
static portMUX_TYPE s_pool_lock = portMUX_INITIALIZER_UNLOCKED;

static const synth_node_t *sys_node(const char *path, size_t (*gen)(char *, size_t))
{
    portENTER_CRITICAL(&s_pool_lock);
    int victim = 0;
    for (int i = 0; i < SYS_NODES; i++) {
        if (strcmp(s_pool[i].path, path) == 0) {
            victim = i;
            break;
        }
        if (s_pool[i].used < s_pool[victim].used) {
            victim = i;
        }
    }
    s_pool[victim].used = ++s_clock;
    strlcpy(s_pool[victim].path, path, sizeof(s_pool[victim].path));
    s_pool[victim].node = (synth_node_t){
        .path = s_pool[victim].path, .kind = SYNTH_TEXT,
        .mode = S_IFREG | 0444, .gen = gen,
    };
    portEXIT_CRITICAL(&s_pool_lock);
    return &s_pool[victim].node;
}

/* Scratch for a directory entry; synth copies the name before the next call. */
static synth_node_t s_list_node;
static char         s_list_path[64];

static const synth_node_t *list_node(const char *dir, const char *name, synth_kind_t kind)
{
    snprintf(s_list_path, sizeof(s_list_path), "%s/%s", dir, name);
    s_list_node = (synth_node_t){ .path = s_list_path, .kind = kind,
                                  .mode = (kind == SYNTH_DIR) ? (S_IFDIR | 0555)
                                                              : (S_IFREG | 0444) };
    return &s_list_node;
}

static bool at(const char *p, const char *s)
{
    return strcmp(p, s) == 0;
}

/* /sys/devices/system/cpu/cpuN, then whatever follows the number. */
static bool cpu_path(const char *abs_path, int *cpu, const char **rest)
{
    static const char head[] = "/sys/devices/system/cpu/cpu";
    if (strncmp(abs_path, head, sizeof(head) - 1) != 0) {
        return false;
    }
    const char *p = abs_path + sizeof(head) - 1;
    char       *end = NULL;
    const long  v   = strtol(p, &end, 10);
    if (end == p || v < 0 || v >= portNUM_PROCESSORS ||
        (*end != '\0' && *end != '/')) {
        return false;
    }
    *cpu  = (int)v;
    *rest = end;
    return true;
}

static bool sys_isdir(const char *abs_path)
{
    if (at(abs_path, "/sys") || at(abs_path, "/sys/devices") ||
        at(abs_path, "/sys/devices/system") ||
        at(abs_path, "/sys/devices/system/cpu") ||
        at(abs_path, "/sys/class") || at(abs_path, "/sys/class/thermal") ||
        at(abs_path, "/sys/class/thermal/thermal_zone0")) {
        return true;
    }
    int         cpu;
    const char *rest;
    if (!cpu_path(abs_path, &cpu, &rest)) {
        return false;
    }
    (void)cpu;
    return rest[0] == '\0' || at(rest, "/cpufreq");
}

static const synth_node_t *sys_root_at(int nth)
{
    switch (nth) {
    case 0:  return list_node("/sys", "devices", SYNTH_DIR);
    case 1:  return list_node("/sys", "class", SYNTH_DIR);
    default: return NULL;
    }
}

static const synth_node_t *sys_dir_at(const char *abs_path, int nth)
{
    if (at(abs_path, "/sys")) {
        return sys_root_at(nth);
    }
    if (at(abs_path, "/sys/devices")) {
        return (nth == 0) ? list_node(abs_path, "system", SYNTH_DIR) : NULL;
    }
    if (at(abs_path, "/sys/devices/system")) {
        return (nth == 0) ? list_node(abs_path, "cpu", SYNTH_DIR) : NULL;
    }
    if (at(abs_path, "/sys/devices/system/cpu")) {
        if (nth < 0 || nth >= portNUM_PROCESSORS) {
            return NULL;
        }
        char name[16];
        snprintf(name, sizeof(name), "cpu%d", nth);
        return list_node(abs_path, name, SYNTH_DIR);
    }
    if (at(abs_path, "/sys/class")) {
        return (nth == 0) ? list_node(abs_path, "thermal", SYNTH_DIR) : NULL;
    }
    if (at(abs_path, "/sys/class/thermal")) {
        return (nth == 0) ? list_node(abs_path, "thermal_zone0", SYNTH_DIR) : NULL;
    }
    if (at(abs_path, "/sys/class/thermal/thermal_zone0")) {
        switch (nth) {
        case 0:  return list_node(abs_path, "type", SYNTH_TEXT);
        case 1:  return list_node(abs_path, "temp", SYNTH_TEXT);
        default: return NULL;
        }
    }

    int         cpu;
    const char *rest;
    if (!cpu_path(abs_path, &cpu, &rest)) {
        return NULL;
    }
    (void)cpu;
    if (rest[0] == '\0') {
        switch (nth) {
        case 0:  return list_node(abs_path, "online", SYNTH_TEXT);
        case 1:  return list_node(abs_path, "cpufreq", SYNTH_DIR);
        default: return NULL;
        }
    }
    if (at(rest, "/cpufreq")) {
        switch (nth) {
        case 0:  return list_node(abs_path, "scaling_cur_freq", SYNTH_TEXT);
        case 1:  return list_node(abs_path, "cpuinfo_max_freq", SYNTH_TEXT);
        default: return NULL;
        }
    }
    return NULL;
}

static const synth_node_t *sys_lookup(const char *abs_path)
{
    if (at(abs_path, "/sys/class/thermal/thermal_zone0/type")) {
        return sys_node(abs_path, sys_type);
    }
    if (at(abs_path, "/sys/class/thermal/thermal_zone0/temp")) {
        return sys_node(abs_path, sys_temp);
    }

    int         cpu;
    const char *rest;
    if (!cpu_path(abs_path, &cpu, &rest)) {
        return NULL;
    }
    (void)cpu;
    if (at(rest, "/online")) {
        return sys_node(abs_path, sys_online);
    }
    if (at(rest, "/cpufreq/scaling_cur_freq")) {
        return sys_node(abs_path, sys_freq);
    }
    if (at(rest, "/cpufreq/cpuinfo_max_freq")) {
        return sys_node(abs_path, sys_freq_max);
    }
    return NULL;
}

static const synth_tree_t s_sys_tree = {
    .prefix     = "/sys",
    .prefix_len = 4,
    .nodes      = NULL,
    .count      = 0,
    .dyn_at     = sys_root_at,
    .dyn_lookup = sys_lookup,
    .dyn_isdir  = sys_isdir,
    .dyn_dir_at = sys_dir_at,
};

const synth_tree_t *espix_fs_sys_tree(void)
{
    return &s_sys_tree;
}
