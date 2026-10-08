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
_vp_check_flavor = $(if $(filter thin dev,$(PODMAN_FLAVOR)),,$(error PODMAN_FLAVOR must be thin or dev (got '$(PODMAN_FLAVOR)')))

PODMAN ?= podman
# thin: builders/thin, the pinned quick-tier toolchain (default).  dev:
# builders/dev, qb's chimera-dev kitchen sink.  Each flavor gets its own image
# tag and build dir, because a tree configured in one image carries that
# image's configure choices (libaio vs pread, found tools) into the other.
PODMAN_FLAVOR ?= thin
PODMAN_DOCKERFILE ?= builders/$(PODMAN_FLAVOR)/Dockerfile
PODMAN_IMAGE ?= localhost/chimera-$(PODMAN_FLAVOR):local
PODMAN_BUILD_DIR ?= $(HOME)/.cache/chimera/podman-$(PODMAN_FLAVOR)
# Extra podman build flags, e.g. --no-cache for a cold rebuild.
PODMAN_BUILD_FLAGS ?=
PODMAN_STORAGE ?= $(HOME)/.local/share/containers
# Podman's default seccomp profile, and the overlay derived from it.
PODMAN_SECCOMP_BASE ?= /usr/share/containers/seccomp.json
PODMAN_SECCOMP = $(PODMAN_BUILD_DIR)/seccomp.json

# Unprivileged on purpose: host uid, no capabilities, and the default seccomp
# profile minus its io_pgetevents deny (builders/dev/seccomp-libaio.py) so the
# diskfs tests can keep libaio.  io_uring stays denied.  Tests that need
# privilege SKIP (exit 77); privileged runs belong to qb and the devcontainer.
PODMAN_RUN = $(PODMAN) run --rm --init \
	--userns=keep-id --user $(shell id -u):$(shell id -g) \
	--cap-drop=ALL --security-opt no-new-privileges \
	--security-opt seccomp=$(PODMAN_SECCOMP) \
	--pids-limit=0 \
	-v $(CURDIR):/chimera:Z -v $(PODMAN_BUILD_DIR):/build:Z -w /chimera \
	-e HOME=/build/home -e EVPL_IO_URING_ENTRIES=1024

# The generator runs in the image, so the host needs no python.  It refuses
# (and the target fails) rather than fall back to the default profile.
define _vp_podman_prep
	@$(PODMAN) image exists $(PODMAN_IMAGE) || { \
		echo "Image $(PODMAN_IMAGE) not found: run make podman-image first" >&2; exit 1; }
	@mkdir -p $(PODMAN_BUILD_DIR)/home
	@test -r $(PODMAN_SECCOMP_BASE) || { \
		echo "Seccomp profile $(PODMAN_SECCOMP_BASE) not found: set PODMAN_SECCOMP_BASE to podman's default profile" >&2; exit 1; }
	@$(PODMAN) run --rm -i --network=none --cap-drop=ALL --security-opt no-new-privileges \
		-v $(PODMAN_SECCOMP_BASE):/seccomp-base.json:ro $(PODMAN_IMAGE) \
		python3 - /seccomp-base.json < builders/dev/seccomp-libaio.py > $(PODMAN_SECCOMP).tmp || { \
		rm -f $(PODMAN_SECCOMP).tmp; echo "Cannot generate $(PODMAN_SECCOMP); not running without it" >&2; exit 1; }
	@mv $(PODMAN_SECCOMP).tmp $(PODMAN_SECCOMP)
endef

# Coverage forces clang (Makefile CMAKE_ARGS_COVERAGE).  Ask the image rather
# than the flavor name, so any PODMAN_IMAGE that has clang works.
define _vp_podman_coverage_check
	@if [ "$(_vp_bt)" = coverage ]; then \
		$(PODMAN) run --rm --network=none --cap-drop=ALL --security-opt no-new-privileges \
			$(PODMAN_IMAGE) sh -c 'command -v clang' >/dev/null || { \
			echo "BUILD_TYPE=Coverage needs clang in $(PODMAN_IMAGE) (PODMAN_FLAVOR=dev has it)" >&2; exit 1; }; \
	fi
endef

.PHONY: help build test integration install clean
.PHONY: podman-image podman-image-bump podman-build podman-test podman-shell podman-clean

##@ Standard

help: ## Show this help (default goal; never builds)
	@awk 'BEGIN { FS = ":.*## " } \
		/^##@/ { printf "\n%s\n", substr($$0, 5); next } \
		/^[a-zA-Z0-9_.-]+:.*## / { printf "  %-22s %s\n", $$1, $$2 }' $(MAKEFILE_LIST)
	@echo ""
	@echo "Variables: BUILD_TYPE=$(BUILD_TYPE) BUILD_DIR=$(BUILD_DIR) PREFIX=$(PREFIX)"
	@echo "           CTEST_TIER (empty = quick, '-C extended' = full) INTEGRATION_TIMEOUT=$(INTEGRATION_TIMEOUT)"
	@echo "Podman:    PODMAN_FLAVOR=$(PODMAN_FLAVOR) (thin or dev) PODMAN_IMAGE=$(PODMAN_IMAGE)"
	@echo "           PODMAN_DOCKERFILE=$(PODMAN_DOCKERFILE) PODMAN_BUILD_DIR=$(PODMAN_BUILD_DIR)"

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

