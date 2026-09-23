# espix — the idf.py incantations, and the test suite.
#
# Everything here shells out to tools/idf.sh, which finds an ESP-IDF for itself:
# `idf.py` is frequently a *shell function*, and a function is invisible to a
# make recipe's subshell. Nothing needs to be sourced first.
#
#   make menu             target/board/options: the remembered setup
#   make build            firmware
#   make flash            loader and kernel -- leaves the filesystem alone
#   make flash-kernel     just the kernel (ota_1), with bootloader + table
#   make flash-loader     just the loader (ota_0)
#   make flash-monitor    flash, then attach with the only reset (to see the loader)
#   make flash-fs         rootfs image -- REPLACES the filesystem (alias: fs)
#   make flash-all        everything, in the order a first boot needs
#   make release          tag, build and publish a GitHub release
#   make monitor          attach, without resetting the board
#   make monitor-reset    attach, resetting first (to catch boot output)
#   make coredump         decode the core dump left by the last panic
#   make apps             build apps/ and stage into fsroot/bin
#   make test-app         build the test app into fsroot/home/esp
#   make test             run the test suite       [SUITE=fs] [PORT=...]
#                                                 [J=8] [SERIAL=1] [SEED=n]
#   make test-panic       same, with the UART captured to serial.log --
#                         use when hunting a panic; console suites skip
#   make stress           transport regression check, expects zero failures [N=30]
#   make clean            fullclean, firmware and apps
#
# PORT= overrides serial port detection. IDF_PATH= overrides SDK discovery.

SHELL := /bin/bash
IDF   := ./tools/idf.sh

# The target this tree is configured for, and the build directory and sdkconfig
# it implies. tools/espix writes .espix/active and tools/idf.sh reads the same
# file, so the two cannot disagree. Override for one run with TARGET=esp32s31.
TARGET       ?= $(shell cat .espix/active 2>/dev/null || echo esp32s3)
BUILD        := build-$(TARGET)
SDKCONF      := sdkconfig.$(TARGET)
LOADER_BUILD := loader/build-$(TARGET)

# Two esptool facts come from the target, not the partition CSV. The S31
# reserves its first two flash sectors, so its bootloader goes at 0x2000
# rather than 0x0, and IDF builds it without the flasher stub
# (CONFIG_ESPTOOLPY_NO_STUB). Keep in step with espix_target_is_preview in
# tools/idf.sh.
BOOT_OFF := $(if $(filter esp32s31,$(TARGET)),0x2000,0x0)
NO_STUB  := $(if $(filter esp32s31,$(TARGET)),--no-stub)

# Serial port. Detected late (only when a target needs one) so that `make
# build` works with no board attached.
PANIC_LOG ?= serial.log

ifeq ($(origin PORT), undefined)
  PORT_ARG = $$(./tools/port.sh)
else
  PORT_ARG = $(PORT)
endif

.PHONY: all menu build flash flash-kernel flash-loader flash-monitor flash-fs fs flash-all \
        release monitor monitor-reset coredump apps test-app test test-panic \
        stress clean help

all: build

help:
	@sed -n '3,25p' Makefile | sed 's/^# \{0,1\}//'

menu:
	./tools/espix

build:
	$(IDF) build

# The historical "write the firmware": bootloader, partition table, the loader
# (ota_0, which runs first) and the kernel (ota_1). The rootfs is never touched;
# `flash-fs` does that, deliberately, and only when asked.
#
# `idf.py flash` cannot be used here: it writes the app to the first OTA slot,
# which is the loader's. Everything is written by offset instead, from whichever
# partition table sdkconfig selects, and in one esptool invocation so the board
# resets once -- into the loader, which then selects the kernel.
flash: build
	$(IDF) -C loader build
	@csv=$$(sed -n 's/^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="\([^"]*\)"/\1/p' $(SDKCONF)); \
	tgt=$$(sed -n 's/^CONFIG_IDF_TARGET="\([^"]*\)"/\1/p' $(SDKCONF)); \
	lo=$$(awk -F, '/^ota_0,/ {gsub(/ /,"",$$4); print $$4}' "$$csv"); \
	ko=$$(awk -F, '/^ota_1,/ {gsub(/ /,"",$$4); print $$4}' "$$csv"); \
	if [ -z "$$lo" ] || [ -z "$$ko" ]; then \
	    echo "flash: no ota_0/ota_1 in $$csv" >&2; exit 1; \
	fi; \
	eval "$$($(IDF) --env)"; \
	"$$ESPIX_PYTHON" -m esptool --chip "$$tgt" $(NO_STUB) -p $(PORT_ARG) -b 460800 write_flash \
	    $(BOOT_OFF)     $(BUILD)/bootloader/bootloader.bin \
	    0x8000  $(BUILD)/partition_table/partition-table.bin \
	    0xf000  $(BUILD)/ota_data_initial.bin \
	    "$$lo"  $(LOADER_BUILD)/espix_loader.bin \
	    "$$ko"  $(BUILD)/espix.bin

