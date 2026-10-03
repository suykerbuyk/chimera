# SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: Unlicense

# Downstream Makefile facade: self-documenting help, the standard
# build/test/integration/install/clean targets, and user-mode Podman.
#
# The top-level Makefile is upstream's and includes this file right after its
# SPDX header; everything else in it stays byte-identical so upstream syncs
# merge cleanly.  The standard targets here *call* upstream's recipes rather
# than replacing them, and the help text for upstream's targets lives below as
# doc-only rule lines (a rule with no recipe adds nothing to the real one).
#
# Nothing in this file may fail at parse time: the qb syntax task runs
# `make syntax-check`, which reads this file too.  BUILD_TYPE is validated in
# the recipes that use it.

# Beats upstream's later `.DEFAULT_GOAL := build_debug`: bare `make` is help
# and never builds.
override .DEFAULT_GOAL := help

# BUILD_DIR is the conventional name; upstream's knob is CHIMERA_BUILD_DIR.
# This runs before upstream's `CHIMERA_BUILD_DIR ?=`, so a BUILD_DIR given by
# the user wins there.  Otherwise BUILD_DIR follows upstream's choice (/build
# under /chimera, ./build elsewhere).
ifdef BUILD_DIR
CHIMERA_BUILD_DIR ?= $(BUILD_DIR)
endif
BUILD_DIR ?= $(CHIMERA_BUILD_DIR)
BUILD_TYPE ?= Debug
PREFIX ?= $(HOME)/.local
INTEGRATION_TIMEOUT ?= 300

# Upstream's target suffix for BUILD_TYPE: debug, release or coverage.
_vp_bt = $(strip \
	$(if $(filter Debug debug,$(BUILD_TYPE)),debug, \
	$(if $(filter Release release,$(BUILD_TYPE)),release, \
	$(if $(filter Coverage coverage,$(BUILD_TYPE)),coverage))))
_vp_check_bt = $(if $(_vp_bt),,$(error BUILD_TYPE must be Debug, Release or Coverage (got '$(BUILD_TYPE)')))

PODMAN ?= podman
PODMAN_IMAGE ?= localhost/chimera-dev:local
PODMAN_BUILD_DIR ?= $(HOME)/.cache/chimera/podman-build
PODMAN_STORAGE ?= $(HOME)/.local/share/containers

# Unprivileged on purpose: host uid, no capabilities, default seccomp.  Tests
# that need privilege SKIP (exit 77); privileged runs belong to qb and the
# devcontainer.
PODMAN_RUN = $(PODMAN) run --rm --init \
	--userns=keep-id --user $(shell id -u):$(shell id -g) \
	--cap-drop=ALL --security-opt no-new-privileges \
	--pids-limit=0 \
	-v $(CURDIR):/chimera:Z -v $(PODMAN_BUILD_DIR):/build:Z -w /chimera \
	-e HOME=/build/home -e EVPL_IO_URING_ENTRIES=1024

define _vp_podman_prep
	@$(PODMAN) image exists $(PODMAN_IMAGE) || { \
		echo "Image $(PODMAN_IMAGE) not found: run make podman-image first" >&2; exit 1; }
	@mkdir -p $(PODMAN_BUILD_DIR)/home
endef

.PHONY: help build test integration install clean
.PHONY: podman-image podman-build podman-test podman-shell podman-clean

##@ Standard

help: ## Show this help (default goal; never builds)
	@awk 'BEGIN { FS = ":.*## " } \
		/^##@/ { printf "\n%s\n", substr($$0, 5); next } \
		/^[a-zA-Z0-9_.-]+:.*## / { printf "  %-22s %s\n", $$1, $$2 }' $(MAKEFILE_LIST)
	@echo ""
	@echo "Variables: BUILD_TYPE=$(BUILD_TYPE) BUILD_DIR=$(BUILD_DIR) PREFIX=$(PREFIX)"
	@echo "           CTEST_TIER (empty = quick, '-C extended' = full) INTEGRATION_TIMEOUT=$(INTEGRATION_TIMEOUT)"

build: ## Configure and build BUILD_TYPE (Debug with ASAN by default) under BUILD_DIR
	$(_vp_check_bt)
	@$(MAKE) --no-print-directory build_$(_vp_bt)

