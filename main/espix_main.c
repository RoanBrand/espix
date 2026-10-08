/*
 * espix boot.
 *
 * app_main is the init sequence and nothing else. Keeping the ordering here —
 * rather than inside a kernel component that reaches sideways into the others —
 * is what keeps the component dependency graph acyclic.
 *
 * Order matters:
 *   1. kernel   — the log ring must exist before anything can report.
 *   2. fault    — before the filesystem, so a mount that panics is still
 *                 reported on the next boot.
 *   3. fs       — the rootfs, which everything below reads from.
 *   4. proc     — the process table.
 *   5. time     — after the filesystem, since it reads /etc/timezone; before
 *                 networking, because it starts the SNTP client from the
 *                 IP_EVENT that networking is about to raise.
 *   6. net      — after the filesystem, since it reads /etc/hostname and
 *                 /etc/wifi.conf. Returns immediately; association and DHCP
 *                 run on the event loop, so an absent or unreachable network
 *                 never delays the prompt.
 *   7. usb      — the OTG port as a host, when the role is host. Independent of
 *                 everything above and, like networking, done as soon as the
 *                 stack is up: an empty socket is the normal case, and devices
 *                 appear on the USB task as they are plugged in.
 *   8. commands — need the registry, and the filesystem to act on.
 *   9. console  — takes over this task and does not return.
 */

#include "esp_err.h"
#include "esp_log.h"

#include "espix_audio.h"
#include "espix_auth.h"
#include "espix_bt.h"
#if CONFIG_ESPIX_I2S
#include "espix_i2s.h"
#endif
#include "espix_cmds.h"
#include "espix_display.h"
#include "espix_fault.h"
#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_net.h"
#include "espix_ota.h"
#include "espix_proc.h"
#include "espix_svc.h"
#include "espix_shell.h"
#include "espix_ssh.h"
#include "espix_time.h"
#include "espix_usb.h"

#if CONFIG_ESP_TRACE_LIB_EXTERNAL
#include "esp_trace.h"

/*
 * The espressif/esp_sysview encoder registers itself as "sysview"
 * (ESP_TRACE_REGISTER_ENCODER in the component), but Kconfig's name for an
 * external library is the generic "ext" -- and CONFIG_ESP_TRACE_LIB_NAME has no
 * prompt, so a profile overlay cannot set it. Without this override
 * esp_trace_init() returns ESP_ERR_NOT_FOUND at boot and the image aborts in
 * ipc0 with "init function ... has failed (0x105)".
 *
 * This is the override the IDF custom-library example documents for an encoder
 * registered under a name other than the default. It is compiled only when the
 * trace library is external, which is only the PROFILE=sysview build, so a
 * normal or release build neither links esp_trace nor defines this.
 *
 * The same override names the CPU to trace. A single-stream transport can carry
 * only one CPU's scheduling, and the component's config type lives in its
 * private header (src/esp/adapter_encoder_sysview.h), so this mirrors the one
 * field the encoder reads -- it casts encoder_cfg to esp_trace_sysview_config_t
 * and takes dest_cpu first.
 */
typedef struct {
    int dest_cpu;
} espix_trace_encoder_cfg_t;

esp_trace_open_params_t esp_trace_get_user_params(void)
{
    static const espix_trace_encoder_cfg_t enc = {
        .dest_cpu = CONFIG_ESPIX_TRACE_CORE,
    };
    const esp_trace_open_params_t params = {
        .core_cfg = NULL,
        .encoder_name = "sysview",
        .encoder_cfg = &enc,
        .transport_name = CONFIG_ESP_TRACE_TRANSPORT_NAME,
        .transport_cfg = NULL,
    };
    return params;
}
#endif

#define TAG "espix"

/*
 * Give every attached storage device a name in /dev, and take the names away
 * when it goes.
 *
 * This is the only place that knows both halves: espix_fs has no idea what USB
 * is, espix_usb knows nothing about the VFS, and this file already depends on
 * both. Doing it here is what keeps those two components independent of each
 * other, which is the reason the hook exists at all.
 *
 * Per disk *and* per partition, the way every other system names them, and what
 * makes `mount /dev/sda1 /mnt` work. A superfloppy has no partitions, so its
 * disk node is the only one there is -- and mounting the disk itself is then the
 * only way to reach the filesystem on it.
 */
