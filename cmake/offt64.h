/*
 * Make off_t 64 bits, for the whole firmware and for every app.
 *
 * espix's filesystems can all express a file larger than 4 GiB -- FatFs has
 * FF_LBA64, lwext4 seeks with int64_t and reports uint64_t -- and the only thing
 * that could not was the type: `st_size` is an `off_t`, and on this target
 * `off_t` is a 32-bit `long`. So a 5 GiB file listed as its low 32 bits, and
 * `/dev/sda4`, a 23 GiB partition, listed as `0`.
 *
 * This is a force-include (`-include`, applied to every translation unit by
 * `cmake/offt64.cmake`) rather than a patch to anything, because the toolchain's
 * own header leaves the door open:
 *
 *   sys/_types.h:45   #ifndef __machine_off_t_defined
 *                     typedef long _off_t;
 *                     #endif
 *   sys/_types.h:102  typedef _off_t __off_t;
 *   sys/types.h:155   typedef __off_t off_t;
 *
 * Defining the guard and the type here means the toolchain's long `_off_t` is
 * never declared, `__off_t` takes this one, and `off_t` follows. Nothing is
 * edited, so nothing drifts: a toolchain bump either keeps this working or fails
 * the assertion in components/espix_fs/fs.c, which is the point of having it.
 *
 * The alternative -- editing the newlib headers under ~/.espressif/tools -- would
 * change off_t for every project on the machine, which is not espix's to do.
 *
 * Two consequences worth knowing, both in docs/ROADMAP.md. `struct stat` changes
 * size, so this is an ABI decision and not a preference: an app built without it
 * would disagree with the kernel about every stat. And `fseek`/`ftell` take a
 * `long` and stay 32-bit -- newlib's, and unchangeable here -- so a transfer that
 * seeks with them cannot pass 4 GiB however wide off_t is; espix's SFTP path uses
 * lseek for that reason.
 */
#pragma once

/*
 * Nothing in this header is for an assembler, and a force-included header reaches
 * one wherever a `.S` file is preprocessed -- which is how this failed the first
 * time, with every `typedef` in machine/_default_types.h read as an opcode. The
 * cmake include now scopes the flag to C and C++; this is the second guard, so
 * that a translation unit which *is* assembly but still sees it stays harmless.
 */
#ifndef __ASSEMBLER__

#include <stdint.h>

#define __machine_off_t_defined 1
typedef int64_t _off_t;

#endif /* __ASSEMBLER__ */
