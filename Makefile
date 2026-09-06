# espix — the idf.py incantations, and the test suite.
#
# Everything here shells out to tools/idf.sh, which finds an ESP-IDF for itself:
# `idf.py` is frequently a *shell function*, and a function is invisible to a
# make recipe's subshell. Nothing needs to be sourced first.
#
#   make build            firmware
#   make flash            firmware only -- leaves the filesystem alone
#   make fs               rootfs image -- REPLACES the filesystem
#   make flash-all        both, in the order a first boot needs
#   make monitor          attach, without resetting the board
#   make monitor-reset    attach, resetting first (to catch boot output)
#   make apps             build apps/ and stage into fsroot/bin
#   make test-app         build the test app into fsroot/home/esp
#   make test             run the test suite       [SUITE=fs] [PORT=...]
#   make stress           transport regression check, expects zero failures [N=30]
#   make clean            fullclean, firmware and apps
#
# PORT= overrides serial port detection. IDF_PATH= overrides SDK discovery.

SHELL := /bin/bash
IDF   := ./tools/idf.sh

# Serial port. Detected late (only when a target needs one) so that `make
# build` works with no board attached.
ifeq ($(origin PORT), undefined)
  PORT_ARG = $$(./tools/port.sh)
else
  PORT_ARG = $(PORT)
endif

.PHONY: all build flash fs flash-all monitor monitor-reset apps test-app \
        test stress clean help

all: build

help:
	@sed -n '3,22p' Makefile | sed 's/^# \{0,1\}//'

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

monitor:
	$(IDF) -p $(PORT_ARG) monitor --no-reset

monitor-reset:
	$(IDF) -p $(PORT_ARG) monitor

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
	    $$(p="$(PORT)"; [ -n "$$p" ] || p=$$(./tools/port.sh 2>/dev/null || true); \
	       [ -n "$$p" ] && printf -- '--port %s' "$$p")

# The port argument is built in the shell rather than with $(if ...) because
# there may not be one. `--port $$(./tools/port.sh)` passes a bare `--port` with
# nothing after it when no board is attached, and run.sh then dies on an unbound
# $2 -- which is a confusing way to be told "no serial port", and happens
# routinely now that USB-NCM gives a reason to unplug the UART cable.

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
