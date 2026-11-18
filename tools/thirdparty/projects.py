"""The projects whose own test suites tools/thirdparty/thirdparty.py runs.

Each is pinned to its last release that supports CPython 3.7. 'install' is
what goes into the project's venv (resolved with its dependencies into
lock.txt), 'source' the release whose sdist holds the tests (or
'source_url', the project's own release archive), 'runner' how
they are run (pytest, or Django's own runtests.py), 'args' the runner's
arguments, relative to the unpacked sdist, and 'timeout' the seconds one
run (plain or Ni) may take in all.
"""

PYTEST = ['pytest==7.4.4', 'pytest-timeout==2.3.1']

PROJECTS = {
    'attrs': dict(
        install=['attrs==23.2.0', 'hypothesis', 'cloudpickle'] + PYTEST,
        source='attrs==23.2.0', runner='pytest',
        args=['tests'], timeout=1800),
    'more-itertools': dict(
        install=['more-itertools==9.1.0'] + PYTEST,
        source='more-itertools==9.1.0', runner='pytest',
        args=['tests'], timeout=900),
    'pyparsing': dict(
        install=['pyparsing==3.1.4', 'railroad-diagrams', 'Jinja2'] + PYTEST,
        source='pyparsing==3.1.4', runner='pytest',
        args=['tests'], timeout=1800, pure=['markupsafe/_speedups*.so']),
    'jinja2': dict(
        install=['Jinja2==3.1.6', 'MarkupSafe==2.1.5', 'trio'] + PYTEST,
        source='Jinja2==3.1.6', runner='pytest',
        args=['tests'], timeout=1200, pure=['markupsafe/_speedups*.so']),
    # MarkupSafe's C speedups are removed after install, so its pure
    # fallback runs (for Jinja2 too)
    'markupsafe': dict(
        install=['MarkupSafe==2.1.5'] + PYTEST,
        source='MarkupSafe==2.1.5', runner='pytest',
        args=['tests'], timeout=600, pure=['markupsafe/_speedups*.so']),
    'docutils': dict(
        install=['docutils==0.20.1'] + PYTEST,
        source='docutils==0.20.1', runner='pytest',
        args=['test'], timeout=1800),
    'networkx': dict(
        install=['networkx==2.6.3'] + PYTEST,
        source='networkx==2.6.3', runner='pytest',
        args=['networkx'], timeout=3600),
    # the whole suite takes hours; these packages are the core, the
    # elementary functions, simplification, sets, logic and number theory
    'sympy': dict(
        install=['sympy==1.10.1', 'mpmath==1.3.0'] + PYTEST,
        source='sympy==1.10.1', runner='pytest',
        args=['sympy/core', 'sympy/functions', 'sympy/simplify',
              'sympy/sets', 'sympy/logic', 'sympy/ntheory',
              'sympy/combinatorics', 'sympy/strategies', 'sympy/printing'],
        timeout=7200),
    'dateutil': dict(
        install=['python-dateutil==2.9.0.post0', 'hypothesis', 'freezegun']
        + PYTEST,
        source='python-dateutil==2.9.0.post0', runner='pytest',
        args=['tests'], timeout=1800),
    'toolz': dict(
        install=['toolz==0.12.1'] + PYTEST,
        source='toolz==0.12.1', runner='pytest',
        args=['toolz'], timeout=900),
    # its sdist has no tests: the release's tag archive does
    'sortedcontainers': dict(
        install=['sortedcontainers==2.4.0'] + PYTEST,
        source='sortedcontainers==2.4.0', runner='pytest',
        source_url='https://github.com/grantjenks/python-sortedcontainers/'
        'archive/refs/tags/v2.4.0.tar.gz',
        args=['tests'], timeout=1800),
    'django': dict(
        install=['Django==3.2.25'],
        source='Django==3.2.25', runner='django',
        args=[], timeout=7200),
}
