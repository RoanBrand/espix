#!/usr/bin/env bash
#
# htop for espix: upstream htop and ncurses, both built with tools/espix-cc, so
# htop is an ordinary POSIX app on either chip (docs/USER-MODE.md).
#
#   ./build.sh [esp32s31|esp32s3] [out]     tools/build-apps.sh calls it so
#
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
target="${1:-${ESPIX_TARGET:-$(head -n1 "$root/.espix/active" 2>/dev/null || echo esp32s31)}}"
build="$here/build-$target"
out="${2:-$build/htop.staged}"
jobs="${ESPIX_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

case "$target" in
    esp32s31) nc_host=riscv32-esp-elf;    htop_host=riscv32-linux-gnu; tool=riscv32-esp-elf ;;
    esp32s3)  nc_host=xtensa-esp32s3-elf; htop_host=xtensa-linux-gnu;  tool=xtensa-esp-elf ;;
    *) echo "htop: no target $target (esp32s31, esp32s3)" >&2; exit 1 ;;
esac
cc="$root/tools/espix-cc --target=$target"
bin=$(dirname "$(command -v "$tool-ar" || ls -d "$HOME"/.espressif/tools/"$tool"/*/"$tool"/bin/"$tool-ar" | tail -1)")
ar="$bin/$tool-ar"
ranlib="$bin/$tool-ranlib"

# ncurses' compiled-in xterm entries are made by the host's tic; macOS's 6.0 cannot read 6.4's terminfo.
tic_bin="${TIC:-}"
[ -n "$tic_bin" ] || { [ -x /opt/homebrew/opt/ncurses/bin/tic ] && tic_bin=/opt/homebrew/opt/ncurses/bin/tic; } || true
[ -n "$tic_bin" ] || tic_bin="$(command -v tic || true)"
[ -x "$tic_bin" ] || { echo "htop: no tic, which ncurses needs for its xterm entries" >&2; exit 1; }
PATH="$(dirname "$tic_bin"):$PATH"

"$here/fetch-htop.sh"
"$here/fetch-ncurses.sh"

# A terminal that ignores keypad mode sends the arrows as ESC [ A, not ESC O A: take both, as Linux's terminals do.
python3 - "$here/third_party/htop/CRT.c" <<'PY'
import sys
p = sys.argv[1]
t = open(p).read()
if 'define_key("\\033[A", KEY_UP)' not in t:
    old = '      define_key("\\033OP", KEY_F(1));'
    if old not in t:
        sys.exit('htop: CRT.c F1 define_key not found')
    new = ''.join(f'      define_key("\\033[{c}", {k});\n' for c, k in
                  (("A", "KEY_UP"), ("B", "KEY_DOWN"), ("C", "KEY_RIGHT"), ("D", "KEY_LEFT"))) + old
    open(p, 'w').write(t.replace(old, new, 1))
PY

# ncurses' MKfallback.sh widens its numbers with GNU sed's \<short\>, which BSD sed reads as nothing.
python3 - "$here/third_party/ncurses/ncurses/tinfo/MKfallback.sh" <<'PY'
import sys
p = sys.argv[1]
t = open(p).read()
old, new = "sed -e 's/\\<short\\>/NCURSES_INT2/g'", "sed -e 's/^static short /static NCURSES_INT2 /'"
if old in t:
    open(p, 'w').write(t.replace(old, new, 1))
elif new not in t:
    sys.exit(f"{p}: the numeric-type sed is not there")
PY

mkdir -p "$build/lib" "$build/ncurses" "$build/htop"

# An S3 app links as a shared object, which leaves a missing function to load time, so configure
# cannot see one; the S31 links statically against the same libc and libespix, and its answers hold.
s31_funcs="$here/build-esp32s31/funcs.cache"
cache_for() {           # <dir> <name>: a configure cache, seeded with the S31's function answers on the S3
    local cache="$1/$2.cache"
    rm -f "$cache"
    if [ "$target" = esp32s3 ]; then
        [ -f "$s31_funcs.$2" ] || "$0" esp32s31 "$here/build-esp32s31/htop.staged" >&2
        cp "$s31_funcs.$2" "$cache"
    fi
    echo "$cache"
}
keep_funcs() {          # <cache> <name>: an S31 configure's function answers, for the S3's
    [ "$target" = esp32s31 ] && grep -E '^(ac_cv_func_|cf_cv_func_)' "$1" > "$s31_funcs.$2" || true
}

echo "htop: ncurses for $target"
cache=$(cache_for "$build/ncurses" ncurses)
(cd "$build/ncurses" && cf_cv_have_tcgetattr=yes CC="$cc" CFLAGS="-Os -ffunction-sections -fdata-sections" \
    "$here/third_party/ncurses/configure" --cache-file="$cache" --host="$nc_host" \
    --disable-widec --without-cxx --without-ada --without-progs --without-tests --without-manpages \
    --with-termlib --enable-termcap=no --enable-ext-colors --with-fallbacks=xterm,xterm-256color \
    --disable-db-install --without-debug --disable-stripping > "$build/ncurses.log" 2>&1)
keep_funcs "$cache" ncurses
rm -f "$build/ncurses/ncurses/fallback.c"
make -C "$build/ncurses" -j"$jobs" AR="$ar" RANLIB="$ranlib" >> "$build/ncurses.log" 2>&1
# A narrow ncurses keeps an entry's numbers in 16 bits, where xterm-256color's 65536 pairs is 0: no colour.
python3 - "$build/ncurses/ncurses/fallback.c" <<'PY'
import sys
p = sys.argv[1]
t = open(p).read()
old, new = "/*  14: pairs    */\t65536,", "/*  14: pairs    */\t256,"
if old in t:
    open(p, "w").write(t.replace(old, new, 1))
elif new not in t:
    sys.exit("fallback.c: xterm-256color's pairs are not there")
PY
make -C "$build/ncurses" -j"$jobs" AR="$ar" RANLIB="$ranlib" >> "$build/ncurses.log" 2>&1
nc="$build/ncurses"

# libsensors over /sys's thermal zone: htop's temperature meter.
$cc -Os -I"$here/sensors" -c "$here/sensors/libsensors_espix.c" -o "$build/libsensors.o"
"$ar" rcs "$build/lib/libsensors.a" "$build/libsensors.o"

echo "htop: htop for $target"
cache=$(cache_for "$build/htop" htop)
# 32 KB of stack: htop and ncurses reach 22 KB, and an S3 app's stack cannot grow.
(cd "$build/htop" && CC="$cc" CFLAGS="-Os -ffunction-sections -fdata-sections -I$here/compat" \
    CPPFLAGS="-I$here/sensors" LDFLAGS="-L$build/lib -Wl,-z,stack-size=32768" \
    CURSES_CFLAGS="-I$nc/include" CURSES_LIBS="$nc/lib/libncurses.a $nc/lib/libtinfo.a" \
    "$here/third_party/htop/configure" --cache-file="$cache" --host="$htop_host" \
    --disable-unicode --disable-affinity --disable-delayacct --disable-hwloc --disable-capabilities \
    --enable-static --enable-sensors > "$build/htop-configure.log" 2>&1)
keep_funcs "$cache" htop
# am__append_7 is -rdynamic, which an espix app has no use for.
make -C "$build/htop" -j"$jobs" htop am__append_7= > "$build/htop-make.log" 2>&1

"$root/tools/espix-cc" --target="$target" --strip "$build/htop/htop" "$out"
echo "htop: $out ($(wc -c < "$out" | tr -d ' ') bytes)"
