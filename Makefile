# Thin wrapper over CMake presets, so the everyday commands are short.
# The real build configuration lives in CMakePresets.json.

PRESET ?= debug
BUILD_DIR := build/$(PRESET)
JOBS ?= $(shell nproc)

PREFIX ?= /usr/local
DESTDIR ?=

# The version, read out of the one place that holds it rather than written here as
# well. The artefacts a release produces are named after it -- mole-<version>-x86_64 --
# and a third copy would defeat the point of MOLE-117. One `sed` over one line, so
# a script can rewrite that line and everything follows.
VERSION := $(shell sed -n 's/^ *VERSION \([0-9][0-9.]*\)$$/\1/p' CMakeLists.txt)

.PHONY: all build configure optimised release run test packages deb rpm appimage start-check test-live test-heavy test-verbose tsan clean distclean format tidy help guide-images where-the-log-is \
        install uninstall bundle licence-check screenshots version

all: build

## version: print the version this repository is at
##          Read from project(VERSION) in CMakeLists.txt, which is the only place
##          it is written down; `make release` rewrites that line and nothing else.
version:
	@test -n "$(VERSION)" || { echo "no VERSION in CMakeLists.txt"; exit 1; }
	@echo "$(VERSION)"

## build: configure if needed, then compile (default)
build: configure
	@cmake --build $(BUILD_DIR) --parallel $(JOBS)
	@echo ""
	@echo "  binary : $(BUILD_DIR)/mole"
	@echo "  plugins: $(BUILD_DIR)/plugins"

configure:
	@test -f $(BUILD_DIR)/CMakeCache.txt || cmake --preset $(PRESET)

## optimised: optimised build with debug info
##            What `make release` used to mean. It was renamed when release
##            started meaning "cut a release", which is what everybody outside
##            this file assumes it means. See MOLE-118.
optimised:
	@$(MAKE) build PRESET=release

## release: cut a release -- gate, version, changelog marker, commit, tag, push
##          The gate is all three test tiers, so it runs from a machine that can
##          reach the live environment and nowhere else: a suite that skipped never
##          met it, and that is a refusal rather than a pass. Stops at the first
##          step that fails and puts the tree back. DRY=1 does everything except
##          the writes and prints what it would have written; MAJOR=1, MINOR=1 or
##          VERSION=x.y.z choose the number. The only thing here that makes a tag,
##          because the tag is what publishes a release.
release:
	@MAKE="$(MAKE)" scripts/release.sh

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
	@ctest --test-dir $(BUILD_DIR) --output-on-failure --parallel $(JOBS) --label-exclude heavy \
		|| { $(MAKE) --no-print-directory where-the-log-is; exit 1; }

## where-the-log-is: print where a failing run's full output was written
#
# Printed by `test` and `test-verbose` when they fail, because not knowing is what
# cost MOLE-256 a fortnight: one assertion failed once in a parallel run and was
# never seen again, the terminal output having been filtered to its summary line
# before anybody read it. CTest had written all of it down the whole time.
where-the-log-is:
	@echo ""
	@echo "  Every test's full output, passed and failed alike, is in"
	@echo "    $(BUILD_DIR)/Testing/Temporary/LastTest.log"
	@echo "  and the names that failed are in"
	@echo "    $(BUILD_DIR)/Testing/Temporary/LastTestsFailed.log"
	@echo ""
	@echo "  Read those before re-running. An intermittent failure may not come back,"
	@echo "  and the assertion is only in the log of the run that saw it."

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
	@ctest --test-dir $(BUILD_DIR) --output-on-failure --verbose --label-exclude heavy \
		|| { $(MAKE) --no-print-directory where-the-log-is; exit 1; }

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

## install: build optimised and install into $(PREFIX) (override with PREFIX=...)
install:
	@$(MAKE) build PRESET=release
	@cmake --install build/release --prefix $(DESTDIR)$(PREFIX)
	@echo ""
	@echo "  installed to $(DESTDIR)$(PREFIX)/bin/mole"

## uninstall: remove what `make install` put in place
uninstall:
	@xargs -a build/release/install_manifest.txt rm -f 2>/dev/null || true
	@# lib or lib64, whichever the install used: GNUInstallDirs picks lib64 on
	@# every RPM distribution, so a hard-coded lib/mole left the plugins behind
	@# on exactly the systems the .rpm installs on. See MOLE-387.
	@find $(DESTDIR)$(PREFIX) -maxdepth 3 -type d -path '*/mole' -prune -exec rm -rf {} + 2>/dev/null || true
	@echo "removed"

# Where the distribution packages are built, and why it is not build/release.
#
# Arrow is in no Ubuntu archive at any version, so a .deb built with it depends on
# libarrow2500 and `apt install ./mole_*.deb` refuses on a clean system -- proved
# in a container, and the only dependency that failed. A distribution package may
# only need what the distribution can give it, so this build leaves Arrow out and
# the Parquet grid with it. The self-contained tarball keeps it: it carries its own
# libraries and answers to nobody's archive. See MOLE-121.
PACKAGE_DIR := build/packages
PACKAGE_FLAGS := -DCMAKE_DISABLE_FIND_PACKAGE_Arrow=ON -DCMAKE_DISABLE_FIND_PACKAGE_Parquet=ON

