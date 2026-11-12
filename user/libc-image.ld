/*
 * /lib/libc.so on the S31: libc's shared code and read-only data, one R-X segment
 * at the address every U-mode app maps it at (ESPIX_UM_LIBC_VA, espix_umode.h).
 * tools/build-libc.sh links it. Nothing writable: each app's libc state is its own.
 * No __global_pointer$, so nothing is relaxed to gp, which is the app's.
 */
ENTRY(__espix_libc_image)

PHDRS
{
    text PT_LOAD FLAGS(5);
    note PT_NOTE;
}

SECTIONS
{
    . = 0x6FC00000;
    __espix_libc_image = .;

    .text : { *(.text .text.* .gnu.linkonce.t.*) } :text
    .rodata : { *(.rodata .rodata.* .srodata .srodata.* .gnu.linkonce.r.*) } :text
    .note.gnu.build-id : { *(.note.gnu.build-id) } :text :note
    __espix_libc_image_end = .;

    /* Must stay empty: writable data or a constructor would be one copy for every app. */
    .forbidden : {
        *(.data .data.* .sdata .sdata.* .bss .bss.* .sbss .sbss.* COMMON)
        *(.tdata .tdata.* .tbss .tbss.*)
        *(.init_array* .fini_array* .ctors* .dtors* .preinit_array*)
    } :text

    /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.note.GNU-stack) }
}

ASSERT(SIZEOF(.forbidden) == 0, "libc.so's shared part has writable data or a constructor")
ASSERT(__espix_libc_image_end - __espix_libc_image <= 0x400000, "libc.so is larger than its 4 MiB window")
