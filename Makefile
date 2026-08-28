# FreeLinX/src - build root filesystem and system integration.
#
# The Makefile only wires up the scripts in scripts/; it does not duplicate
# build logic. Scripts are POSIX /bin/sh and can be run directly.

SHELL := /bin/sh

.PHONY: all build rootfs check clean help

all: build

## build - run the full FreeLinX build (see scripts/build.sh)
build:
	./scripts/build.sh

## rootfs - prepare the staging root filesystem (see scripts/rootfs.sh)
rootfs:
	./scripts/rootfs.sh

## check - validate the host environment and configuration (see scripts/check.sh)
check:
	./scripts/check.sh

## clean - remove build artifacts (see scripts/clean.sh)
clean:
	./scripts/clean.sh

## help - list available targets
help:
	@echo 'FreeLinX/src targets:'
	@echo '  build   - run the full build (default)'
	@echo '  all     - alias for build'
	@echo '  rootfs  - (re)prepare the staging root filesystem'
	@echo '  check   - validate host environment and configuration'
	@echo '  clean   - remove build artifacts'
	@echo
	@echo 'Configuration: env vars FREELINX_* override config/<arch>/default.conf'
	@echo 'See README.md and docs/architecture.md for details.'