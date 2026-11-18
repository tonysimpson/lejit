"""The generator's on-disk cache (doc/BUILD.md "Generation cache"): pickled
results of the front end and the effect analysis, under
cpython_src.cache_dir()/gen/KIND/, each file named by the sha256 of
everything its result depends on, which the caller computes.

Several builds may share the cache at once: an entry is written to a file of
its own and renamed into place, so a reader sees a whole entry or none, and
an entry that doesn't load is treated as missing and written again.
NI_NO_CACHE=1 (generate.py --no-cache) bypasses it, reading and writing
nothing. Entries unused for PRUNE_DAYS are removed.
"""
import hashlib
import os
import pickle
import sys
import time

PRUNE_DAYS = 14
_digests = {}
_pruned = set()


def enabled():
    return not os.environ.get('NI_NO_CACHE')


def _dir(kind):
    import cpython_src
    return os.path.join(cpython_src.cache_dir(), 'gen', kind)


def file_digest(path):
    """sha256 of the file's bytes, read once per (path, size, mtime)."""
    st = os.stat(path)
    k = (path, st.st_size, st.st_mtime_ns)
    if k not in _digests:
        with open(path, 'rb') as f:
            _digests[k] = hashlib.sha256(f.read()).hexdigest()
    return _digests[k]


def key(*parts):
    """The sha256 of `parts`, each a str or bytes."""
    h = hashlib.sha256()
    for p in parts:
        b = p if isinstance(p, bytes) else str(p).encode()
        h.update(b'%d:' % len(b))
        h.update(b)
    return h.hexdigest()


def load(kind, k):
    """The entry, or None if there is none or it doesn't load."""
    if not enabled():
        return None
    path = os.path.join(_dir(kind), k)
    try:
        with open(path, 'rb') as f:
            value = pickle.load(f)
        os.utime(path)
    except Exception:  # noqa: BLE001 - missing, partial or stale: rebuild
        return None
    return value


def store(kind, k, value):
    """Write the entry; a failure (a full disk, a tree too deep to pickle)
    only costs the next run the work."""
    if not enabled():
        return
    d = _dir(kind)
    tmp = os.path.join(d, '.%s.%d.%d' % (k, os.getpid(), time.monotonic_ns()))
    try:
        os.makedirs(d, exist_ok=True)
        _prune(d)
        with open(tmp, 'wb') as f:
            pickle.dump(value, f, protocol=pickle.HIGHEST_PROTOCOL)
        os.replace(tmp, os.path.join(d, k))
    except (OSError, pickle.PicklingError, RecursionError) as e:
        print('cache: not stored (%s)' % e, file=sys.stderr)
        try:
            os.remove(tmp)
        except OSError:
            pass


def _prune(d):
    """Remove entries unused for PRUNE_DAYS (once per directory and
    process); a temporary file that old was left by a killed writer."""
    if d in _pruned:
        return
    _pruned.add(d)
    old = time.time() - PRUNE_DAYS * 86400
    for name in os.listdir(d):
        path = os.path.join(d, name)
        try:
            if os.stat(path).st_mtime < old:
                os.remove(path)
        except OSError:
            pass
