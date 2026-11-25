# Build and test LeJIT for one interpreter (PY), or every supported one.
#
#   make                      build lejit for PY, in place
#   make check                checked build and every test, for PY
#   make check-all            make check for each of PYTHONS
#   make test | cpython-test | encoder-test | strings-test
#   make extra-test           slower tests and fuzzers, for PY (run nightly, not on PRs)
#   make features-installed-test | cross-check (both detectors, every interpreter)
#   make format | format-check   rewrite, or check, the C code's layout
#   make lint                 clang-tidy on src/ (reports, not enforced yet)
#
# The build detects PY's features and takes its messages from PY itself
# (LEJIT_DEV_CPYTHON_SRC=fetch reads the source of PY's CPython release
# instead, fetched into .cache/ once). GEN_PY runs the tools' own tests.

SHELL := bash

PY ?= python3
GEN_PY ?= python3
# CI pins clang-format 19.1.7 (pip install clang-format==19.1.7): other
# versions may lay the code out differently
CLANG_FORMAT ?= clang-format
# and clang-tidy 19.1.0 (pip install clang-tidy==19.1.0)
CLANG_TIDY ?= clang-tidy
PYTHONS ?= $(HOME)/.pyenv/versions/3.7.17/bin/python3.7 \
           $(HOME)/.pyenv/versions/3.8.20/bin/python3.8 \
           $(HOME)/.pyenv/versions/3.9.21/bin/python3.9 \
           $(HOME)/.pyenv/versions/3.10.16/bin/python3.10

.PHONY: build checked-build check check-all test cpython-test encoder-test extra-test \
        features-installed-test strings-test \
        cross-check no-version-tests format format-check lint clean

build:
	$(PY) setup.py build_ext --inplace

checked-build:
	LEJIT_DEV_TRACE=1 $(PY) setup.py build_ext --inplace

test: checked-build
	$(PY) tests/jit/run.py
	$(PY) tests/jit/leak_check.py
	$(PY) tests/jit/build_info.py
	$(PY) tests/jit/bounds.py
	$(PY) tests/jit/out_of_memory.py
	$(PY) tests/jit/no_threads.py
	diff <($(PY) tests/jit/smoke.py plain 2>&1) <($(PY) tests/jit/smoke.py lejit 2>&1)

cpython-test: checked-build
	$(PY) tests/cpython/run.py

extra-test: checked-build
	$(PY) tests/jit/run.py tests/extra/cases
	$(PY) tests/extra/stale_types/run.py
	$(PY) tests/extra/fuzz/run.py -n 2000 -j 4
	$(PY) tests/extra/fuzz/combine.py -j 4

encoder-test: checked-build
	$(GEN_PY) tests/encoder/check.py $(PY)
	sh tests/encoder/mov_mem.sh $(PY)

# the installed detector (no source) agrees with the source one, for PY
features-installed-test:
	$(GEN_PY) tests/features/installed_test.py --strict $(PY)

# each message probe's answer is in PY's CPython source
strings-test:
	$(GEN_PY) tests/features/strings_test.py $(PY)

# both, on every interpreter installed here (pyenv), and newer ones for
# information
cross-check:
	$(GEN_PY) tests/features/installed_test.py --strict
	$(GEN_PY) tests/features/strings_test.py

# features come from CPython's source (LE_HAS_*), never its version number
no-version-tests:
	@! grep -rnwE 'PY_(VERSION_HEX|MAJOR_VERSION|MINOR_VERSION|MICRO_VERSION)' \
	    src/ --include='*.[ch]' \
	  || { echo "a CPython version test in src/ (above): add an LE_HAS_ feature"; exit 1; }

# the hand-written C, laid out by .clang-format; the generated headers
# (*_gen.h) are left alone
FORMAT_FILES = $(shell find src tests -name '*.[ch]' ! -name '*_gen.h' | sort)

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

# lists the files clang-format would change, and fails if there are any
format-check:
	@$(CLANG_FORMAT) --version
	@bad=; for f in $(FORMAT_FILES); do \
	    $(CLANG_FORMAT) "$$f" | cmp -s - "$$f" || bad="$$bad $$f"; \
	done; \
	if [ -n "$$bad" ]; then \
	    echo "not formatted (make format):"; printf '  %s\n' $$bad; exit 1; \
	fi

# clang-tidy (.clang-tidy) on the C the build compiles, with the flags of
# PY's checked build, so the checked-only code is linted too: setup.py
# writes them to build/lint/compile_commands.json instead of compiling.
# Not part of check
lint:
	LEJIT_DEV_TRACE=1 $(PY) setup.py build_ext --build-temp build/lint \
	    --build-lib build/lint --compile-commands build/lint/compile_commands.json
	$(CLANG_TIDY) --version
	$(CLANG_TIDY) -p build/lint --quiet $$($(PY) -c 'import json; \
	    print(" ".join(e["file"] for e in json.load(open("build/lint/compile_commands.json"))))')

check: no-version-tests features-installed-test strings-test \
       test cpython-test encoder-test

check-all:
	@for p in $(PYTHONS); do \
	    echo "######## $$p"; $(MAKE) --no-print-directory check PY=$$p || exit 1; \
	done

clean:
	rm -rf build lejit/_core.cpython-*.so lejit/_features