## packages: build the .deb and the .rpm, each on the family it is for
##           Both come from the install rules, through CPack, so the package and
##           `make install` cannot come apart. Skips one with a reason rather than
##           failing when the tool for it is not on the machine -- and **only**
##           for that: both scripts exit 3 with a printed `skipped:` when the tool
##           is absent and non-zero for every other failure, and these recipes
##           ended in `|| true`, which swallowed both alike. So release.yml's
##           "The .deb, the .rpm and the AppImage" step was green whatever the
##           three builds did, and the release stopped two steps later when
##           `dnf install` could not find a file -- naming the wrong fault.
##           See MOLE-387.
packages: deb rpm appimage

## deb: the .deb, built here, from what this distribution's archive can satisfy
deb:
	@command -v dpkg-deb >/dev/null || { echo "  skipped: no dpkg-deb on this machine"; exit 0; }
	@cmake -S . -B $(PACKAGE_DIR) -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo 		-DMOLE_BUILD_TESTS=OFF $(PACKAGE_FLAGS) >/dev/null
	@cmake --build $(PACKAGE_DIR) --parallel $(JOBS) >/dev/null
	@cd $(PACKAGE_DIR) && cpack -G DEB >cpack-deb.log 2>&1 || { cat cpack-deb.log; exit 1; }
	@tail -1 $(PACKAGE_DIR)/cpack-deb.log

## rpm: the .rpm, built in a container of the family it installs on
##      An .rpm built on Debian cannot be installed on an RPM system: rpmbuild
##      records what the binaries link, and Debian's libcurl carries symbol
##      versions no RPM distribution provides. See scripts/package-rpm.sh.
rpm:
	@scripts/package-rpm.sh $(PACKAGE_DIR); status=$$?; 		[ $$status = 0 ] || [ $$status = 3 ] || exit $$status

## appimage: the AppImage, built on the oldest distribution Mole runs on
##           AlmaLinux 9, so glibc 2.34: what it is built on decides what it runs
##           on, and that is a promise rather than a build detail. The figure is in
##           TODO.md and in the release notes as well as in the script.
appimage:
	@scripts/package-appimage.sh $(PACKAGE_DIR); status=$$?; 		[ $$status = 0 ] || [ $$status = 3 ] || exit $$status

## bundle: self-contained folder in dist/ that runs on machines without Qt
##         Built in its own directory with MOLE_WITH_SMB=OFF: libsmbclient is
##         GPL-3.0-or-later, and a self-contained artefact carrying it would be a
##         combined work Mole's Apache-2.0 licence cannot absorb. A local release
##         build, a .deb and an .rpm all keep Windows shares, because there the
##         library comes from the distribution. See ADR-0094.
bundle:
	@cmake -S . -B build/bundle -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DMOLE_BUILD_TESTS=OFF -DMOLE_WITH_SMB=OFF >/dev/null
	@cmake --build build/bundle --parallel $(JOBS)
	@rm -rf dist && mkdir -p dist
	@cmake --install build/bundle --prefix dist/usr >/dev/null
	@# The stripping is scripts/make-bundle.sh's, over the plugin directories it
	@# discovers. This line named `dist/usr/lib/mole/plugins` -- and GNUInstallDirs
	@# gives lib64 on every RPM distribution, so on one of those it matched nothing
	@# and `|| true` hid it. Two copies of a path that the bundler already works
	@# out for itself. See MOLE-296 for what unstripped plugins cost, and MOLE-387.
	@scripts/make-bundle.sh dist
	@cp LICENSE NOTICE THIRD-PARTY-NOTICES.md dist/
	@cp -r licenses dist/
	@scripts/licence-check.sh dist/usr/bin/mole dist

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
	@scripts/licence-check.sh

## start-check: start the artefacts on a display of their own and see them come up
##              The check every other one could not make: --version and --plugins
##              answer without a platform plugin, so an artefact that cannot open a
##              window at all passes them. See MOLE-300.
start-check:
	@fail=0; \
	if [ -x dist/mole ]; then scripts/check-artefact-starts.sh dist/mole || fail=1; \
	else echo "  skipped: no bundle in dist/ -- run make bundle"; fi; \
	for image in build/packages/*.AppImage; do \
		[ -f "$$image" ] || continue; \
		scripts/check-artefact-starts.sh "$$image" || fail=1; \
	done; \
	exit $$fail

## clean: remove build artefacts for the current preset
clean:
	@cmake --build $(BUILD_DIR) --target clean 2>/dev/null || true

## distclean: remove every build directory
distclean:
	@rm -rf build dist

## help: list available targets
help:
	@grep -E '^## ' $(MAKEFILE_LIST) | sed 's/## /  make /'