static void usb_dev_nodes(const espix_usb_dev_t *dev, bool attached)
{
    if (attached) {
        (void)espix_dev_register_block(dev->name, dev->size);
        for (size_t i = 0; i < dev->nparts; i++) {
            (void)espix_dev_register_block(dev->parts[i].name,
                                           dev->parts[i].size);
        }

        /* And whatever /etc/fstab says about it, if anything. */
        espix_blk_device_added(dev->name);
        return;
    }

    /*
     * Before anything else: a mount of this device is about to have its block
     * device released underneath it, so the filesystem has to be told first --
     * this is the detach half of the use-after-free Stage 2 left open.
     */
    espix_blk_device_gone(dev->name);

    /* Detach hands the row over intact -- before it is released -- so the
     * partitions are still there to be named. */
    espix_dev_unregister_block(dev->name);
    for (size_t i = 0; i < dev->nparts; i++) {
        espix_dev_unregister_block(dev->parts[i].name);
    }
}

/*
 * The shutdown sequence's hooks.
 *
 * This file is the one place that knows every component, which is why the wiring
 * is here and not in the kernel: the kernel's table knows the phases and must
 * know nothing about ssh, nfsd or the display. The phase decides the order, and
 * inside a phase it is the list below.
 *
 * Unlike the USB device hook and the session-end hook above, these are not the
 * only way to reach the feature -- espix_shutdown_reboot() is a call any command
 * can make -- but they are the only place the parts are named.
 *
 * There is no USB-host stop to call: a reset powers the port down with
 * everything else, and the volume on it was already flushed by the storage
 * phase, which is the part that mattered.
 */
static void shutdown_units(int64_t deadline_us)
{
    (void)espix_svc_quiesce(deadline_us);
}

static void shutdown_processes(int64_t deadline_us)
{
    (void)espix_proc_stop_all(deadline_us);
}

static void shutdown_storage(int64_t deadline_us)
{
    (void)espix_fs_unmount_all(deadline_us);
}

static void shutdown_display(int64_t deadline_us)
{
    (void)deadline_us;
    /* The viewer only. The canvas outlives the backend by design, and the
     * desktop may be the thing that asked for this. */
    espix_display_vnc_stop();
}

static void shutdown_audio(int64_t deadline_us)
{
    /* Bounded by what is left of the sequence's budget rather than waiting
     * forever: the decoder answers in a frame's time, and a stop that cannot
     * finish is the one thing that would hang the shutdown instead. */
    (void)espix_audio_stop_wait(espix_shutdown_remaining_ms(deadline_us));
}

static void shutdown_bluetooth(int64_t deadline_us)
{
    (void)deadline_us;
    /* After the audio, not before. An A2DP stream torn down underneath the
     * decoder is the crash this pairing exists to avoid -- the same order
     * bluetoothctl power off uses. */
    (void)espix_bt_shutdown();
}

static void shutdown_wiring(void)
{
    (void)espix_shutdown_add(ESPIX_SHUTDOWN_UNITS, shutdown_units, "units");
    (void)espix_shutdown_add(ESPIX_SHUTDOWN_APPS, shutdown_processes, "processes");
    (void)espix_shutdown_add(ESPIX_SHUTDOWN_STORAGE, shutdown_storage, "storage");
    (void)espix_shutdown_add(ESPIX_SHUTDOWN_HARDWARE, shutdown_display, "display");
    (void)espix_shutdown_add(ESPIX_SHUTDOWN_HARDWARE, shutdown_audio, "audio");
    (void)espix_shutdown_add(ESPIX_SHUTDOWN_HARDWARE, shutdown_bluetooth,
                             "bluetooth");
}

