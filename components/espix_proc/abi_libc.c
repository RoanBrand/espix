/*
 * The last few standard C names an app can reach, and why they need naming.
 *
 * An app resolves its undefined symbols against tables of {"name", &name}
 * pairs walked with strcmp -- not against libc. The firmware *contains* all of
 * libc and calls it constantly, but there is no runtime name-to-address
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
 * One definition, and it is this one
 * ----------------------------------
 * Until R-P3.1 these names were answered a layer below, by elf_loader's own
 * hand-written libc table, which is searched before anything espix registers.
 * That made the loader's example list part of the app ABI without espix having
 * chosen it, and a name in it could not be shadowed by a table entry here --
 * only by the resolver. CONFIG_ELF_LOADER_LIBC_SYMBOLS and _ESPIDF_SYMBOLS are
 * off now, and every name they answered for is published by espix, in the file
 * that owns it:
 *
 *   this file    string.h, stdio.h, time.h, setjmp.h, getopt.h, and errno and
 *                ctype as picolibc's headers reach them (__errno, _ctype_b)
 *   abi_stdio.c  stdin/stdout/stderr and the calls that use them unnamed --
 *                one set per process, via the resolver
 *   abi_fs.c     close, beside the other POSIX file calls
 *   abi_alloc.c  malloc, calloc, realloc, free -- the arena, via the resolver
 *   abi_signal.c sleep and usleep -- interruptible, via the resolver
 *   abi_exit.c   exit and friends, via the resolver
 *   abi_pthread.c  the thread surface, create/exit through the resolver so a
 *                thread belongs to its process
 *   abi_drivers.c  the libgcc soft-double helpers and ets_printf
 *   espix_net/abi.c  the whole lwIP surface, socket through select
 *
 * tools/check-abi.py enforces both directions: an entry here that something
 * searched earlier already answers for fails the build, and so does a name the
 * loader's tables answered for that espix does not publish now that they are
 * off.
 *
 * sleep and usleep go through the resolver rather than a table: espix overrides
 * them so a signal can cut a long one short, and the resolver is searched before
 * every table, espix's included. See abi_signal.c.
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

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"

#include "esp_elf.h"

#include "espix_kernel.h"
#include "espix_proc_priv.h"

#define TAG "abi"

/*
 * sysconf(), for the few names a program asks about and espix can answer.
 *
 * _SC_OPEN_MAX is the one that matters: a program sizing a poll() or select()
 * array asks for it first, and the answer has to be the descriptor space it
 * will actually pass, which is FD_SETSIZE -- the same bound the kernel's
 * select() and poll() check against. mux_poll.c in Mosquitto allocates its
 * pollfd array from exactly this.
 *
 * _SC_CLK_TCK is the kernel's own tick, so a program that converts seconds to
 * ticks and one that prints a tick count agree.
 */
static long espix_sysconf(int name)
{
    switch (name) {
    case _SC_OPEN_MAX:
        return FD_SETSIZE;
    case _SC_CLK_TCK:
        return CONFIG_FREERTOS_HZ;
    case _SC_NPROCESSORS_ONLN:
    case _SC_NPROCESSORS_CONF:
        return portNUM_PROCESSORS;
    case _SC_PAGESIZE:
        return 4096;
    default:
        errno = EINVAL;
        return -1;
    }
}

/* The calling thread's errno: picolibc's own is thread-local, so &errno here is
 * the caller's. */
