/*
 * Put global C++ constructors somewhere espix's ELF loader can cope with.
 *
 * The loader resolves an address only if it falls inside one of five sections
 * it recognises by name -- .text, .data, .rodata, .data.rel.ro, .bss. A
 * constructor pointer normally lands in .ctors, which is not one of them, so
 * esp_elf_map_sym() returns 0 for it and esp_elf_arch_relocate() writes through
 * that null. The app dies during relocation, before its first instruction.
 *
 * Folding .ctors into .data fixes it at the source: the relocation then targets
 * a section the loader tracks. The bounding symbols give the app's startup shim
 * a way to find the constructors, which .ctors otherwise lacks -- the toolchain
 * locates it via crtbegin.o/crtend.o markers that -nostdlib never links.
 *
 * The trailing *(.data .data.*) matters: an app's own initialised globals must
 * end up in this same output section, or there would be two called .data and
 * the loader, which keeps one slot per name, would silently load only one.
 *
 * INSERT augments the default linker script rather than replacing it.
 */
SECTIONS {
  .data : {
    __espix_ctors_start = .;
    KEEP(*(SORT(.ctors.*)))
    KEEP(*(.ctors))
    __espix_ctors_end = .;
    *(.data .data.*)
  }
} INSERT AFTER .bss;
