/*
 * The last few standard C names an app can reach, and why they need naming.
 *
 * An app resolves its undefined symbols against tables of {"name", &name}
 * pairs walked with strcmp -- not against libc. The firmware *contains* all of
 * newlib and calls it constantly, but there is no runtime name-to-address
 * directory, so a function is reachable only if somebody typed it into a table.
 * snprintf sits at a known address in this image and was unreachable from an
 * app until this file existed.
 *
 * Before this, apps had 138 names: 66 from elf_loader's own hand-written libc
 * table (esp_elf_symbol.c, grouped by header, covering what Espressif's
 * examples happened to need) and the rest from espix's abi_fs.c, abi_time.c and
 * abi_drivers.c. Which is why the gaps look arbitrary rather than principled --
 * `strncat` is published and `strncmp` is not, `vsnprintf` is published and
 * `snprintf` is not. Nobody weighed those; a list was written once.
 *
 * Why not generate the table instead
 * ----------------------------------
 * elf_loader ships tool/symbols.py for exactly that: point it at a firmware
 * ELF and it emits every global symbol as a C table, into g_customer_elfsyms
 * behind CONFIG_ELF_LOADER_CUSTOMER_SYMBOLS. espix leaves that off deliberately.
 *
 * On a chip with no MMU this table *is* the sandbox -- the README calls the
 * export table the real boundary, because an app shares the address space and
 * can only call what it can name. Generating it from the firmware would hand
 * every app the WiFi driver, the flash API and espix's own internals, with
 * nothing to make that harmless.
 *
 * So these tables are an allowlist, and this file extends it rather than
 * patching around a missing feature. Everything below is pure computation over
 * memory the caller already owns: no device access, no espix state, nothing
 * that could be misused in a way printf() does not already allow. A symbol that
 * cannot pass that test does not belong here.
 *
 * Found by writing an app: it wanted setvbuf and would not load, then snprintf
 * and still would not load, each time with "relocation failed" naming no
 * symbol. See docs/UPSTREAM.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_elf.h"

#include "espix_kernel.h"
#include "espix_proc_priv.h"

#define TAG "abi"

static const struct esp_elfsym s_libc_syms[] = {

    /* stdio.h -- formatting into a buffer, and control over buffering.
     *
     * printf, fprintf and vsnprintf were already published; snprintf and
     * sprintf were not, which is the gap an app finds first. setvbuf matters
     * more than it looks: without it an app cannot batch its own output at all,
     * and on an SSH session espix line-buffers at 128 bytes, so it gets one
     * packet per line whether that suits it or not. */
    ESP_ELFSYM_EXPORT(snprintf),
    ESP_ELFSYM_EXPORT(sprintf),
    ESP_ELFSYM_EXPORT(vprintf),
    ESP_ELFSYM_EXPORT(setvbuf),
    ESP_ELFSYM_EXPORT(fflush),

    /* string.h -- strcmp, strlen, strchr, strrchr, strncat, memset and memcpy
     * were published and their obvious neighbours were not. */
    ESP_ELFSYM_EXPORT(strcpy),
    ESP_ELFSYM_EXPORT(strncpy),
    ESP_ELFSYM_EXPORT(strncmp),
    ESP_ELFSYM_EXPORT(strcat),
    ESP_ELFSYM_EXPORT(strstr),
    ESP_ELFSYM_EXPORT(strdup),
    ESP_ELFSYM_EXPORT(memmove),
    ESP_ELFSYM_EXPORT(memcmp),

    /* stdlib.h -- strtol and strtod were published, strtoul was not.
     *
     * The 64-bit pair goes with the widened off_t: an app that seeks or reads
     * past 4 GiB has to parse the number first, and strtol cannot hold it. This
     * table is also what pulls them out of newlib -- nothing else in the
     * firmware converts 64-bit text, so without these two the linker has no
     * reason to keep them and an app calling either fails to *load*, with the
     * failure naming a symbol rather than the gap that let it happen. */
    ESP_ELFSYM_EXPORT(atoi),
    ESP_ELFSYM_EXPORT(abs),
    ESP_ELFSYM_EXPORT(qsort),
    ESP_ELFSYM_EXPORT(strtoul),
    ESP_ELFSYM_EXPORT(strtoll),
    ESP_ELFSYM_EXPORT(strtoull),

    ESP_ELFSYM_END
};

void espix_proc_abi_libc_register(void)
{
    if (esp_elf_register_symbol(s_libc_syms) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "could not publish libc to apps");
        return;
    }

    espix_klog(ESPIX_KLOG_DEBUG, TAG, "libc gaps published to apps");
}