void app_main(void)
{
    espix_kernel_early_init();

    ESP_ERROR_CHECK(espix_fault_init());
    ESP_ERROR_CHECK(espix_fs_mount_root());
    ESP_ERROR_CHECK(espix_proc_init());

    /*
     * The shell ends a session; this is what ends the processes in it. Wired
     * here because espix_proc is above espix_shell and cannot call down into
     * it, and because this file is the one place that knows both.
     */
    espix_shell_set_session_end_hook(espix_proc_hangup);

    /* Before networking: SSH will authenticate against this, and it warns while
     * the shipped default password is still in place. */
    ESP_ERROR_CHECK(espix_auth_init());

    /* Reads the OTA configuration and reports which slot is running. Never
     * fatal: a board with one slot simply has nowhere to write an update. */
    ESP_ERROR_CHECK(espix_ota_init());

    /*
     * Must precede networking: this registers the IP_EVENT handler that starts
     * the SNTP client, and the address it waits for is about to arrive. Not
     * fatal either -- a wrong clock is worse than no clock only to code that
     * assumes it is right, and espix's own timestamps are monotonic.
     */
    if (espix_time_init() != ESP_OK) {
        ESP_LOGW(TAG, "system time unavailable; the clock stays at the epoch");
    }

#if CONFIG_ESPIX_AUDIO_SINK_BT_A2DP
    /*
     * Publish the A2DP source to the audio engine. Registration only -- the
     * sink allocates nothing and reports itself disconnected until a link is
     * up, so this brings neither Bluetooth nor memory up. It lives here rather
     * than in espix_bt_init() because the engine must see the sink before the
     * first play, which is not when Bluetooth is started.
     */
    (void)espix_bt_audio_sink_register();
#endif

#if CONFIG_ESPIX_AUDIO_NULL_SINK
    /*
     * The benchmark sink, for measuring the decoder with no other sink in the
     * picture. Registered after the A2DP sink so that, if both are built, the
     * real one is default and this has to be selected.
     */
    (void)espix_audio_null_sink_register();
#endif

#if CONFIG_ESPIX_I2S
    /*
     * The board's own codec. Registered after the A2DP sink, but the engine
     * prefers a connected sink, so this is what plays when no link is up --
     * and the only sink at all with Bluetooth off.
     */
    (void)espix_i2s_sink_register();
    (void)espix_i2s_source_register();
#endif

    /*
     * Audio is not reserved here on purpose: everything is loaded on demand.
     * `play` opens the decoder and allocates its buffers, and the task frees
     * them again when it finishes; Bluetooth is only brought up by `play` or by
     * bluetoothctl. The decoder used to be opened at boot to win the internal
     * memory before Bluetooth and Wi-Fi took theirs -- that mattered while the
     * BT/Wi-Fi .bss was in internal RAM, and stopped mattering once it moved to
     * PSRAM (internal now has ~78 kB free with Bluetooth connected).
     */

    /* Not fatal: no network is a perfectly usable espix. */
    const esp_err_t net_err = espix_net_init();
    if (net_err != ESP_OK) {
        ESP_LOGW(TAG, "networking unavailable: %s", esp_err_to_name(net_err));
    }

    /*
     * The other use of the OTG port, and the reason the port has a role: only
     * one of USB-NCM and USB host can have it. Also not fatal -- the board works
     * with nothing in the socket, which is how it usually is.
     */
    const esp_err_t usb_err = espix_usb_init();
    if (usb_err != ESP_OK) {
        ESP_LOGW(TAG, "usb host unavailable: %s", esp_err_to_name(usb_err));
    }

    /* Installed whether or not the stack came up: an empty socket is the normal
     * case, and a device-role build simply never hears anything. */
    espix_usb_set_dev_hook(usb_dev_nodes);

#if CONFIG_ESPIX_SSH_ENABLED
    /* Binds immediately and accepts asynchronously, so this does not wait for
     * an address; a connection simply cannot arrive until one exists. */
    if (net_err == ESP_OK && espix_ssh_start() != ESP_OK) {
        ESP_LOGW(TAG, "ssh server did not start");
    }
#endif

    espix_cmds_register_all();

    /*
     * Units last: a unit may be anything the shell can run, so everything it
     * might reach -- the filesystem, the network, the USB stack -- is already
     * up. Not fatal: a board with no units file is the normal case.
     */
    if (espix_svc_init() != ESP_OK) {
        ESP_LOGW(TAG, "service supervisor did not start");
    }

    /* After the supervisor: stopping the units is the sequence's first act, and
     * it can stop nothing that is not there yet. */
    shutdown_wiring();

    /*
     * Everything above is up, so this image is worth keeping: if the bootloader
     * is holding it as pending-verify, say so now rather than let the next reset
     * roll it back. A no-op on a normal boot. See the rollback note in
     * docs/OTA.md.
     */
    espix_ota_confirm_boot();

    /*
     * What a viewer sees when nothing else owns the screen. Registered here
     * because this is the only place that knows both the display service and
     * the shell -- the same reason the USB device-node hook lives here.
     */
    {
        static const espix_display_default_t vnc_console = {
            .start = espix_console_canvas_start,
            .stop  = espix_console_canvas_stop,
        };
        espix_display_set_default(&vnc_console);
    }

    const esp_err_t err = espix_console_session_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console session failed: %s", esp_err_to_name(err));
    }

    /* Falling out of app_main just deletes this task; the rest of the system
     * (reaper, any running apps) keeps going. */
    ESP_LOGW(TAG, "console session ended, no interactive shell remains");
}
