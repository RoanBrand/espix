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
 * `cmake/offt64.cmake`) rather than a patch to anything, because picolibc's
 * headers leave the door open: every one that declares off_t does so as
 *
 *   #ifndef _OFF_T_DECLARED
 *   typedef __off_t off_t;
 *   #define _OFF_T_DECLARED
 *   #endif
 *
 * It is glibc's large-file scheme (_FILE_OFFSET_BITS=64) on a libc that has no
 * switch for it, and it splits the type the same way glibc does:
 *
 *   off_t     the public type, 64 bits. struct stat, lseek, pread, pwrite and
 *             ftruncate are declared with it, and all of those are implemented
 *             in source this build compiles -- IDF's VFS and espix -- so caller
 *             and callee agree.
 *
 *   __off_t   libc's internal type, left at the 32 bits the *prebuilt* picolibc
 *             was compiled with. Every callback that crosses into it is declared
 *             with this one: a FILE's seek, funopen's seekfn, bufio's lseek, and
 *             fseeko/ftello. Widening it too would make each of those a call
 *             that passes 32 bits where 64 are read -- type-correct and silently
 *             wrong, which is how fseek() once landed past the end of a file
 *             during an SFTP upload.
 *
 * And lseek, like glibc's, is redirected: every caller compiled with this header
 * reaches the 64-bit `lseek64` (components/espix_fs/fs.c), while the plain
 * `lseek` symbol stays the 32-bit one that the prebuilt fdopen/fseek bind to by
 * name. That one is IDF's, in esp_libc, which is why esp_libc alone is compiled
 * without this header -- it *is* the libc glue, so it keeps libc's width. See
 * cmake/offt64.cmake.
 *
 * Two consequences worth knowing. `struct stat` is laid out identically at either
 * width (st_size sits at an 8-byte-aligned offset either way), so this is not
 * where an app and the kernel can disagree -- but an app built without it would
 * still pass a 32-bit offset to a 64-bit lseek. And fseeko/ftello stay 32-bit,
 * being the prebuilt libc's, so a transfer that seeks a FILE cannot pass 4 GiB;
 * espix's SFTP path uses lseek for that reason.
 */
#pragma once

/*
 * Nothing in this header is for an assembler, and a force-included header reaches
 * one wherever a `.S` file is preprocessed -- which is how this failed the first
 * time, with every `typedef` in machine/_default_types.h read as an opcode. The
 * cmake include now scopes the flag to C and C++; this is the second guard, so
 * that a translation unit which *is* assembly but still sees it stays harmless.
 */
#if !defined(__ASSEMBLER__) && !defined(ESPIX_LIBC_OWN_OFF_T)

/*
 * Deliberately no libc header here: one included this early fixes which libc's
 * sys/_types.h wins before the toolchain's own include order has had its say.
 * The compiler's own int64 type needs no header, and the width is verified after
 * a normal <sys/types.h> by the static assertions in components/espix_fs/fs.c
 * and components/espix_kernel/kernel.c.
 */
typedef __INT64_TYPE__ off_t;
#define _OFF_T_DECLARED

#ifdef __cplusplus
extern "C" {
#endif

off_t lseek(int fd, off_t offset, int whence) __asm__("lseek64");

#ifdef __cplusplus
}
#endif

#endif /* !__ASSEMBLER__ && !ESPIX_LIBC_OWN_OFF_T */
