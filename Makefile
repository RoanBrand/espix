# espix — the idf.py incantations, and the test suite.
#
# Everything here shells out to tools/idf.sh, which finds an ESP-IDF for itself:
# `idf.py` is frequently a *shell function*, and a function is invisible to a
# make recipe's subshell. Nothing needs to be sourced first.
#
#   make build            firmware
#   make flash            kernel only -- leaves the filesystem alone
#   make flash-loader     the loader (ota_1); first move to this table needs both
#   make fs               rootfs image -- REPLACES the filesystem
#   make flash-all        both, in the order a first boot needs
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

# Serial port. Detected late (only when a target needs one) so that `make
# build` works with no board attached.
PANIC_LOG ?= serial.log

ifeq ($(origin PORT), undefined)
  PORT_ARG = $$(./tools/port.sh)
else
  PORT_ARG = $(PORT)
endif

.PHONY: all build flash flash-loader fs flash-all monitor monitor-reset \
        coredump apps test-app test test-panic stress clean help

all: build

help:
	@sed -n '3,24p' Makefile | sed 's/^# \{0,1\}//'

build:
	$(IDF) build

flash:
	$(IDF) -p $(PORT_ARG) flash

# Deliberately separate from `flash`: this replaces the whole rootfs, and
# reflashing firmware should never destroy what is on the device.
fs:
	$(IDF) -p $(PORT_ARG) storage-flash

flash-all:
	$(IDF) -p $(PORT_ARG) flash storage-flash

# The loader is the second app, in ota_1. `idf.py flash` writes only the kernel
# to ota_0 and knows nothing about it, so it is written here by offset. A board
# adopting the loader table needs `make flash` and then `make flash-loader`.
LOADER_OFFSET = 0x3A0000

flash-loader:
	$(IDF) -C loader build
	@eval "$$($(IDF) --env)"; \
	"$$ESPIX_PYTHON" -m esptool --chip esp32s3 -p $(PORT_ARG) -b 460800 \
	    write_flash $(LOADER_OFFSET) loader/build/espix_loader.bin

# Push the firmware over the network instead of the UART cable: copy it to the
# board, put it in /boot and queue it for the loader, over SSH. Needs the board
# already on the network and its SSH reachable. See tools/flash-ota.sh.
flash-ota: build
	./tools/flash-ota.sh

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
	rm -rf apps/*/build apps/*/sdkconfig tests/app/build tests/app/sdkconfig
