PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=scalarfs
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# ---------------------------------------------------------------------------
# tidy-check override (real coverage). Same shape as plinking_duck's.
#
# The upstream tidy-check (extension-ci-tools) selects translation units with the
# regex 'src/.*/'. Its trailing slash requires a SUBDIRECTORY of src/, but
# scalarfs's sources are flat (src/*.cpp), so that regex matches zero TUs and the
# gate lints nothing while still reporting green (main run 40254809357: the
# run-clang-tidy invocation ends 1.7 s later with no output). This override:
#   - selects the flat sources with 'src/[^/]+\.cpp$';
#   - keeps the header filter on scalarfs's own src (excludes duckdb-core headers);
#   - forces -std=c++17 (the tidy build configures C++14, so without this clang-tidy
#     emits spurious clang-diagnostic-c++17-extensions that would fail the gate);
#   - PRINTS the TUs it selected and FAILS if the count is zero, so a green run can
#     never again be a silently empty gate.
tidy-check:
	mkdir -p ./build/tidy
	cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_DEBUG_FLAGS) -DDISABLE_UNITY=1 -DCLANG_TIDY=1 -DCMAKE_CXX_STANDARD=17 -S $(DUCKDB_SRCDIR) -B build/tidy
	cp duckdb/.clang-tidy build/tidy/.clang-tidy
	@python3 -c "import json,re,sys; db=json.load(open('build/tidy/compile_commands.json')); rx=re.compile(r'$(PROJ_DIR)src/[^/]+[.]cpp$$'); fs=sorted({e['file'] for e in db if rx.search(e['file'])}); print('[tidy] translation units selected:', len(fs)); [print('   ', f) for f in fs]; sys.exit('ERROR: tidy selected 0 translation units — the source regex matched nothing (hollow gate)' if not fs else 0)"
	cd build/tidy && python3 ../../duckdb/scripts/run-clang-tidy.py '$(PROJ_DIR)src/[^/]+\.cpp$$' -header-filter '$(PROJ_DIR)src/.*' -extra-arg=-std=c++17 -quiet $(TIDY_THREAD_PARAMETER) $(TIDY_BINARY_PARAMETER) $(TIDY_PERFORM_CHECKS)

.PHONY: test-require
# A TEST RUN THAT SKIPPED REPORTS SUCCESS. `require scalarfs` in a .test file means
# "skip unless available", so when the extension cannot be loaded EVERY file skips and
# the harness still prints "all tests passed" with exit 0. That is how 680 tests across
# this fleet went unrun behind 12 green registry checks in October 2026
# (duckdb/community-extensions#3086: the recorded extension version is truncated to 32
# bytes while the engine compares the untruncated value, so `require` finds nothing).
#
# `--require <name>` makes a MISSING require a hard failure instead -- see
# sqllogic_test_runner.cpp, where IsRequired(param) turns it into
# parser.Fail("require %s: FAILED"). It is SCOPED to the named requirement, so files
# gated on anything else (json, parquet) still skip normally. That scoping is why this
# is used rather than run_tests.py's --fail-require-skip, which collapses every skip
# reason into one string and so cannot tell an unloadable extension from a deliberate
# platform gate -- and which is a no-op as shipped anyway, searching stdout while the
# summary goes to stderr (duckdb/duckdb#27413, with a verified fix).
#
# THE NAME IS WRITTEN OUT, not $(EXT_NAME), because the two are not always the same:
# sitting_duck sets EXT_NAME=duckdb_ast while its tests say `require sitting_duck`, so
# $(EXT_NAME) there would match no token and make this guard a silent no-op -- the exact
# failure class it exists to catch.
#
# WATCHED IT FAIL BOTH WAYS on DuckDB 2.0 (b15358c371d) before being trusted, by moving
# the built extension aside (measured on duck_block_utils):
#     make test-require, present : exit 0  "2 skipped, 813 assertions in 16 cases"
#     make test-require, hidden  : exit 2  "require duck_block_utils: FAILED"
#     make test      , hidden    : exit 0  "ran tests: 0 passed, 18 skipped"  <-- the bug
# The present case's legitimate skips passing is the proof the scoping works.
#
# INERT ON THE v1.5.6 LINE, where `require` cannot fail: hiding the artifact changes
# nothing (measured on markdown and yaml). Harmless there, load-bearing on 2.0.
# Report the ASSERTION count this prints, never `make test`'s file count.
test-require:
	./build/release/test/unittest --require scalarfs "$(TESTS_BASE_DIRECTORY)*"
