"""Find the CPython source tree for the version LeJIT is being built for.

LEJIT_CPYTHON_SRC, if set, is used as it is. Otherwise the release tarball for
that exact version is fetched from python.org once, checked against
cpython_sha256.txt, and unpacked into .cache/cpython/ in the repository
(untracked; make clean leaves it). Either way the tree's
Include/patchlevel.h must name exactly the version asked for: a missing or
different source is an error, never a reason to use something else.
"""
import hashlib
import os
import re
import shutil
import tarfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
HASHES = os.path.join(HERE, 'cpython_sha256.txt')
URL = 'https://www.python.org/ftp/python/%s/%s'
VERIFIED = '.le-verified'


class SourceError(Exception):
    pass


def source_version(src):
    """PY_VERSION from the tree's Include/patchlevel.h, e.g. '3.7.17'."""
    path = os.path.join(src, 'Include', 'patchlevel.h')
    try:
        with open(path) as f:
            m = re.search(r'#define\s+PY_VERSION\s+"([^"]+)"', f.read())
    except OSError as e:
        raise SourceError('%s is not a CPython source tree (%s)' % (src, e))
    if not m:
        raise SourceError('%s has no PY_VERSION' % path)
    return m.group(1)


def check_version(src, version):
    found = source_version(src)
    if found != version:
        raise SourceError('%s is CPython %s, but the build is for %s'
                          % (src, found, version))


def source_sha(src):
    """The sha256 of the release tarball the tree was unpacked from, or
    'local' for a tree this module didn't unpack (LEJIT_CPYTHON_SRC)."""
    try:
        with open(os.path.join(src, VERIFIED)) as f:
            return f.read().strip()
    except OSError:
        return 'local'


def cache_dir():
    return os.path.normpath(os.path.join(HERE, '..', '..', '.cache'))


def expected_sha256(name):
    with open(HASHES) as f:
        for line in f:
            fields = line.split('#')[0].split()
            if len(fields) == 2 and fields[0] == name:
                return fields[1].lower()
    return None


def find_source(version, log=print):
    """The source tree for `version` (PY_VERSION spelling)."""
    src = os.environ.get('LEJIT_CPYTHON_SRC')
    if src:
        check_version(src, version)
        return src
    name = 'Python-%s.tgz' % version
    sha = expected_sha256(name)
    if sha is None:
        raise SourceError(
            'no checksum for %s in %s: add its sha256 (from a source you'
            ' trust), or set LEJIT_CPYTHON_SRC to a CPython %s source tree'
            % (name, os.path.relpath(HASHES), version))
    root = os.path.join(cache_dir(), 'cpython')
    tree = os.path.join(root, 'Python-%s' % version)
    marker = os.path.join(tree, VERIFIED)
    if os.path.exists(marker):
        with open(marker) as f:
            if f.read().strip() == sha:
                check_version(tree, version)
                return tree
    os.makedirs(root, exist_ok=True)
    tarball = os.path.join(root, name)
    if not os.path.exists(tarball) or _sha256(tarball) != sha:
        _fetch(version, name, tarball, sha, log)
    _unpack(tarball, root, tree, sha, log)
    check_version(tree, version)
    return tree


def _sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def _fetch(version, name, tarball, sha, log):
    # the directory is the release without any pre-release suffix
    release = re.match(r'\d+\.\d+\.\d+', version).group(0)
    url = URL % (release, name)
    log('fetching %s' % url)
    tmp = '%s.part%d' % (tarball, os.getpid())
    req = urllib.request.Request(url, headers={'User-Agent': 'le-build'})
    try:
        with urllib.request.urlopen(req, timeout=60) as r, open(tmp, 'wb') as f:
            shutil.copyfileobj(r, f)
        got = _sha256(tmp)
        if got != sha:
            raise SourceError('%s has sha256 %s, expected %s'
                              % (url, got, sha))
        os.replace(tmp, tarball)
    except OSError as e:
        raise SourceError('cannot fetch %s: %s (set LEJIT_CPYTHON_SRC to a'
                          ' CPython %s source tree)' % (url, e, version))
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def _unpack(tarball, root, tree, sha, log):
    log('unpacking %s' % tarball)
    top = os.path.basename(tree)
    tmp = os.path.join(root, '.unpack%d' % os.getpid())
    if os.path.exists(tmp):
        shutil.rmtree(tmp)
    os.makedirs(tmp)
    try:
        with tarfile.open(tarball) as tf:
            members = [m for m in tf.getmembers() if m.isfile() or m.isdir()]
            for m in members:
                parts = m.name.split('/')
                if parts[0] != top or '..' in parts or m.name.startswith('/'):
                    raise SourceError('%s: unexpected member %r'
                                      % (tarball, m.name))
            if hasattr(tarfile, 'data_filter'):
                tf.extractall(tmp, members, filter='data')
            else:
                tf.extractall(tmp, members)
        with open(os.path.join(tmp, top, VERIFIED), 'w') as f:
            f.write(sha + '\n')
        if os.path.exists(tree):
            shutil.rmtree(tree)
        try:
            os.replace(os.path.join(tmp, top), tree)
        except OSError:   # another build unpacked it meanwhile
            if not os.path.exists(os.path.join(tree, VERIFIED)):
                raise
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