# Bootloader, table, otadata and the kernel, without touching the loader -- the
# common case while iterating on the kernel.
flash-kernel: build
	@csv=$$(sed -n 's/^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="\([^"]*\)"/\1/p' $(SDKCONF)); \
	tgt=$$(sed -n 's/^CONFIG_IDF_TARGET="\([^"]*\)"/\1/p' $(SDKCONF)); \
	ko=$$(awk -F, '/^ota_1,/ {gsub(/ /,"",$$4); print $$4}' "$$csv"); \
	if [ -z "$$ko" ]; then echo "flash-kernel: no ota_1 in $$csv" >&2; exit 1; fi; \
	eval "$$($(IDF) --env)"; \
	"$$ESPIX_PYTHON" -m esptool --chip "$$tgt" $(NO_STUB) -p $(PORT_ARG) -b 460800 write_flash \
	    $(BOOT_OFF)     $(BUILD)/bootloader/bootloader.bin \
	    0x8000  $(BUILD)/partition_table/partition-table.bin \
	    0xf000  $(BUILD)/ota_data_initial.bin \
	    "$$ko"  $(BUILD)/espix.bin

# Flash without resetting, then attach with a reset: the one reset is the
# monitor's, so the loader's first lines are on screen. An extra monitor after
# an ordinary flash is too late -- the loader has already handed over to the
# kernel -- and resetting again would boot the kernel, not the loader.
flash-monitor: build
	$(IDF) -C loader build
	@csv=$$(sed -n 's/^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="\([^"]*\)"/\1/p' $(SDKCONF)); \
	tgt=$$(sed -n 's/^CONFIG_IDF_TARGET="\([^"]*\)"/\1/p' $(SDKCONF)); \
	lo=$$(awk -F, '/^ota_0,/ {gsub(/ /,"",$$4); print $$4}' "$$csv"); \
	ko=$$(awk -F, '/^ota_1,/ {gsub(/ /,"",$$4); print $$4}' "$$csv"); \
	if [ -z "$$lo" ] || [ -z "$$ko" ]; then \
	    echo "flash-monitor: no ota_0/ota_1 in $$csv" >&2; exit 1; \
	fi; \
	eval "$$($(IDF) --env)"; \
	"$$ESPIX_PYTHON" -m esptool --after no-reset --chip "$$tgt" $(NO_STUB) -p $(PORT_ARG) -b 460800 \
	    write_flash \
	    $(BOOT_OFF)     $(BUILD)/bootloader/bootloader.bin \
	    0x8000  $(BUILD)/partition_table/partition-table.bin \
	    0xf000  $(BUILD)/ota_data_initial.bin \
	    "$$lo"  $(LOADER_BUILD)/espix_loader.bin \
	    "$$ko"  $(BUILD)/espix.bin
	$(IDF) -p $(PORT_ARG) monitor

# The loader alone, in ota_0.
flash-loader:

	$(IDF) -C loader build
	@csv=$$(sed -n 's/^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="\([^"]*\)"/\1/p' $(SDKCONF)); \
	tgt=$$(sed -n 's/^CONFIG_IDF_TARGET="\([^"]*\)"/\1/p' $(SDKCONF)); \
	lo=$$(awk -F, '/^ota_0,/ {gsub(/ /,"",$$4); print $$4}' "$$csv"); \
	if [ -z "$$lo" ]; then echo "flash-loader: no ota_0 in $$csv" >&2; exit 1; fi; \
	eval "$$($(IDF) --env)"; \
	"$$ESPIX_PYTHON" -m esptool --chip "$$tgt" $(NO_STUB) -p $(PORT_ARG) -b 460800 \
	    write_flash "$$lo" $(LOADER_BUILD)/espix_loader.bin

# Deliberately separate from `flash`: this replaces the whole rootfs, and
# reflashing firmware should never destroy what is on the device.
#
# The image is sized to its contents and grown to the partition by the kernel on
# first mount, so this is a small write (a few hundred KB) rather than the whole
# partition. It packages the *dev* fsroot, test app and local config included.
flash-fs: build
	./tools/make-fs-image.sh fsroot $(BUILD)/storage.bin
	@csv=$$(sed -n 's/^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="\([^"]*\)"/\1/p' $(SDKCONF)); \
	off=$$(awk -F, '/^storage,/ {gsub(/ /,"",$$4); print $$4}' "$$csv"); \
	tgt=$$(sed -n 's/^CONFIG_IDF_TARGET="\([^"]*\)"/\1/p' $(SDKCONF)); \
	if [ -z "$$off" ]; then \
	    echo "flash-fs: no storage partition in $$csv" >&2; exit 1; \
	fi; \
	eval "$$($(IDF) --env)"; \
	"$$ESPIX_PYTHON" -m esptool --chip "$$tgt" $(NO_STUB) -p $(PORT_ARG) -b 460800 \
	    write_flash "$$off" $(BUILD)/storage.bin

