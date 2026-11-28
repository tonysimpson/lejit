# Frame-pointer unwinding (perf --call-graph fp, and profilers on CPython
# builds that keep frame pointers) through compiled code: a C function
# called from compiled code walks the chain of saved RBP values. Where the
# interpreter's C keeps frame pointers, compiled code never changes the RBP
# its glue from C set, so the walk goes from the compiled code that called
# it straight into LeJIT's C that ran it, and on into CPython's own frames.
# Elsewhere compiled code allocates RBP and the cases are skipped. Each
# case gives (whether the C function was called from compiled code,
# whether the walk got where it should).
import ctypes
import hashlib
import os
import subprocess
import sys
import sysconfig

SOURCE = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES 512
static void *ips[MAX_FRAMES];

/* the return address of each frame on the RBP chain from here outwards,
   while each saved RBP is further out on this thread's stack; how many */
static __attribute__((noinline)) int
fp_walk(void)
{
    void **fp = __builtin_frame_address(0);
    pthread_attr_t attr;
    void *lo;
    size_t size;
    char *hi;
    int n = 0;
    if (pthread_getattr_np(pthread_self(), &attr) != 0)
        return -1;
    pthread_attr_getstack(&attr, &lo, &size);
    pthread_attr_destroy(&attr);
    hi = (char *)lo + size;
    while (n < MAX_FRAMES) {
        void **next = fp[0];
        ips[n++] = fp[1];
        if ((char *)next <= (char *)fp || (char *)next + 16 > hi ||
            ((size_t)next & 7) != 0)
            break;
        fp = next;
    }
    return n;
}

static char core_file[PATH_MAX], interpreter_file[PATH_MAX];

/* the files compiled code's walk should reach: LeJIT's, then the
   interpreter's (NULL: not looked for) */
void
set_files(const char *core, const char *interpreter)
{
    if (core == NULL || realpath(core, core_file) == NULL)
        core_file[0] = 0;
    if (interpreter == NULL || realpath(interpreter, interpreter_file) == NULL)
        interpreter_file[0] = 0;
}

/* whether the i'th address is in the loaded object 'file' */
static int
in_file(int i, const char *file)
{
    Dl_info info;
    char path[PATH_MAX];
    return dladdr(ips[i], &info) != 0 && info.dli_fname != NULL &&
           realpath(info.dli_fname, path) != NULL && strcmp(path, file) == 0;
}

/* walks from its caller: 1 if the walk starts in compiled code (no
   loaded object holds its first addresses), 2 if it then gets where it
   should; the test makes no objects that depend on the walk */
int
fp_verdict(void)
{
    Dl_info info;
    int n = fp_walk(), compiled = 1, ok, i;
    /* the first address returns here */
    while (compiled < n && dladdr(ips[compiled], &info) == 0)
        compiled++;
    ok = n > compiled;
    if (ok && compiled > 1)
        ok = core_file[0] != 0 && in_file(compiled, core_file);
    if (ok && interpreter_file[0] != 0) {
        for (i = compiled; i < n; i++)
            if (in_file(i, interpreter_file))
                break;
        ok = i < n;
    }
    return (compiled > 1) | (ok << 1);
}

const char *
object_of(void *p)
{
    Dl_info info;
    if (dladdr(p, &info) == 0)
        return NULL;
    return info.dli_fname;
}
'''


def _build():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))))
    key = hashlib.sha1(SOURCE.encode()).hexdigest()[:12]
    out = os.path.join(root, 'build', 'fp-walk',
                       '%s-%s.so' % (sys.implementation.cache_tag, key))
    if not os.path.exists(out):
        os.makedirs(os.path.dirname(out), exist_ok=True)
        src = '%s.%d.c' % (out, os.getpid())
        tmp = '%s.%d.tmp' % (out, os.getpid())
        with open(src, 'w') as f:
            f.write(SOURCE)
        cc = (sysconfig.get_config_var('CC') or 'cc').split()
        subprocess.check_call(cc + ['-O2', '-fno-omit-frame-pointer',
                                    '-shared', '-fPIC', '-o', tmp, src,
                                    '-ldl', '-lpthread'])
        os.remove(src)
        os.rename(tmp, out)
    lib = ctypes.CDLL(out)
    lib.fp_verdict.argtypes = ()
    lib.fp_verdict.restype = ctypes.c_int
    lib.set_files.argtypes = (ctypes.c_char_p, ctypes.c_char_p)
    lib.set_files.restype = None
    lib.object_of.argtypes = (ctypes.c_void_p,)
    lib.object_of.restype = ctypes.c_char_p
    return lib


def _keeps_frame_pointers():
    # as setup.py:cpython_keeps_frame_pointers decides it
    flags = (sysconfig.get_config_var('CFLAGS') or '').split()
    keep = [f for f in flags if f in ('-fno-omit-frame-pointer',
                                      '-fomit-frame-pointer')]
    return keep[-1:] == ['-fno-omit-frame-pointer']


# tests/jit/run.py reads every file here for its CASES, with no __file__
if '__file__' in globals() and _keeps_frame_pointers():
    _lib = _build()
    _interpreter = _lib.object_of(ctypes.cast(
        ctypes.pythonapi.PyEval_EvalCode, ctypes.c_void_p))
    _files_set = []


def _verdict():
    if not _files_set:
        core = sys.modules.get('lejit._core')
        _lib.set_files(core.__file__.encode() if core is not None else None,
                       _interpreter)
        _files_set.append(True)
    r = _lib.fp_verdict()
    return (r & 1) != 0, (r & 2) != 0


def walk():
    return _verdict()


def walk_in_callee():
    return walk()


def walk_in_loop(k):
    out = None
    for i in range(k):
        out = _verdict()
    return out


def _walk_gen():
    yield _verdict()
    yield _verdict()


def walk_in_generator():
    return list(_walk_gen())


CASES = [
    ('walk', [()], {'expected': [(True, True)]}),
    ('walk_in_callee', [()], {'expected': [(True, True)]}),
    ('walk_in_loop', [(50,)], {'expected': [(True, True)]}),
    ('walk_in_generator', [()], {'expected': [[(True, True), (True, True)]]}),
]
if not _keeps_frame_pointers():
    for _case in CASES:
        _case[2]['skip'] = ("CPython's C is built without frame pointers, so"
                            " compiled code allocates RBP")
