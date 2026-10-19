/* A U-mode app: one R-X segment at 0x10000, one RW segment page-aligned after it. */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
    tls  PT_TLS;
}

SECTIONS
{
    . = 0x00010000;

    .text : {
        KEEP(*(.text.init.enter))
        *(.text.unlikely .text.unlikely.*)
        *(.text.startup .text.startup.*)
        *(.text .text.* .gnu.linkonce.t.*)
    } :text

    .rodata : {
        *(.rodata .rodata.* .gnu.linkonce.r.*)
        *(.srodata .srodata.*)
        . = ALIGN(8);
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } :text

    . = ALIGN(4096);

    .data : {
        *(.data .data.* .gnu.linkonce.d.*)
        . = ALIGN(8);
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.* .sdata2.*)
    } :data

    .tdata : ALIGN_WITH_INPUT {
        *(.tdata .tdata.* .gnu.linkonce.td.*)
        PROVIDE(__tdata_end = .);
    } :data :tls

    .tbss : {
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        *(.tcommon)
        PROVIDE(__tls_end = .);
    } :data :tls

    .bss : {
        *(.sbss .sbss.* .gnu.linkonce.sb.*)
        *(.bss .bss.* .gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(16);
    } :data

    .heap (NOLOAD) : {
        PROVIDE(__heap_start = .);
        . += 64K;
        PROVIDE(__heap_end = .);
    } :data

    /DISCARD/ : { *(.comment) *(.note.*) *(.riscv.attributes) }
}

PROVIDE(__tdata_source = ADDR(.tdata));
PROVIDE(__tdata_size = SIZEOF(.tdata));
PROVIDE(__tbss_size = SIZEOF(.tbss));
PROVIDE(__tbss_offset = ADDR(.tbss) - ADDR(.tdata));
PROVIDE(__tls_size = ADDR(.tbss) + SIZEOF(.tbss) - ADDR(.tdata));
PROVIDE(__tls_align = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)));
