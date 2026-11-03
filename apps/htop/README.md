# htop on espix

[htop](https://github.com/htop-dev/htop), the real one, built as a U-mode espix
app. This directory is a **build spike**, not an integrated app: the ELF links
and is the right kind of object for the loader, but the kernel does not yet
publish enough of /proc and /sys for it to draw a screen.

    ./build.sh
    # build/htop: 377 KB, ET_EXEC, entry 0x10000
    scp build/htop esp@<host>:/bin/htop
    htop
    # cannot open /proc/stat          <- until the kernel side lands

The point of the exercise is the kernel side. htop reads Linux's interfaces,
and every gap it finds is a small, real thing espix should have anyway: a
per-process stat, a machine-wide stat, two CPU directories, one thermal zone,
uname, a clock tick. None of it needs fork(), mmap or a dynamic linker.

## What is upstream and what is ours

| file | what |
|---|---|
| fetch-htop.sh, fetch-ncurses.sh | pinned upstream sources; nothing vendored |
| build.sh | the whole cross build, in the order that works |
| compat/ | the Linux headers htop includes that espix does not have |
| espix_compat.c | the POSIX calls htop links that espix does not have |
| sensors/ | a seven-function libsensors, over the kernel's thermal zone |

htop itself is unmodified. ncurses is unmodified.

## Measured

| | |
|---|---|
| htop | 3.5.3 |
| ncurses | 6.4, narrow (--disable-widec), static |
| text | 200 KB |
| data | 95 KB |
| bss | 4 KB |
| stripped ELF | 377 KB |
| of which terminfo | ~9 KB, xterm entry compiled in by --with-fallbacks=xterm |

A 377 KB file in /bin is unremarkable on the 3.2 MB rootfs an 8 MB S31 has, and
it is one file: there is no libncurses.so or terminfo tree to install beside
it.

## The shape: a static app, not a virtual .so

A U-mode app is an ET_EXEC at a fixed address. The loader relocates it against
the kernel's published symbol tables and starts it; an app's own libc and
libraries are inside the ELF. espix has no ELF interpreter, no PT_DYNAMIC
handling, no mmap of a file's segments, no GOT/PLT resolution, and dlopen()
deliberately returns "Dynamic loading not supported".

So a file called /lib/libsensors.so on the VFS would be **bytes**, not
something the loader maps. Serving it from the kernel does not make it
linkable, and implementing what would is a dynamic linker plus M7's mmap -- a
far larger project than htop, for no flash saving: with one address space per
process, every app maps its own copy of the text anyway. Shared text (M7) is a
RAM optimization, not a flash one.

The espix-shaped answers are:

- **ncurses**: a static library linked into the app. It is big enough to be
  worth pulling in, and too big to reimplement; it is not kernel material.
- **libsensors**: seven functions. A tiny implementation in the app, over a
  kernel thermal file. Pulling the real library would add a hwmon scanner and a
  configuration parser to read one register.

A "library supplied by the kernel" is what the ABI tables and syscalls already
are. A .so is the wrong mechanism for it.

## ncurses

Built once by build.sh:

    --disable-widec --without-cxx --without-ada --without-progs
    --without-tests --without-manpages --with-termlib --enable-termcap=no
    --with-fallbacks=xterm --disable-db-install --without-debug

Two espix things had to be true first:

- termios exists and is IDF's layout; ncurses compiles its TERMIO path against
  it. It needs the **full** B0-B4000000 baud table, not the subset espix had;
  that is now in user/include/common/espix_termios.h.
- tcgetattr() is in libespix, not libc, so the configure test is told
  cf_cv_have_tcgetattr=yes and every link test includes libespix.

--with-fallbacks=xterm compiles the terminfo entry into libncurses, so the app
needs no /usr/share/terminfo. espix sets TERM=xterm for an ANSI session
(components/espix_shell/env.c), which is exactly that entry. It costs about
9 KB; a 4 KB terminfo file in the rootfs is the alternative if you would rather
keep the ELF smaller.

The host needs a tic able to parse ncurses 6.4's terminfo.src. macOS's
/usr/bin/tic (6.0) cannot; Homebrew's can, and build.sh prefers it.

## libsensors, and the temperature

htop does **not** link libsensors by default: it dlopen()s it so a GPL binary
does not have to link an LGPL one. But it has a BUILD_STATIC path, and
--enable-static --enable-sensors uses that. So no dlopen, no loader, and the
shim is an ordinary static library.

sensors/libsensors_espix.c implements exactly the seven calls htop uses:

- one chip, coretemp (one of the prefixes htop gives package priority 0);
- one feature, temp1, of type SENSORS_FEATURE_TEMP;
- one subfeature, SENSORS_SUBFEATURE_TEMP_INPUT;
- sensors_get_value() reads /sys/class/thermal/thermal_zone0/temp
  (millidegrees, Linux's format);
- sensors_get_label() returns NULL. htop then treats temp1 as the package
  temperature and copies it to every core, which is the truth here: one sensor
  on the die, not one per hart.

The kernel side is one synthetic file whose generator calls
temperature_sensor_get_celsius(). The sensor is installed lazily, once, and the
file is read-only. That keeps the sensor out of every app's ABI and gives any
program the same "cat /sys/class/thermal/thermal_zone0/temp" a Linux box has.

**Accuracy:** the ESP32 sensor is the die's temperature, calibrated over a
limited range, not an ambient thermometer. It is fine for "is the chip hot",
which is what htop's Temp column is for.

## CPU frequency

htop tries /sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq first and falls
back to /proc/cpuinfo's "cpu MHz". For the first version, do the second: emit a
"processor : N" block per core with "cpu MHz : 320.000". No cpufreq sysfs is
needed.

CONFIG_PM_ENABLE is off, so the frequency is fixed at 320 MHz on the S31
(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_320). The column is still useful -- it says
what the chip is running at -- and it will become dynamic for free if PM/DFS is
enabled later. That is its own project: DFS changes timing assumptions across
the kernel and drivers.

## Two cores, and why the Linux port showed one

htop does not ask sysconf() how many CPUs there are. It enumerates
/sys/devices/system/cpu/cpu[0-9]*, and it reads /proc/stat for exactly that many
cpuN lines plus the aggregate cpu line.

[annoyedmilk/esp32-s31-linux](https://github.com/annoyedmilk/esp32-s31-linux)
runs Linux on hart 1 only (OpenSBI on 0, IDF on 0 or 1 depending on the
branch). Its /sys therefore has one CPU, and htop is right to draw one bar. It
is not an htop limit.

On espix both cores run the kernel's tasks, so:

- /sys/devices/system/cpu/cpu0 and cpu1 must exist as directories. htop's
  readdir() counts them; an absent online file means "online", so the
  directories can be empty.
- /proc/stat must start with cpu, cpu0, cpu1 in that order, then btime,
  processes, procs_running, procs_blocked. htop reads exactly existingCPUs + 1
  leading cpu lines and calls CRT_fatalError() if the file is missing.

The per-core numbers do not need new tick accounting. FreeRTOS already keeps a
cumulative run-time counter per task (CONFIG_FREERTOS_RUN_TIME_STATS=y,
..._USING_ESP_TIMER=y), and the idle task on each core is pinned there. So:

    total_ticks(core) = esp_timer_get_time() / 10000
    idle_ticks(core)  = IDLEn.ulRunTimeCounter / 10000
    busy_ticks(core)  = total - idle

at USER_HZ 100. That is the same data top already uses for its per-core
figures; only the formatting is new. uxTaskGetSystemState() is already called
from /proc and from top, so there is no new periodic work.

/proc/<pid>/stat similarly gets utime + stime from the task's
ulRunTimeCounter, and starttime from started_us, both at 100 Hz.

**USER_HZ is 100, not the 250 Hz FreeRTOS tick.** /proc is a Linux-shaped
interface; htop divides those fields by sysconf(_SC_CLK_TCK). The U-mode
sysconf() now answers 100 and /proc generates at 100, so the two agree and the
kernel's tick rate stays an internal detail.

## The /proc and /sys gap

What exists today:

    /proc/meminfo /proc/cpuinfo /proc/uptime /proc/version
    /proc/<pid>/status /proc/<pid>/task/<tid>/status  (and the directories)

What htop needs for a useful screen:

| path | state | work |
|---|---|---|
| /proc/stat | missing | generator from idle counters + uptime; mandatory |
| /proc/<pid>/stat | missing | 39 fields from espix_proc_info_t + TaskStatus_t; mandatory |
| /proc/<pid>/comm | missing | optional; htop falls back to stat's comm |
| /proc/<pid>/cmdline | missing | optional; argv is not in espix_proc_info_t |
| /proc/<pid>/io | missing | optional; IO columns stay blank |
| /proc/loadavg | missing | optional; load meter stays blank |
| /proc/cpuinfo | one "processor : 0" | per-core blocks, and cpu MHz |
| /proc/meminfo | espix keys + MemTotal/Free/Available | add Buffers/Cached/Swap* as zero |
| /sys/devices/system/cpu/cpuN | missing | two empty directories |
| /sys/class/thermal/thermal_zone0/temp | missing | generator over the temp sensor |

/proc/stat must be at most 512 bytes today (SYNTH_TEXT_MAX). Three CPU lines
plus the summary fit; four cores would not. Either raise the cap for /proc/stat
or make the generator aware of the limit.

htop's process list also reads the **owner of /proc/<pid>** with stat() to fill
the USER column. Every synth node currently reports uid 0, so every row would
say root. Adding uid/gid to synth_node_t and setting them on the per-pid
directory is a small, honest fix; otherwise the USER column is wrong even
though the processes are not.

## What htop links that espix lacks

Found by linking, not guessed. espix_compat.c answers them:

| call | answer | effect |
|---|---|---|
| pipe | ENOSYS | lsof/strace/systemd/OpenRC screens cannot start |
| execlp | ENOSYS | same |
| waitpid | ECHILD | same |
| readlink | EINVAL | /proc/<pid>/exe, cwd, ns/pid columns are blank |
| openat | ENOTSUP | process-lock screen unavailable |
| dirfd, fdopendir | ENOTSUP | /proc/<pid>/fd walking unavailable |
| umask | returns 022 | htop's config is opened 0600 anyway |
| realpath | lexical resolver | htop can make its config path absolute |

Only realpath is on the normal startup path (the config file); everything else
belongs to a screen htop opens on a keypress. fork is not in the link because
picolibc's weak stub satisfies it, and the same screens are the only callers.

Two calls are worth growing properly rather than stubbing:

- readlink: espix's VFS has no symlinks. /proc/<pid>/exe is a link on Linux, so
  this waits for procfs to grow links.
- openat/dirfd: useful for any program that walks a directory tree by
  descriptor; not needed for htop's main screen.

## uname, and the small ABI additions

htop's compose path calls uname() for the architecture meter, and its configure
refuses to run without <sys/utsname.h>. espix had no U-mode uname, so this
branch adds:

- ESPIX_SYS_UNAME (72) and struct espix_utsname in
  components/espix_proc/include/espix_syscall.h;
- the handler in umode_sys.c, from espix_version(), espix_nodename(),
  espix_build_id() and the chip model;
- user/include/sys/utsname.h and user/libespix/uname.c;
- _SC_CLK_TCK = 100 in user/libespix/proc.c;
- the full baud table in espix_termios.h.

Plus sys/resource.h with getpriority()/setpriority(): htop's NICE column and its
priority actions. getpriority() returns 0, setpriority() fails with EPERM,
since espix has no Unix nice value to map onto a FreeRTOS priority yet. That is
a truthful placeholder, not a feature.

## M7 and M8

**Not needed.** The table is the milestone plan, not a prerequisite:

| htop needs | milestone | status |
|---|---|---|
| a tty with termios, TIOCGWINSZ, SIGWINCH | M6-era tty work | landed |
| an ELF app in U-mode | M1-M6 | landed |
| /proc, /sys, screen drawing | none of M7/M8 | this work |
| fork/exec/pipe for lsof, strace, systemd | M8 | optional screens |
| mmap for a large malloc | M7 | htop's own heap is small; picolibc's sbrk is enough |
| shared text for two htop instances | M7 | RAM only; one instance is fine |
| growable stacks | M7 | the 8 MB lazy stack already exists |

Do not wait for M7/M8. If you later want htop's lsof/strace screens, that is
M8's fork/exec and a pipe; it is not this exercise. The only overlap is the
syscall table: UNAME is additive and rebases trivially.

## Plan

Each phase leaves the ELF loadable and "make test" green.

1. **Make it start.** /proc/stat (aggregate + per-core), /sys/.../cpuN
   directories, _SC_CLK_TCK. htop draws two CPU meters and an empty process
   list.
2. **Make the process list real.** /proc/<pid>/stat with 39 fields,
   /proc/<pid>/comm, ownership on /proc/<pid>, cpu MHz per processor. htop
   lists processes, CPU%, TIME+, USER, FREQ.
3. **Make Temp work.** /sys/class/thermal/thermal_zone0/{temp,type},
   esp_driver_tsens in the fs component, the sensors/ shim, build with
   --enable-sensors. Enable "Also show CPU temperature" in htop's Display
   Options.
4. **Make it pleasant.** /proc/loadavg, /proc/meminfo standard keys, colors,
   TERM/terminfo, terminal resize, config save under $HOME.
5. **Integrate.** A uapp/CMakeLists.txt and a targets file so
   tools/build-apps.sh stages /bin/htop; add a test suite that checks
   /proc/stat advances, cpu0/cpu1 exist, and htop starts with a pty.
6. **Optional, M8.** fork/exec/pipe for lsof, strace and the systemd meter.

Phases 1-3 are the interesting kernel work and are independent of M7/M8.
Phase 4 is polish. Phase 5 is how it stops being a spike.

## Decisions

- Real htop, real ncurses, unmodified; the seam is app-local.
- Static libraries, no .so, no dynamic loader. A kernel-supplied library is an
  ABI table or a syscall, not a file with a .so name.
- /proc and /sys are Linux-shaped: USER_HZ 100, stat field order, millidegree
  thermal files. Apps should not have to know espix's internals.
- One thermal sensor is the package's; htop shows it for both cores.
- No PM/DFS in this work. CPU frequency is the fixed configured clock.
- htop stays GPL-2.0+; ncurses is MIT/X11. The app is a separate program, but a
  release that ships the binary must ship htop's source (or the written offer)
  and ncurses' notice. The fetch scripts keep upstream unvendored, which makes
  that easy.
