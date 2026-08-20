# Thin wrapper over CMake presets, so the everyday commands are short.
# The real build configuration lives in CMakePresets.json.

PRESET ?= debug
BUILD_DIR := build/$(PRESET)
JOBS ?= $(shell nproc)

PREFIX ?= /usr/local
DESTDIR ?=

.PHONY: all build configure release run test test-live test-heavy test-verbose tsan clean distclean format tidy help guide-images \
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
##       The heavy tier is excluded by label: it moves gigabytes and needs a
##       server, and `make test` has to stay something anybody can run.
test: build
	@ctest --test-dir $(BUILD_DIR) --output-on-failure --parallel $(JOBS) --label-exclude heavy

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
	@ctest --test-dir $(BUILD_DIR) --output-on-failure --verbose --label-exclude heavy

## asan: build and test with address and undefined-behaviour sanitizers
asan:
	@$(MAKE) test PRESET=asan

## tsan: build and test under ThreadSanitizer
##       A separate build from asan because the two cannot share a binary, and a
##       core-only build against an instrumented Qt -- scripts/qt-tsan.sh makes
##       one, and ADR-0055 says why nothing else will do. Takes TESTS= to narrow it.
TESTS ?= .
# Where scripts/qt-tsan.sh installs by default. Override to put it elsewhere.
MOLE_TSAN_QT ?= $(HOME)/opt/qt-6.4.2-tsan
export MOLE_TSAN_QT

# Two suites are left out by name rather than by accident.
#
# Both start or fork a process, and the ThreadSanitizer runtime in GCC 13 aborts
# on an internal assertion when a multithreaded program forks --
# `CHECK failed: tsan_rtl.cpp:253`. That is the tool falling over, not a race:
# both suites pass under `make test` and under `make asan`. Excluding them by
# name keeps the tier green and keeps the reason visible; a filter that happened
# to miss them would tell nobody anything.
TSAN_EXCLUDE ?= tst_KilledOutright|tst_MoleTasks

tsan:
	@test -f "$(MOLE_TSAN_QT)/lib/libQt6Core.so.6" || { \
		echo "No instrumented Qt at $(MOLE_TSAN_QT)."; \
		echo "Build one with scripts/qt-tsan.sh, or set MOLE_TSAN_QT to where yours is."; \
		echo "Running this against a distribution Qt produces thousands of warnings"; \
		echo "that are its locking being invisible rather than anything of ours --"; \
		echo "see docs/adr/0055-thread-sanitizer-needs-a-qt-that-answers-it.md."; \
		exit 2; }
	@# setarch -R turns address-space randomisation off, and it has to cover the
	@# *build* as well as the run. Without it a ThreadSanitizer binary dies on
	@# "unexpected memory mapping": this distribution's mmap entropy is wider than
	@# TSan's shadow mapping expects, and nothing in the build can fix that from
	@# inside. The build needs it because the instrumented Qt's own moc is a TSan
	@# binary too, so every AutoMoc step runs one -- which is not obvious until it
	@# fails at the first file with an error that names no cause.
	@setarch $$(uname -m) -R cmake --build build/tsan --parallel $(JOBS) 2>/dev/null \
		|| cmake --preset tsan
	@setarch $$(uname -m) -R cmake --build build/tsan --parallel $(JOBS)
	@setarch $$(uname -m) -R ctest --test-dir build/tsan --output-on-failure \
		--parallel $(JOBS) --label-exclude heavy -R "$(TESTS)" -E "$(TSAN_EXCLUDE)"

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
##               Only the pictures that actually changed are written, so the diff
##               is the change rather than fifty binary files of rendering noise.
guide-images: screenshots
	@mkdir -p docs/guide/images
	@cmake --build $(BUILD_DIR) --target compare-shots --parallel $(JOBS) >/dev/null
	@changed=$$($(BUILD_DIR)/compare-shots docs/guide/images $(BUILD_DIR)/screenshots \
		--tolerance 8 --pixels 0 --list-changed); \
	count=0; for name in $$changed; do \
		cp $(BUILD_DIR)/screenshots/$$name docs/guide/images/$$name; count=$$((count + 1)); \
	done; \
	echo "  guide images: $$count of $$(ls $(BUILD_DIR)/screenshots/*.png | wc -l) rewritten"

## screenshots-check: take the pictures twice and prove a regeneration is reviewable
##                    Fails when a picture moves with nothing changed. The ones that
##                    are genuinely of something in motion are named in the script.
screenshots-check: build
	@cmake --build $(BUILD_DIR) --target compare-shots tst_Walkthrough --parallel $(JOBS) >/dev/null
	@scripts/check-screenshots.sh $(BUILD_DIR)

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