podman-image: ## Build PODMAN_IMAGE from PODMAN_DOCKERFILE (network; see PODMAN_FLAVOR)
	$(_vp_check_flavor)
	@echo "Disk free for podman storage and the build dir ($(if $(filter dev,$(PODMAN_FLAVOR)),dev: image + build may take 12-25 GB,thin: image ~1 GB + ~3.6 GB per Debug tree)):"
	@df -h $(firstword $(wildcard $(PODMAN_STORAGE)) $(HOME)) $(firstword $(wildcard $(PODMAN_BUILD_DIR)) $(HOME))
	$(PODMAN) build $(PODMAN_BUILD_FLAGS) -t $(PODMAN_IMAGE) -f $(PODMAN_DOCKERFILE) $(dir $(PODMAN_DOCKERFILE))

# Network (skopeo, curl); never builds, commits or pushes.  DRY_RUN=1 shows the
# rewrite without touching the Dockerfile.  See builders/thin/README.md.
podman-image-bump: ## Re-resolve the flavor's pins and show the diff (thin: base/snapshot/quint, no build; dev: base digest + regenerate packages.lock from a reference build)
	$(_vp_check_flavor)
	@$(if $(DRY_RUN),DRY_RUN=$(DRY_RUN) )sh builders/$(PODMAN_FLAVOR)/bump-pins.sh

podman-build: ## Build BUILD_TYPE in the container (no network)
	$(_vp_check_flavor)
	$(_vp_check_bt)
	$(_vp_podman_prep)
	$(_vp_podman_coverage_check)
	$(PODMAN_RUN) --network=none $(PODMAN_IMAGE) make build BUILD_TYPE=$(BUILD_TYPE)

podman-test: ## Build BUILD_TYPE and run the quick tier in the container (no network)
	$(_vp_check_flavor)
	$(_vp_check_bt)
	$(_vp_podman_prep)
	$(_vp_podman_coverage_check)
	$(PODMAN_RUN) --network=none $(PODMAN_IMAGE) make test BUILD_TYPE=$(BUILD_TYPE)

podman-shell: ## Interactive shell in the container (same user and caps, default network)
	$(_vp_check_flavor)
	$(_vp_podman_prep)
	$(PODMAN_RUN) -it $(PODMAN_IMAGE) bash -l

podman-clean: ## Remove PODMAN_BUILD_DIR for this flavor (the image is left alone)
	$(_vp_check_flavor)
	rm -rf $(PODMAN_BUILD_DIR)

##@ Docs

docs: ## Print pointers to the API docs and OpenAPI spec

##@ Podman privileged (opt-in; ROOT via sudo unless noted)

# Opt-in local privileged test route (builders/priv/priv-ci.sh): it runs the CI
# test legs as root in a scoped podman container, the way .gitlab-ci.yml runs
# them, without qb.  These targets are NEVER prerequisites of any default target
# and never run from `make test` or `make check`; the unprivileged SKIP-77
# default-target contract is untouched.  The live image/test/compare runs need
# sudo and are operator-run; PRIV_DRY_RUN=1 previews each without sudo.  See
# docs/architecture/user-mode-podman.md and builders/priv/README.md.
PRIV_CI = $(CURDIR)/builders/priv/priv-ci.sh
PRIV_IMAGE ?= repository.mdh.quantum.com/chimera/chimera-dev:develop
PRIV_DIR ?= $(HOME)/.cache/chimera/podman-priv
PRIV_LEGS ?= ctest_*_pynfs ctest_*_smbtorture ctest_*_wpts ctest_*_rest ctest_quint_*
PRIV_MODE ?= release
PRIV_SUDO ?= sudo
export PRIV_IMAGE PRIV_DIR PRIV_LEGS PRIV_MODE PRIV_SUDO
export PRIV_REF PRIV_FRESH PRIV_DRY_RUN PRIV_CLEAN_STORE
export PRIV_BASE PRIV_HEAD PRIV_EXPECT PRIV_REQUIRE

.PHONY: podman-priv-list podman-priv-image podman-priv-test podman-priv-compare podman-priv-diff podman-priv-clean

podman-priv-list: ## List the legs PRIV_LEGS selects (no sudo; PRIV_DRY_RUN=1 prints each script)
	@$(PRIV_CI) list

podman-priv-image: ## Pull+load chimera-dev into the scoped root store (sudo; PRIV_DRY_RUN=1 previews)
	@$(PRIV_CI) image

podman-priv-test: ## Build+run the selected legs in a privileged container (sudo; PRIV_DRY_RUN=1 previews)
	@$(PRIV_CI) test

podman-priv-compare: ## Compare PRIV_BASE vs PRIV_HEAD, write compare.md (sudo; PRIV_DRY_RUN=1 previews)
	@$(PRIV_CI) compare

podman-priv-diff: ## Compare two run dirs: make podman-priv-diff A=<dir> B=<dir> (PRIV_DRY_RUN=1 previews)
	@$(PRIV_CI) diff "$(A)" "$(B)"

podman-priv-clean: ## Remove PRIV_DIR run dirs; PRIV_CLEAN_STORE=1 also store+/run (sudo; PRIV_DRY_RUN=1 previews)
	@$(PRIV_CI) clean
