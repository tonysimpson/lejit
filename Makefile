# Build and test Ni for one interpreter (PY), or every supported one.
#
#   make                      build ni for PY, in place
#   make check                checked build and every test, for PY
#   make check-all            make check for each of PYTHONS
#   make test | cpython-test | encoder-test | features-test
#   make features-installed-test
#
# The build runs the feature detector under PY; it reads the source of PY's
# CPython release, fetched into ~/.cache/ni once. GEN_PY runs the tools'
# own tests.

SHELL := bash

PY ?= python3
GEN_PY ?= python3
PYTHONS ?= $(HOME)/.pyenv/versions/3.7.17/bin/python3.7 \
           $(HOME)/.pyenv/versions/3.8.20/bin/python3.8 \
           $(HOME)/.pyenv/versions/3.9.21/bin/python3.9 \
           $(HOME)/.pyenv/versions/3.10.16/bin/python3.10

.PHONY: build checked-build check check-all test cpython-test encoder-test \
        features-test features-installed-test no-version-tests clean

build:
	$(PY) setup.py build_ext --inplace

checked-build:
	NI_TRACE=1 NI_ALL_CHECKS=1 $(PY) setup.py build_ext --inplace

test: checked-build
	$(PY) tests/jit/run.py
	$(PY) tests/jit/build_info.py
	$(PY) tests/jit/bounds.py
	diff <($(PY) tests/jit/smoke.py plain 2>&1) <($(PY) tests/jit/smoke.py ni 2>&1)

cpython-test: checked-build
	$(PY) tests/cpython/run.py

encoder-test: checked-build
	NI_PYTHON=$(PY) $(GEN_PY) tests/encoder/check.py
	NI_PYTHON=$(PY) sh tests/encoder/mov_mem.sh

features-test:
	$(GEN_PY) tests/features/test.py

# the installed detector (no source) agrees with the source one, on each
# interpreter
features-installed-test:
	$(GEN_PY) tests/features/installed_test.py

# features come from CPython's source (NI_HAS_*), never its version number
no-version-tests:
	@! grep -rnwE 'PY_(VERSION_HEX|MAJOR_VERSION|MINOR_VERSION|MICRO_VERSION)' \
	    ni/ --include='*.[ch]' \
	  || { echo "a CPython version test in ni/ (above): add an NI_HAS_ feature"; exit 1; }

check: no-version-tests features-test features-installed-test test cpython-test \
       encoder-test

check-all:
	@for p in $(PYTHONS); do \
	    echo "######## $$p"; $(MAKE) --no-print-directory check PY=$$p || exit 1; \
	done

clean:
	rm -rf build ni.cpython-*.so
