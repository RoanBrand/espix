/*
 * C++ runtime surface for loadable apps.
 *
 * espix is a C project, so nothing here is for espix's own benefit. An app
 * written in C++ — an Arduino sketch, say — emits calls to operator new and
 * operator delete that the ELF loader must resolve at load time, and its
 * built-in tables cover only libc and a slice of ESP-IDF. Without these, any
 * app that allocates an object fails to load.
 *
 * operator new and delete are defined here rather than taken from libstdc++,
 * which ESP-IDF does link. Nothing in the firmware references them, so the
 * library's versions are never pulled in and these are the only definitions —
 * which is what we want: they go straight to malloc, with none of the
 * exception-throwing behaviour the standard versions carry. Everything else
 * C++ might expect — exceptions, RTTI, iostreams — stays unavailable, which is
 * the right default on a device.
 *
 * Where IDF already has a better implementation, it is exported rather than
 * reimplemented; see the note on __cxa_guard_* below.
 *
 * This is the only C++ translation unit in espix, and it exists to keep C++
 * out of the rest of it.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_elf.h"

#include "espix_kernel.h"
#include "espix_proc.h"

#define TAG "abi"

/* ------------------------------------------------------------------ */
/* Definitions                                                         */
/* ------------------------------------------------------------------ */

void *operator new(size_t size)
{
    /* Nothrow by construction: exceptions are disabled, so a caller that gets
     * NULL back is what a failed allocation looks like. */
    return malloc(size != 0 ? size : 1);
}

void *operator new[](size_t size)
{
    return malloc(size != 0 ? size : 1);
}

void operator delete(void *p) noexcept
{
    free(p);
}

void operator delete[](void *p) noexcept
{
    free(p);
}

/* Sized deallocation, which is what GCC actually emits for a `delete` whose
 * type it knows — the unsized form above is never called by such code. */
void operator delete(void *p, size_t size) noexcept
{
    (void)size;
    free(p);
}

void operator delete[](void *p, size_t size) noexcept
{
    (void)size;
    free(p);
}

/*
 * Reached only if a pure virtual is called during construction or destruction,
 * which is undefined behaviour in C++. Aborting the app beats returning into a
 * vtable slot that does not exist.
 */
extern "C" void __cxa_pure_virtual(void)
{
    espix_klog(ESPIX_KLOG_ERROR, TAG, "app called a pure virtual function");
    abort();
}

/*
 * Guards for function-local statics are NOT defined here. ESP-IDF's `cxx`
 * component already provides them (components/cxx/cxx_guards.cpp), and its
 * versions block on a FreeRTOS mutex so two tasks racing into the same static
 * initialiser behave correctly. Defining our own produced a duplicate-symbol
 * link failure, which was the right answer from the linker: only the export
 * below is wanted.
 */
extern "C" int  __cxa_guard_acquire(void *guard);
extern "C" void __cxa_guard_release(void *guard);
extern "C" void __cxa_guard_abort(void *guard);

/*
 * Registration of destructors for objects with static storage duration. An
 * espix app's image is torn down wholesale when the process ends, so there is
 * nothing to unwind: accepting and ignoring is honest, and refusing would fail
 * the load of any app with a global object.
 */
extern "C" int __cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    (void)fn;
    (void)arg;
    (void)dso;
    return 0;
}

/*
 * std::function calls this when invoked while empty. Arduino's HAL uses
 * std::function, so an app links against it even when the path is never taken.
 * Exceptions are disabled, so there is nothing to throw: aborting names the
 * fault instead of returning into a function that is not there.
 */
extern "C" void _ZSt25__throw_bad_function_callv(void);
extern "C" void _ZSt25__throw_bad_function_callv(void)
{
    espix_klog(ESPIX_KLOG_ERROR, TAG, "app called an empty std::function");
    abort();
}

/* ------------------------------------------------------------------ */
/* Export                                                              */
/* ------------------------------------------------------------------ */

/*
 * Spelled with mangled names because that is what an app's object file
 * references, exactly as espix_net's table uses lwip_-prefixed names for the
 * same reason. ESP_ELFSYM_EXPORT() cannot spell `operator new`, so the entries
 * are written out.
 */
static esp_elf_symbol_table_t s_cxx_syms[] = {
    { "_Znwj",   reinterpret_cast<const void *>(
                     static_cast<void *(*)(size_t)>(::operator new)) },
    { "_Znaj",   reinterpret_cast<const void *>(
                     static_cast<void *(*)(size_t)>(::operator new[])) },
    { "_ZdlPv",  reinterpret_cast<const void *>(
                     static_cast<void (*)(void *) noexcept>(::operator delete)) },
    { "_ZdaPv",  reinterpret_cast<const void *>(
                     static_cast<void (*)(void *) noexcept>(::operator delete[])) },
    { "_ZdlPvj", reinterpret_cast<const void *>(
                     static_cast<void (*)(void *, size_t) noexcept>(::operator delete)) },
    { "_ZdaPvj", reinterpret_cast<const void *>(
                     static_cast<void (*)(void *, size_t) noexcept>(::operator delete[])) },

    { "__cxa_pure_virtual",  reinterpret_cast<const void *>(__cxa_pure_virtual) },
    { "__cxa_guard_acquire", reinterpret_cast<const void *>(__cxa_guard_acquire) },
    { "__cxa_guard_release", reinterpret_cast<const void *>(__cxa_guard_release) },
    { "__cxa_guard_abort",   reinterpret_cast<const void *>(__cxa_guard_abort) },
    { "__cxa_atexit",        reinterpret_cast<const void *>(__cxa_atexit) },

    /* Plain libc the loader's own table happens to omit, and which any app
     * that prints without a trailing newline needs. */
    { "fflush", reinterpret_cast<const void *>(fflush) },

    /*
     * espix_app_stopping() used to be published here -- the one way an app
     * could ask whether it had been told to stop, back when a stop was a bare
     * flag. It is gone: espix_sigcheck() answers the same question, and
     * abi_signal.c publishes it alongside the POSIX signal calls it belongs
     * with. An app that drives hardware now installs a SIGTERM handler.
     */

    { "_ZSt25__throw_bad_function_callv",
      reinterpret_cast<const void *>(_ZSt25__throw_bad_function_callv) },

    ESP_ELFSYM_END
};

extern "C" void espix_proc_abi_cxx_register(void)
{
    if (esp_elf_register_symbol(s_cxx_syms) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "could not publish the C++ runtime to apps");
        return;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "C++ runtime published to apps");
}
