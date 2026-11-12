#!/usr/bin/env bash
#
# The apps' picolibc: the kernel's own source (Espressif's tag for this
# toolchain), built with espix's options rather than IDF's. docs/USER-MODE.md.
#
#   tools/build-libc.sh [esp32s3|esp32s31]
#
# -> build-libc-<target>/include, lib/libc.a     the whole library (-static)
#    build-libc-<target>/split/libc-app.a        what each app links: state and allocation
#    build-libc-<target>/split/libc.so           what every app shares: a shared object on the
#                                                S3, an image at a fixed address on the S31
#    build-libc-<target>/split/libc.syms         the shared part's names
#    build-libc-<target>/split/libc-provide.ld   the S31's: binds an app to libc.so's addresses
#
# errno is a call (__errno) and nothing is thread-local: an app's libc state is
# plain data in the app, and errno is the one variable both halves share.
#
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
target=${1:-$(head -n1 "$root/.espix/active" 2>/dev/null || echo esp32s31)}
tag=esp-1.8.10_20251204
meson_version=1.6.1

cache="$root/build-libc-src"
src="$cache/picolibc-$tag"
out="$root/build-libc-$target"

case "$target" in
    esp32s31)
        triple=riscv32-esp-elf; gname=riscv32-esp-elf-gcc; tool=riscv32-esp-elf-
        family=riscv32; cpu=riscv
        # As espix-cc builds a U-mode app.
        arch=(-march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_zcb_zcmp_zcmt -mabi=ilp32f -mno-cm-popret)
        dtors=.fini_array
        # The allocator is the app's own (nano-malloc, wrapped by libespix), not the kernel's.
        split_opts=(--own-alloc)
        ;;
    esp32s3)
        triple=xtensa-esp-elf; gname=xtensa-esp32s3-elf-gcc; tool=xtensa-esp-elf-
        family=xtensa; cpu=esp32s3
        # As espix-cc builds an app without an MMU: a shared object.
        arch=(-mlongcalls -fPIC)
        # GCC for Xtensa emits the legacy .dtors, run in name order: atexit's runner goes first.
        dtors=.dtors.00000
        split_opts=()
        ;;
    *) echo "build-libc: no target $target (esp32s31, esp32s3)" >&2; exit 1 ;;
esac

gcc=$(command -v "$gname" || ls -d "$HOME"/.espressif/tools/"$triple"/*/"$triple"/bin/"$gname" 2>/dev/null | tail -1)
[[ -x "$gcc" ]] || { echo "build-libc: no $gname" >&2; exit 1; }
bin=$(dirname "$gcc")
ninja=$(command -v ninja || ls -d "$HOME"/.espressif/tools/ninja/*/ninja 2>/dev/null | tail -1)
[[ -x "$ninja" ]] || { echo "build-libc: no ninja" >&2; exit 1; }
# IDF's Python has venv everywhere; a distribution's python3 may not (Debian splits it out).
py=python3
if eval "$("$root/tools/idf.sh" --env 2>/dev/null)" 2>/dev/null && [[ -x "${ESPIX_PYTHON:-}" ]]; then
    py=$ESPIX_PYTHON
fi

# One build at a time: espix-cc runs this on every link. mkdir is atomic on macOS and Linux alike.
lock="$out.lock"
mkdir -p "$(dirname "$out")"
for _ in $(seq 1 600); do
    mkdir "$lock" 2>/dev/null && break
    sleep 1
done
[[ -d "$lock" ]] || { echo "build-libc: $lock is held; remove it if no build is running" >&2; exit 1; }
trap 'rmdir "$lock" 2>/dev/null' EXIT

# The library, rebuilt when this script changes.
stamp="$out/.built-$tag"
if [[ ! -f "$stamp" || ! -f "$out/lib/libc.a" || "$0" -nt "$stamp" ]]; then
    mkdir -p "$cache"
    if [[ ! -d "$src" ]]; then
        echo "build-libc: fetching picolibc $tag"
        git clone -q --depth 1 --branch "$tag" https://github.com/espressif/picolibc.git "$src.tmp"
        mv "$src.tmp" "$src"
    fi
    if [[ ! -x "$cache/venv/bin/meson" ]]; then
        echo "build-libc: installing meson $meson_version"
        "$py" -m venv "$cache/venv"
        "$cache/venv/bin/pip" install -q "meson==$meson_version"
    fi

    work="$out/work"
    rm -rf "$work" "$out/include" "$out/lib" "$out/split"
    mkdir -p "$work"
    cargs=$(printf "'%s', " "${arch[@]}")
    cat > "$work/cross.txt" <<EOF
[binaries]
c = '$gcc'
ar = '$bin/${tool}ar'
as = '$gcc'
nm = '$bin/${tool}nm'
strip = '$bin/${tool}strip'

[host_machine]
system = 'none'
cpu_family = '$family'
cpu = '$cpu'
endian = 'little'

[properties]
skip_sanity_check = true

