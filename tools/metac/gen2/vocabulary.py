"""The vocabulary (doc/GENERATOR_DESIGN.md "Front end", "Rules"): the C
idioms whose name must survive preprocessing, because the translator and
the effect analysis read them as what they mean, not as what they expand
to. The shim (shim_path()) is written from this list: it #undefs each
name and redefines it as a call of the marker __ni_vocab_NAME.

Each entry has:
  kind      value, statement or writes_arg (its first argument is an
            lvalue it assigns);
  marker    the marker's return type in the shim;
  effect    for effects.py: decref (may run __del__), read, write, fence,
            or none;
  refs      the reference-count change per argument, for the translator:
            +1 an incref, -1 a decref; 'x' in front allows NULL;
  ni        the meta's function for it, or None where the translator
            handles the idiom itself.
"""
import collections
import hashlib
import os

Entry = collections.namedtuple('Entry', 'name kind marker effect refs ni')

ENTRIES = [
    Entry('_Py_atomic_load', 'value', 'void *', 'read', (), None),
    Entry('_Py_atomic_load_relaxed', 'value', 'void *', 'read', (), None),
    Entry('_Py_atomic_load_explicit', 'value', 'void *', 'read', (), None),
    Entry('_Py_atomic_store', 'statement', 'void', 'write', (), None),
    Entry('_Py_atomic_store_relaxed', 'statement', 'void', 'write', (), None),
    Entry('_Py_atomic_store_explicit', 'statement', 'void', 'write', (), None),
    Entry('_Py_atomic_signal_fence', 'statement', 'void', 'fence', (), None),
    Entry('_Py_atomic_thread_fence', 'statement', 'void', 'fence', (), None),
    Entry('Py_INCREF', 'statement', 'void', 'none', ('+1',), 'vinfo_incref'),
    Entry('Py_XINCREF', 'statement', 'void', 'none', ('x+1',), 'vinfo_incref'),
    Entry('Py_DECREF', 'statement', 'void', 'decref', ('-1',), 'vinfo_decref'),
    Entry('Py_XDECREF', 'statement', 'void', 'decref', ('x-1',), 'vinfo_xdecref'),
    Entry('Py_CLEAR', 'writes_arg', 'void', 'decref', ('x-1',), None),
    Entry('Py_SETREF', 'writes_arg', 'void', 'decref', ('-1',), None),
    Entry('Py_XSETREF', 'writes_arg', 'void', 'decref', ('x-1',), None),
]
BY_NAME = {e.name: e for e in ENTRIES}
MARKER = '__ni_vocab_'


def names():
    return [e.name for e in ENTRIES]


def of_marker(name):
    """The entry whose marker is the function name, else None."""
    if name.startswith(MARKER):
        return BY_NAME.get(name[len(MARKER):])
    return None


def shim_text():
    lines = ['/* written by tools/metac/gen2/vocabulary.py */',
             '#ifndef NI_VOCAB_MARKERS', '#define NI_VOCAB_MARKERS']
    lines += ['%s%s%s%s();' % (e.marker, '' if e.marker.endswith('*') else ' ',
                               MARKER, e.name) for e in ENTRIES]
    lines += ['#endif', '']
    lines += ['#undef %s' % e.name for e in ENTRIES]
    lines += ['#define %s(...) %s%s(__VA_ARGS__)' % (e.name, MARKER, e.name)
              for e in ENTRIES]
    return '\n'.join(lines) + '\n'


def shim_path():
    """The shim's file, written once per content into Ni's cache directory
    (cpython_src.cache_dir()), so that no generated file sits in the
    tree."""
    import cpython_src
    text = shim_text()
    d = os.path.join(cpython_src.cache_dir(), 'shim')
    path = os.path.join(d, 'vocab-%s.h' % hashlib.sha256(text.encode()).hexdigest()[:12])
    if not os.path.exists(path):
        os.makedirs(d, exist_ok=True)
        tmp = '%s.%d.tmp' % (path, os.getpid())
        with open(tmp, 'w') as f:
            f.write(text)
        os.replace(tmp, path)
    return path
