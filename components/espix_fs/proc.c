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
#include <string.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "espix_kernel.h"

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
    { "/proc/meminfo", SYNTH_TEXT, S_IFREG | 0444, 0, 0, proc_meminfo },
    { "/proc/cpuinfo", SYNTH_TEXT, S_IFREG | 0444, 0, 0, proc_cpuinfo },
    { "/proc/uptime",  SYNTH_TEXT, S_IFREG | 0444, 0, 0, proc_uptime },
    { "/proc/version", SYNTH_TEXT, S_IFREG | 0444, 0, 0, proc_version },
};

static const synth_tree_t s_proc_tree = {
    .prefix     = "/proc",
    .prefix_len = 5,
    .nodes      = s_proc_nodes,
    .count      = (int)(sizeof(s_proc_nodes) / sizeof(s_proc_nodes[0])),
    .dyn_at     = NULL,
    .dyn_lookup = NULL,
};

const synth_tree_t *espix_fs_proc_tree(void)
{
    return &s_proc_tree;
}
