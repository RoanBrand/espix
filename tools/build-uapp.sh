#!/usr/bin/env bash
#
# Build a U-mode app for the S31: the sources, picolibc and libespix-u, linked
# at 0x10000 as a static executable. docs/USER-MODE.md.
#
#   tools/build-uapp.sh -o out.elf main.c [more.c ...] [-- extra gcc flags]
#
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)

gcc=$(command -v riscv32-esp-elf-gcc || true)
if [[ -z "$gcc" ]]; then
    gcc=$(ls -d "$HOME"/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-gcc 2>/dev/null | tail -1)
fi
[[ -x "$gcc" ]] || { echo "build-uapp: no riscv32-esp-elf-gcc" >&2; exit 1; }

out=""
srcs=()
extra=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -o) out="$2"; shift 2 ;;
        --) shift; extra=("$@"); break ;;
        *)  srcs+=("$1"); shift ;;
    esac
done
[[ -n "$out" && ${#srcs[@]} -gt 0 ]] || { echo "usage: build-uapp.sh -o out.elf src.c..." >&2; exit 2; }

# No PIE or hardware-loop extensions: both exist only on core 1 (docs/USER-MODE.md).
"$gcc" -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_zcb_zcmp_zcmt -mabi=ilp32f \
    --specs=picolibc.specs -Os -ffunction-sections -fdata-sections \
    -I"$root/components/espix_proc/include" \
    -nostartfiles -static -T "$root/user/uapp.ld" -Wl,--gc-sections \
    -o "$out" "$root/user/crt0.S" "$root/user/libespix-u.c" "${srcs[@]}" ${extra[@]+"${extra[@]}"}
