# pycparser 3.11 (vendored)

The `pycparser/` package and `LICENSE` (BSD 3-clause, Eli Bendersky) are copied unchanged from PyPI's sdist `pycparser-3.11.tar.gz`, sha256 `d875f09c3507d00e1aba0eecc6dcadc1352f30fff09dc6bff2f1c2935e97c2bc` (PyPI's published hash, checked). `tools/metac/front.py` puts this directory first on `sys.path` and refuses any other version (D94). It needs Python 3.10 or later, like the other generators.

Don't edit it. To move to a newer release, copy the same files from that release's sdist, check its hash against PyPI's, change the version here and in `front.py`, and run `tests/parse_gate.py`.
