"""Sphinx: full HTML build of Flask's documentation (from its sdist), without its conf.py,
whose theme isn't on PyPI's pinned set. Prints a digest of the HTML pages."""
import hashlib, os, sys
from sphinx.cmd.build import build_main

src, out = sys.argv[1], sys.argv[2]
code = build_main(['-q', '-C', '-b', 'html', '-E', '-D', 'extensions=sphinx.ext.autodoc,sphinx.ext.intersphinx',
                   '-D', 'master_doc=index', '-D', 'project=Flask', src, out])
h = hashlib.sha256()
pages = 0
for root, dirs, files in sorted(os.walk(out)):
    dirs.sort()
    for f in sorted(files):
        if f.endswith('.html'):
            p = os.path.join(root, f)
            h.update(os.path.relpath(p, out).encode())
            h.update(open(p, 'rb').read())
            pages += 1
print('exit', code, 'pages', pages)
print('html', h.hexdigest())
