# Thin wrapper over CMake presets, so the everyday commands are short.
# The real build configuration lives in CMakePresets.json.

PRESET ?= debug
BUILD_DIR := build/$(PRESET)
JOBS ?= $(shell nproc)

PREFIX ?= /usr/local
DESTDIR ?=

.PHONY: all build configure release run test test-live test-heavy test-verbose clean distclean format tidy help guide-images \
        install uninstall bundle licence-check screenshots

all: build

## build: configure if needed, then compile (default)
build: configure
	@cmake --build $(BUILD_DIR) --parallel $(JOBS)
	@echo ""
	@echo "  binary : $(BUILD_DIR)/mole"
	@echo "  plugins: $(BUILD_DIR)/plugins"

configure:
	@test -f $(BUILD_DIR)/CMakeCache.txt || cmake --preset $(PRESET)

## release: optimised build with debug info
release:
	@$(MAKE) build PRESET=release

SESSION_LOG ?= $(HOME)/.local/share/Mole/Mole/session.log

## run: build and launch the application, keeping a log of the session
run: build
	@$(BUILD_DIR)/mole; \
	code=$$?; \
	if [ $$code -ne 0 ]; then \
		echo; \
		echo "mole exited with $$code. The session log is at:"; \
		echo "  $(SESSION_LOG)"; \
		echo "and the run before it at $(SESSION_LOG).1"; \
	fi; \
	exit $$code

## run-gdb: launch under gdb and print every thread's stack if it crashes
##          Use when the log's own backtrace stops short: a signal that arrives
##          on an alternate stack can defeat an in-process unwinder. gdb can walk
##          past it.
run-gdb: build
	@command -v gdb >/dev/null || { echo "gdb is not installed"; exit 1; }
	@gdb -q -batch -ex "set confirm off" -ex run -ex "thread apply all bt" \
		--args $(BUILD_DIR)/mole

## test: build and run the whole suite in parallel
test: build
	@ctest --test-dir $(BUILD_DIR) --output-on-failure --parallel $(JOBS)

## test-live: run the suites that need a real server, against the testbed
##            Needs MOLE_TESTBED_ADDRESS and MOLE_TESTBED_PASSWORD, which live
##            outside this repository. A suite that skips is reported as a
##            result rather than passing quietly: that silence is how a
##            listing behaviour that differs between servers stayed hidden.
test-live: build
	@scripts/testbed/test-live.sh $(BUILD_DIR)

## test-heavy: the scale tier -- gigabytes each way against the testbed, with
##             peak scratch space, memory and descriptors asserted. Needs
##             MOLE_TESTBED_ADDRESS and MOLE_TESTBED_PASSWORD, which live
##             outside this repository. A destination without room to hold the
##             payload is reported as a skip rather than filling a disk.
test-heavy: build
	@scripts/testbed/test-heavy.sh $(BUILD_DIR)

## test-verbose: same, printing every assertion
test-verbose: build
	@ctest --test-dir $(BUILD_DIR) --output-on-failure --verbose

## asan: build and test with address and undefined-behaviour sanitizers
asan:
	@$(MAKE) test PRESET=asan

## format: apply .clang-format to every source file
format:
	@find src tests -name '*.cpp' -o -name '*.h' | xargs clang-format -i
	@echo "formatted"

## tidy: run clang-tidy over the compilation database
tidy: configure
	@cmake --build $(BUILD_DIR) --target mole_core --parallel $(JOBS) >/dev/null
	@find src -name '*.cpp' | xargs -P $(JOBS) -I{} clang-tidy -p $(BUILD_DIR) {} 2>/dev/null || true

## install: build release and install into $(PREFIX) (override with PREFIX=...)
install:
	@$(MAKE) build PRESET=release
	@cmake --install build/release --prefix $(DESTDIR)$(PREFIX)
	@echo ""
	@echo "  installed to $(DESTDIR)$(PREFIX)/bin/mole"

## uninstall: remove what `make install` put in place
uninstall:
	@xargs -a build/release/install_manifest.txt rm -f 2>/dev/null || true
	@rm -rf $(DESTDIR)$(PREFIX)/lib/mole
	@echo "removed"

## bundle: self-contained folder in dist/ that runs on machines without Qt
bundle:
	@$(MAKE) build PRESET=release
	@rm -rf dist && mkdir -p dist
	@cmake --install build/release --prefix dist/usr >/dev/null
	@strip dist/usr/bin/mole dist/usr/lib/mole/plugins/*.so 2>/dev/null || true
	@scripts/make-bundle.sh dist
	@cp LICENSE NOTICE THIRD-PARTY-NOTICES.md dist/
	@cp -r licenses dist/
	@scripts/licence-check.sh dist/usr/bin/mole

## screenshots: run the walkthrough and write pictures of each verified state
screenshots: build
	@rm -rf $(BUILD_DIR)/screenshots
	@QT_QPA_PLATFORM=offscreen MOLE_SCREENSHOT_DIR=$(BUILD_DIR)/screenshots \
		$(BUILD_DIR)/tests/tst_Walkthrough
	@echo ""
	@echo "  screenshots: $(BUILD_DIR)/screenshots"

## guide-images: refresh the user guide's screenshots from the test suite
guide-images: screenshots
	@mkdir -p docs/guide/images
	@cp $(BUILD_DIR)/screenshots/*.png docs/guide/images/
	@echo "  guide images: docs/guide/images ($(shell ls $(BUILD_DIR)/screenshots/*.png 2>/dev/null | wc -l) files)"

## licence-check: verify the Qt LGPL conditions still hold
licence-check:
	@scripts/licence-check.sh $(if $(wildcard build/release/mole),build/release/mole,build/debug/mole)

## clean: remove build artefacts for the current preset
clean:
	@cmake --build $(BUILD_DIR) --target clean 2>/dev/null || true

## distclean: remove every build directory
distclean:
	@rm -rf build dist

## help: list available targets
help:
	@grep -E '^## ' $(MAKEFILE_LIST) | sed 's/## /  make /'