static int *abi_errno(void)
{
    return &errno;
}

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
    ESP_ELFSYM_EXPORT(setvbuf),
    ESP_ELFSYM_EXPORT(setbuf),
    /* fflush is not here: abi_cxx.cpp publishes it, that table is registered
     * first, and both names point at libc's -- so this entry was unreachable.
     * The C++ side is where it earns its place (iostream flushing). */
    ESP_ELFSYM_EXPORT(clearerr),
    /* Input, and parsing what was read. `fgetc` and `getc` are the same call
     * under two names and both are listed because both are what code says; the
     * plain pair reads espix's stdin, which the SSH channel gives an app. */
    ESP_ELFSYM_EXPORT(fgetc),
    ESP_ELFSYM_EXPORT(getc),
    ESP_ELFSYM_EXPORT(ungetc),
    ESP_ELFSYM_EXPORT(sscanf),

    /* string.h -- strcmp, strlen, strchr, strrchr, strncat, memset and memcpy
     * were published and their obvious neighbours were not. */
    ESP_ELFSYM_EXPORT(strcpy),
    ESP_ELFSYM_EXPORT(strncpy),
    ESP_ELFSYM_EXPORT(strncmp),
    /* strings.h, not string.h, which is why they were missed: the engine's
     * case-insensitive compares land here. */
    ESP_ELFSYM_EXPORT(strcasecmp),
    ESP_ELFSYM_EXPORT(strncasecmp),
    ESP_ELFSYM_EXPORT(strcat),
    ESP_ELFSYM_EXPORT(strstr),
    /* strdup is not here: abi_alloc.c answers for it through the resolver, which
     * is searched first, because an app's allocation has to be one espix can
     * find again. The checker caught the duplicate this created. */
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
     * table is also what pulls them out of libc -- nothing else in the
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
    /* qsort's other half, and the two that need no explanation. `abort` is not
     * listed here: it is claimed by abi_exit.c through the resolver, because the
     * firmware's abort() panics and reboots and an app's assert() reached it. A
     * table entry would be dead anyway -- the resolver is searched first. */
    ESP_ELFSYM_EXPORT(bsearch),
    ESP_ELFSYM_EXPORT(rand),
    ESP_ELFSYM_EXPORT(srand),
    ESP_ELFSYM_EXPORT(strtoul),
    ESP_ELFSYM_EXPORT(strtoll),
    ESP_ELFSYM_EXPORT(strtoull),
    ESP_ELFSYM_EXPORT(atof),
    ESP_ELFSYM_EXPORT(vsprintf),

    /*
     * Deliberately not here, and worth saying where the next person will look:
     * isatty() is unimplemented in IDF -- an alias for syscall_not_implemented
     * -- and atexit() has nothing that runs a dying app's handlers. Each would
     * load and then answer ENOSYS or silently do nothing, which is the case
     * abi_fs.c's note on access() argues against.
     *
     * dup() and dup2() used to be on that list. They are still not in this
     * table -- IDF answers for neither -- but abi_fs.c claims both, and fcntl,
     * through the resolver, which is the only seam that can shadow a name the
     * loader's libc table already defines.
     *
     * perror() is not here: like printf and puts, it writes to a standard
     * stream it does not name, so abi_stdio.c answers it per process. Published
     * here once, it wrote to the firmware's own stderr and the line never
     * reached the app's user.
     *
     * ctype.h needs nothing beyond `_ctype_b` above: picolibc implements it as
     * macros over that table.
     *
     * putenv() is not here either: abi_env.c claims it through the resolver,
     * which is searched before any table, so the entry that used to be here was
     * dead. A name published twice reads as a promise and resolves to whichever
     * ran first -- which is the class of bug tools/check-abi.py cannot see yet.
     *
     * exit(), _Exit(), _exit(), abort() and __assert_func are not here either:
     * they are claimed by abi_exit.c through the resolver, which is searched
     * before every table, because a dying app's streams and slot have to go
     * through espix.
     */

    /*
     * What elf_loader's own libc table used to answer for, absorbed here so
     * that espix is the single definition (R-P3.1). Every name below is one an
     * app reached through that table, so dropping one turns a working app into
     * "undefined symbol" at load; check-abi fails the build if one is missing.
     * They pass the same allowlist test as the rest of this table -- pure
     * computation, or libc that enters espix's own VFS -- and the few that
     * espix owns more tightly (malloc/calloc/realloc/free, sleep/usleep, exit)
     * are already claimed through the resolver and are not repeated.
     */
    /* errno and ctype, as an app compiled against picolibc reaches them: errno
     * through __errno(), which cmake/espix-app.cmake asks for because the loader
     * cannot relocate a thread-local access, and the ctype macros by indexing
     * _ctype_b. */
    { "__errno", (void *)&abi_errno },
    ESP_ELFSYM_EXPORT(_ctype_b),
    ESP_ELFSYM_EXPORT(toupper),
    ESP_ELFSYM_EXPORT(tolower),

    /* string.h -- the half that came from below. */
    ESP_ELFSYM_EXPORT(strerror),
    ESP_ELFSYM_EXPORT(strlen),
    ESP_ELFSYM_EXPORT(strcmp),
    ESP_ELFSYM_EXPORT(strchr),
    ESP_ELFSYM_EXPORT(strrchr),
    ESP_ELFSYM_EXPORT(strcspn),
    ESP_ELFSYM_EXPORT(strncat),
    ESP_ELFSYM_EXPORT(memcpy),
    ESP_ELFSYM_EXPORT(memset),
    ESP_ELFSYM_EXPORT(strtol),
    ESP_ELFSYM_EXPORT(strtod),

    /* stdio.h -- the output half; the input half is already above. */
    ESP_ELFSYM_EXPORT(fprintf),
    ESP_ELFSYM_EXPORT(vfprintf),
    ESP_ELFSYM_EXPORT(fputc),
    ESP_ELFSYM_EXPORT(fputs),
    ESP_ELFSYM_EXPORT(fwrite),
    /* freopen reaches the same VFS fopen does, so it needs no wrapper. */
    ESP_ELFSYM_EXPORT(freopen),

    /* time.h, getopt.h and setjmp.h. */
    ESP_ELFSYM_EXPORT(clock_gettime),
    ESP_ELFSYM_EXPORT(strftime),
    ESP_ELFSYM_EXPORT(getopt_long),
    ESP_ELFSYM_EXPORT(optind),
    ESP_ELFSYM_EXPORT(opterr),
    ESP_ELFSYM_EXPORT(optarg),
    ESP_ELFSYM_EXPORT(optopt),
    ESP_ELFSYM_EXPORT(setjmp),
    ESP_ELFSYM_EXPORT(longjmp),

    /*
     * Two more names an app reaches for that answer something espix owns: the
     * size of the descriptor space, and esp_libc's random source (which is a
     * real one on both targets, not a fallback PRNG). sysconf is a table entry
     * because it is espix's answer rather than libc's stub.
     */
    { "sysconf", (void *)espix_sysconf },
    ESP_ELFSYM_EXPORT(getrandom),

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
