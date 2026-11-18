# A state may only jump into a block whose return-address slot is at the
# same position mod 16 (KNOWN_BUGS C1; CPython's test_shutil): compiled
# code aligns its C calls from that slot, and unification can't move it.
# Comparing only the frame locals stored under the slot is not enough: a
# compiled call of make_archive with three run-time arguments would take, at
# the join point after LOAD_GLOBAL os, the block compiled for a call with
# none ('test', 'tar'), and os.chdir would run with RSP 8 off and fault in
# its SSE stores. The calls are those of test_shutil's tests that reach it;
# each is in its own function, so each is a compiled call.
import os
import shutil
import tempfile
from shutil import make_archive


def files(top):
    root = tempfile.mkdtemp(dir=top)
    os.makedirs(os.path.join(root, 'dist', 'sub'))
    for p in ('dist/file1', 'dist/sub/file3', 'outer'):
        with open(os.path.join(root, p), 'w') as f:
            f.write('xxx')
    return root, 'dist'


def owner_group(top):
    root_dir, base_dir = files(top)
    base_name = os.path.join(tempfile.mkdtemp(dir=top), 'archive')
    make_archive(base_name, 'zip', root_dir, base_dir, owner='root',
                 group='root')
    make_archive(base_name, 'zip', root_dir, base_dir)
    make_archive(base_name, 'tar', root_dir, base_dir, owner='root',
                 group='root')
    make_archive(base_name, 'tar', root_dir, base_dir, owner='kjhkjhkjg',
                 group='oihohoh')


def relative(top, fmt):
    root_dir, base_dir = files(top)
    tmpdir2 = tempfile.mkdtemp(dir=top)
    os.rmdir(tmpdir2)
    rel_base_name = os.path.join(os.path.basename(tmpdir2), 'archive')
    save = os.getcwd()
    os.chdir(top)
    try:
        if fmt == 'tar':
            make_archive(rel_base_name, 'gztar', root_dir, '.')
            make_archive(rel_base_name, 'tar', root_dir, '.')
        else:
            make_archive(rel_base_name, 'zip', root_dir)
            make_archive(rel_base_name, 'zip', root_dir, base_dir)
    finally:
        os.chdir(save)


def in_curdir_tar(top):
    save = os.getcwd()
    os.chdir(tempfile.mkdtemp(dir=top))
    try:
        return make_archive('test', 'tar')
    finally:
        os.chdir(save)


def in_curdir_zip(top):
    save = os.getcwd()
    os.chdir(tempfile.mkdtemp(dir=top))
    try:
        return make_archive('test', 'zip')
    finally:
        os.chdir(save)


def gztar(top):
    root_dir, base_dir = files(top)
    base_name = os.path.join(tempfile.mkdtemp(dir=top), 'archive')
    return os.path.basename(make_archive(base_name, 'gztar', root_dir,
                                         base_dir))


def make_archive_entries():
    top = tempfile.mkdtemp()
    try:
        owner_group(top)
        relative(top, 'tar')
        in_curdir_tar(top)
        relative(top, 'zip')
        in_curdir_zip(top)
        return gztar(top)
    finally:
        shutil.rmtree(top)


CASES = [
    ('make_archive_entries', [()], {}),
]