[built-in options]
c_args = [${cargs}'-ffunction-sections', '-fdata-sections']
EOF

    # UTF-8 (mb-capable) is off: in this picolibc it forces wide %ls into the
    # double printf, which then reads the locale, a per-process variable, and
    # could no longer be shared. Atomic ungetc is off: stdio locks anyway, and its
    # AMOs would be in shared code the S31's loader cannot rewrite per app.
    echo "build-libc: building picolibc $tag for $target"
    PATH="$(dirname "$ninja"):$PATH" "$cache/venv/bin/meson" setup "$work/build" "$src" \
        --cross-file "$work/cross.txt" --prefix "$out" --libdir lib --includedir include \
        -Dbuildtype=minsize -Dmultilib=false -Dspecsdir=none -Dtests=false \
        -Dpicocrt=false -Dsemihost=false \
        -Dthread-local-storage=false -Derrno-function=__errno -Dnewlib-global-errno=false \
        -Dposix-console=true -Dnewlib-have-fcntl=true -Dstdio-locking=true \
        -Dformat-default=double -Dio-long-long=true -Dio-pos-args=true -Dio-percent-b=true \
        -Dprintf-percent-n=false -Dio-wchar=false -Dmb-capable=false \
        -Dwant-math-errno=true -Dassert-verbose=true -Datomic-ungetc=false \
        > "$work/setup.log"
    PATH="$(dirname "$ninja"):$PATH" "$cache/venv/bin/meson" install -C "$work/build" > "$work/build.log"
    touch "$stamp"
fi

# The split needs the OS layer under libc: libespix, and what the kernel provides.
espix_lib=$("$root/tools/espix-cc" --target="$target" --build-lib)
os_names="$root/user/libc-os.$target"
split="$out/split"
if [[ -f "$split/libc.so" && -z "$(find "$out/lib/libc.a" "$espix_lib" "$os_names" \
          "$root/tools/libc-split.py" "$root/user/libc-image.ld" -newer "$split/libc.so" 2>/dev/null | head -1)" ]]; then
    echo "build-libc: $target is up to date ($tag)"
    exit 0
fi
"$py" -I "$root/tools/libc-split.py" --tool "$bin/$tool" --libc "$out/lib/libc.a" \
    --os "$espix_lib" --os-names "$os_names" --dtors "$dtors" ${split_opts[@]+"${split_opts[@]}"} \
    --rt "$("$gcc" "${arch[@]}" -print-libgcc-file-name)" --out "$split"

if [[ $target == esp32s3 ]]; then
    # libgcc stays out: its S3 build is not position-independent, so the kernel answers its helpers.
    if ! "$gcc" "${arch[@]}" -shared -nostdlib -Wl,--gc-sections -Wl,-soname=libc.so -Wl,--build-id=sha1 \
            -Wl,--whole-archive "$split/libc-shared.a" -Wl,--no-whole-archive -o "$split/libc.so.tmp" \
            > "$split/link.log" 2>&1; then
        cat "$split/link.log" >&2
        exit 1
    fi
    mv -f "$split/libc.so.tmp" "$split/libc.so"
    "$bin/${tool}strip" --strip-all -R .xt.prop -R .xt.lit -R .comment -R .xtensa.info \
        -o "$split/libc.so.staged" "$split/libc.so"
else
    # The calls back into the app go to its hook slots, at fixed addresses (libespix/libc_hooks.S).
    hooks=()
    while read -r off name; do
        hooks+=("-Wl,--defsym=${name#__espix_hook_}=$((0x10000 + 0x$off))")
    done < <("$bin/${tool}nm" "$espix_lib" | awk '$3 ~ /^__espix_hook_/ {print $1, $3}')
    [[ ${#hooks[@]} -gt 0 ]] || { echo "build-libc: no hooks in $espix_lib" >&2; exit 1; }
    if ! "$gcc" "${arch[@]}" -nostdlib -static -T "$root/user/libc-image.ld" -Wl,--build-id=sha1 \
            "${hooks[@]}" -Wl,--whole-archive "$split/libc-shared.a" -Wl,--no-whole-archive -lgcc \
            -o "$split/libc.so.tmp" > "$split/link.log" 2>&1; then
        cat "$split/link.log" >&2
        exit 1
    fi
    # The S31's loader rewrites an app's AMOs per process; shared pages cannot have any.
    if "$bin/${tool}objdump" -d "$split/libc.so.tmp" | grep -qE '[[:space:]]amo[a-z]+\.[wd]'; then
        echo "build-libc: libc.so has AMO instructions:" >&2
        "$bin/${tool}objdump" -d "$split/libc.so.tmp" | grep -E '[[:space:]]amo[a-z]+\.[wd]' >&2
        exit 1
    fi
    mv -f "$split/libc.so.tmp" "$split/libc.so"
    "$bin/${tool}strip" --strip-all -o "$split/libc.so.staged" "$split/libc.so"
    # What an app does not define itself binds to libc.so's address; the hooks are the app's own.
    "$bin/${tool}nm" -g --defined-only "$split/libc.so" \
        | awk 'NR == FNR {want[$1] = 1; next} $2 != "A" && ($3 in want) {printf "PROVIDE(%s = 0x%s);\n", $3, $1}' \
              "$split/libc.syms" - > "$split/libc-provide.ld"
fi

# The note an app carries: its ABI (2: its own libc) and the libc.so build it needs, none when static.
id=$("$bin/${tool}readelf" -n "$split/libc.so" | sed -n 's/.*Build ID: *\([0-9a-f]*\).*/\1/p')
[[ ${#id} == 40 ]] || { echo "build-libc: libc.so has no build ID" >&2; exit 1; }
note() {
    { printf '.section .note.espix,"a",@note\n.balign 4\n.4byte 6, 24, 1\n.asciz "espix"\n.balign 4\n.4byte 2\n'
      printf '.byte %s\n' "$(echo "$1" | sed 's/../0x&,/g; s/,$//')"; } > "$split/$2.S"
    "$gcc" "${arch[@]}" -c "$split/$2.S" -o "$split/$2.o"
}
note "$id" abi-note
note "$(printf '%040d' 0)" abi-note-static
echo "build-libc: $target -> $split/libc.so ($("$bin/${tool}size" "$split/libc.so" | awk 'NR==2 {print $1}') bytes of code, build $id)"