test: ## Build BUILD_TYPE, then run the quick ctest tier
	$(_vp_check_bt)
	@$(MAKE) --no-print-directory test_$(_vp_bt)

integration: ## Build BUILD_TYPE, then run the extended ctest tier (timeout INTEGRATION_TIMEOUT)
	$(_vp_check_bt)
	@$(MAKE) --no-print-directory test_$(_vp_bt) \
		CTEST_ARGS="-C extended --output-on-failure --timeout $(INTEGRATION_TIMEOUT) -j $(CTEST_PARALLEL)"

# A tree of its own: CMAKE_INSTALL_PREFIX is cached and compiled into
# CHIMERA_STATE_DIR, so configuring the shared Release tree with PREFIX would
# leak into every later build_release and check.
install: ## Release build in BUILD_DIR/Install, then install to PREFIX (default ~/.local)
	@mkdir -p $(BUILD_DIR)/Install
	@cmake $(CMAKE_ARGS) $(CMAKE_ARGS_RELEASE) -DCMAKE_INSTALL_PREFIX=$(PREFIX) -S . -B $(BUILD_DIR)/Install
	@ninja -C $(BUILD_DIR)/Install install

clean: ## Remove everything under BUILD_DIR (keeps the directory itself)

##@ Build and test variants

build_debug: ## Debug build, no tests
build_release: ## Release build, no tests
build_coverage: ## Clang source-based coverage build, no tests
test_debug: ## Debug build, then ctest (quick tier unless CTEST_TIER is set)
test_release: ## Release build, then ctest (quick tier unless CTEST_TIER is set)
test_coverage: ## Coverage build, run ctest, then print the coverage report
debug: ## build_debug + test_debug
release: ## build_release + test_release
coverage: ## build_coverage + test_coverage

##@ CI gates

check: ## Full CI sweep over the quick tier: lint, Release+Debug build/test, scan-build
check_extended: ## The check sweep once with the extended tier (extended tests in Release and Debug)
build_clang: ## scan-build static analysis, Debug and Release
build_clang_debug: ## scan-build static analysis, Debug
build_clang_release: ## scan-build static analysis, Release

##@ Code quality

syntax: ## Reformat src/ with uncrustify (rewrites sources)
syntax-check: ## Check src/ formatting with uncrustify
copyright: ## Update copyright years on files changed vs BASE (rewrites headers)
copyright-check: ## Check copyright years on files changed vs BASE
reuse-lint: ## Check SPDX/REUSE compliance
sdk-include-check: ## Check VFS backends include only the module SDK

##@ Podman (user mode, unprivileged)

podman-image: ## Build PODMAN_IMAGE from builders/dev (network; 12-25 GB with a build)
	@echo "Disk free for podman storage and the build dir (image + build may take 12-25 GB):"
	@df -h $(firstword $(wildcard $(PODMAN_STORAGE)) $(HOME)) $(firstword $(wildcard $(PODMAN_BUILD_DIR)) $(HOME))
	$(PODMAN) build -t $(PODMAN_IMAGE) -f builders/dev/Dockerfile builders/dev

podman-build: ## Build BUILD_TYPE in the container (no network)
	$(_vp_check_bt)
	$(_vp_podman_prep)
	$(PODMAN_RUN) --network=none $(PODMAN_IMAGE) make build BUILD_TYPE=$(BUILD_TYPE)

podman-test: ## Build BUILD_TYPE and run the quick tier in the container (no network)
	$(_vp_check_bt)
	$(_vp_podman_prep)
	$(PODMAN_RUN) --network=none $(PODMAN_IMAGE) make test BUILD_TYPE=$(BUILD_TYPE)

podman-shell: ## Interactive shell in the container (same user and caps, default network)
	$(_vp_podman_prep)
	$(PODMAN_RUN) -it $(PODMAN_IMAGE) bash -l

podman-clean: ## Remove PODMAN_BUILD_DIR (the image is left alone)
	rm -rf $(PODMAN_BUILD_DIR)

##@ Docs

docs: ## Print pointers to the API docs and OpenAPI spec