fs: flash-fs

flash-all: flash flash-fs

# Push the firmware over the network instead of the UART cable: copy it to the
# board, put it in /boot and queue it for the loader, over SSH. Needs the board
# already on the network and its SSH reachable. See tools/flash-ota.sh.
flash-ota: build
	./tools/flash-ota.sh

# Tag v<version.txt>, build it as a release, and publish the image and manifest
# to GitHub. The tree must be clean; commit first. See tools/release.sh.
release:
	./tools/release.sh $(if $(DRY_RUN),--dry-run,)

monitor:
	$(IDF) -p $(PORT_ARG) monitor --no-reset

monitor-reset:
	$(IDF) -p $(PORT_ARG) monitor

# The core dump the last panic left in flash, decoded against build/espix.elf.
#
# Needs the serial port, because reading flash is what the port is for -- and
# that is exactly the problem when the OTG socket is in use, since this board's
# two USB-C sockets cannot both be occupied. `dmesg` and `coredump` on the device
# carry the faulting task and the reason without any of this; come here when the
# backtrace itself is what is wanted.
#
# tools/coredump.sh, not `idf.py coredump-info`: that reads the dump's exact
# length (unaligned, which esptool 5.4 chokes on) at 460800 (where this link
# drops bytes), and decodes while it reads, so one failure loses both halves.
coredump:
	PORT="$(PORT_ARG)" ./tools/coredump.sh $(CORE)

apps:
	./tools/build-apps.sh

test-app:
	./tools/build-test-app.sh

# ESPIX_PYTHON comes from the SDK: the serial console driver needs pyserial,
# which the IDF virtualenv always has and the system python usually does not.
# Without it the console suites skip rather than fail.
test: test-app
	@eval "$$(./tools/idf.sh --env)"; \
	ESPIX_PYTHON="$$ESPIX_PYTHON" ./tests/run.sh \
	    $(if $(SUITE),--suite $(SUITE),) \
	    $(if $(J),-j $(J),) \
	    $(if $(SEED),--seed $(SEED),) \
	    $(if $(SERIAL),--serial,) \
	    $$(p="$(PORT)"; [ -n "$$p" ] || p=$$(./tools/port.sh 2>/dev/null || true); \
	       [ -n "$$p" ] && printf -- '--port %s' "$$p")

# The port argument is built in the shell rather than with $(if ...) because
# there may not be one. `--port $$(./tools/port.sh)` passes a bare `--port` with
# nothing after it when no board is attached, and run.sh then dies on an unbound
# $2 -- which is a confusing way to be told "no serial port", and happens
# routinely now that USB-NCM gives a reason to unplug the UART cable.

# The suite with the serial console captured, for hunting a cache-error panic.
#
# A CacheError prints which of seven faults fired to the UART and nowhere else;
# the core dump keeps exccause 71, which is the same for all seven. Catching
# that line is the difference between naming the bug and assuming it.
#
# Deliberately NOT passing --port: macOS cu.* devices are not exclusive, so
# serlog and the console suites would race and split the byte stream between
# them, corrupting the capture this target exists to produce. run.sh sets
# ESPIX_HAVE_SERIAL=no without a port and the console suites skip -- that is the
# trade, and it is why this is a separate target rather than a flag on `test`.
test-panic: test-app
	@eval "$$(./tools/idf.sh --env)"; \
	p="$(PORT)"; [ -n "$$p" ] || p=$$(./tools/port.sh 2>/dev/null || true); \
	if [ -z "$$p" ]; then \
	    echo "make: test-panic needs a serial port; none detected (set PORT=)" >&2; \
	    exit 1; \
	fi; \
	./tools/serlog.sh "$$p" $(PANIC_LOG) || exit 1; \
	trap './tools/serlog.sh stop $(PANIC_LOG) >/dev/null 2>&1' EXIT INT TERM; \
	ESPIX_PYTHON="$$ESPIX_PYTHON" ESPIX_SERLOG=$(PANIC_LOG) ./tests/run.sh \
	    $(if $(SUITE),--suite $(SUITE),) \
	    $(if $(J),-j $(J),) \
	    $(if $(SEED),--seed $(SEED),) \
	    $(if $(SERIAL),--serial,); \
	rc=$$?; \
	echo "serial capture: $(PANIC_LOG)"; \
	exit $$rc

# Measures the known transport failure rate rather than gating on it -- see
# tests/suites/90-stress.sh. Separate from `make test` on purpose: a check that
# fails a few times in thirty would make the default run intermittently red for
# a bug that is already documented and open.
stress: test-app
	@eval "$$(./tools/idf.sh --env)"; \
	ESPIX_PYTHON="$$ESPIX_PYTHON" ./tests/run.sh --suite stress --stress \
	    $(if $(N),--stress-n $(N),)

clean:
	$(IDF) fullclean
	$(IDF) -C loader fullclean
	rm -rf apps/*/build apps/*/sdkconfig tests/app/build tests/app/sdkconfig
