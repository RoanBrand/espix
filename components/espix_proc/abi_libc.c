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
 * And a name only belongs here if the call reaches espix -- or reaches nothing.
 * libc's open(), stat() and read() are published unwrapped *because* they
 * dispatch into espix's own VFS and so meet the permission check on the way in;
 * chdir, getcwd and chmod are espix's own under libc's names because IDF's are
 * stubs; sleep and usleep are overridden so a signal can cut them short. A call
 * that would reach *below* espix -- a lower filesystem, a device, a ROM routine
 * with its own policy -- gets overridden to route through espix or is left out
 * with the reason written down. An app's fopen() used to reach the filesystem
 * without passing espix at all, which is the bug this rule exists to prevent.
 *
 * What is answered a layer below, and needs no entry here
 * ------------------------------------------------------
 * elf_loader's own table (esp_elf_symbol.c) is searched *before* this one, so
 * these are already reachable and listing them again is dead weight -- which
 * three entries were, until tools/check-abi.py started comparing the names below
 * with the names here. Grouped as that file groups them, and only the names an
 * app of espix's kind reaches for:
 *
 *   string.h  strerror, memcpy, memset, strlen, strcmp, strchr, strrchr,
 *             strcspn, strncat, strtod, strtol
 *   stdio.h   printf, fprintf, vfprintf, puts, putchar, fputc, fputs, fwrite
 *   stdlib.h  malloc, calloc, realloc, free
 *   unistd.h  close, exit, sleep, usleep
 *   time.h    clock_gettime, strftime
 *   setjmp.h  setjmp, longjmp
 *   getopt.h  getopt_long and its four variables
 *   libc      __errno, __getreent and the ctype table -- `_ctype_` under newlib,
 *             its picolibc equivalent under picolibc. These are newlib's own ABI
 *             rather than a C API, and an app built against either libc needs
 *             the matching set.
 *   pthread.h pthread_create, join, detach, exit and two attribute calls, which
 *             an app that wants a second thread uses. espix does not own these:
 *             a thread created through them is a FreeRTOS task, not a process,
 *             so it has no espix identity and no signal delivery. Worth knowing
 *             before anything promises a thread the isolation an app gets.
 *
 * sleep and usleep are the exception that proves the rule: espix does override
 * them, and it has to go through the resolver to do it, because a table cannot
 * shadow a name the loader's own is searched first for. See abi_signal.c.
 *
 * None of this can be a _Static_assert. `sizeof(&f)` proves the *declaration*
 * exists and costs nothing, but what matters is whether the *definition* is in
 * the image, and naming it to prove that is exactly what pulls it in -- the cost
 * the assertion was meant to avoid. So the check runs at the other end:
 * tools/check-abi.py reads the linked ELF and fails the build if a name
 * elf_loader promises is not in it, and compares the two name lists so that an
 * entry here which something below already answers for is caught before it is
 * flashed rather than discovered when an app gets the wrong implementation.
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
    ESP_ELFSYM_EXPORT(setbuf),
    ESP_ELFSYM_EXPORT(fflush),
    ESP_ELFSYM_EXPORT(clearerr),
    /* Input, and parsing what was read. `fgetc` and `getc` are the same call
     * under two names and both are listed because both are what code says; the
     * plain pair reads espix's stdin, which the SSH channel gives an app. */
    ESP_ELFSYM_EXPORT(fgetc),
    ESP_ELFSYM_EXPORT(getc),
    ESP_ELFSYM_EXPORT(getchar),
    ESP_ELFSYM_EXPORT(ungetc),
    ESP_ELFSYM_EXPORT(sscanf),

    /* string.h -- strcmp, strlen, strchr, strrchr, strncat, memset and memcpy
     * were published and their obvious neighbours were not. */
    ESP_ELFSYM_EXPORT(strcpy),
    ESP_ELFSYM_EXPORT(strncpy),
    ESP_ELFSYM_EXPORT(strncmp),
    ESP_ELFSYM_EXPORT(strcat),
    ESP_ELFSYM_EXPORT(strstr),
    ESP_ELFSYM_EXPORT(strdup),
    ESP_ELFSYM_EXPORT(strnlen),
    ESP_ELFSYM_EXPORT(strndup),
    ESP_ELFSYM_EXPORT(memmove),
    ESP_ELFSYM_EXPORT(memcmp),
    ESP_ELFSYM_EXPORT(memchr),
    /* Splitting and scanning. `strtok_r` is what an app should prefer, and
     * `strtok` is what it usually calls -- the same pairing as the time table's
     * localtime forms. `strspn`/`strpbrk` are the pair that strtok is written
     * out of, and the two an app reaches for when it wants the split without the
     * hidden state. */
    ESP_ELFSYM_EXPORT(strspn),
    ESP_ELFSYM_EXPORT(strpbrk),
    ESP_ELFSYM_EXPORT(strtok),
    ESP_ELFSYM_EXPORT(strtok_r),

    /* stdlib.h -- strtol and strtod were published, strtoul was not.
     *
     * The 64-bit pair goes with the widened off_t: an app that seeks or reads
     * past 4 GiB has to parse the number first, and strtol cannot hold it. This
     * table is also what pulls them out of newlib -- nothing else in the
     * firmware converts 64-bit text, so without these two the linker has no
     * reason to keep them and an app calling either fails to *load*, with the
     * failure naming a symbol rather than the gap that let it happen. */
    ESP_ELFSYM_EXPORT(atoi),
    ESP_ELFSYM_EXPORT(atol),
    ESP_ELFSYM_EXPORT(atoll),
    ESP_ELFSYM_EXPORT(abs),
    ESP_ELFSYM_EXPORT(labs),
    ESP_ELFSYM_EXPORT(llabs),
    ESP_ELFSYM_EXPORT(qsort),
    /* qsort's other half, and the two that need no explanation. `abort` is
     * already reachable -- `__assert_func` is published and calls it -- so
     * listing it adds the name, not the ability. */
    ESP_ELFSYM_EXPORT(bsearch),
    ESP_ELFSYM_EXPORT(rand),
    ESP_ELFSYM_EXPORT(srand),
    ESP_ELFSYM_EXPORT(abort),
    ESP_ELFSYM_EXPORT(strtoul),
    ESP_ELFSYM_EXPORT(strtoll),
    ESP_ELFSYM_EXPORT(strtoull),

    /*
     * Deliberately not here, and worth saying where the next person will look:
     * isatty() and dup()/dup2() are unimplemented in IDF -- isatty is an alias
     * for syscall_not_implemented -- and atexit() has nothing that runs a dying
     * app's handlers. Each would load and then answer ENOSYS or silently do
     * nothing, which is the case abi_fs.c's note on access() argues against.
     *
     * perror() was published here and then measured: it writes to the firmware's
     * own stderr stream, not the channel the app is writing to, so the line never
     * arrives. Removed on that evidence rather than kept for the name's sake --
     * strerror() is the part an app needs, and the loader already publishes it.
     *
     * ctype.h needs nothing: newlib implements it as macros over `_ctype_`, and
     * the loader publishes that table.
     */

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
