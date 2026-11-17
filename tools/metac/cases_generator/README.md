# CPython's instruction-DSL parser (vendored)

`lexer.py`, `plexer.py` and `parser.py` are copied unchanged from CPython 3.12.9's `Tools/cases_generator/` (source tarball `Python-3.12.9.tgz` from python.org), under the PSF licence like the rest of CPython. They parse `Python/bytecodes.c`. `tools/metac/gen_ceval.py` uses them to read `tools/metac/bytecodes37.c` (D26).

Don't edit them. To move to a newer CPython, copy the same three files from that release's `Tools/cases_generator/` and run `tools/check.sh`. They need Python 3.10 or later, so they run under the system `python3`, not 3.7.
