"""Split perf samples (DWARF call graphs) by who owns them.

    perf script --no-inline -F comm,ip,sym,dso -i perf.data | python split.py

Walking each stack from the leaf, a sample belongs to the first frame that is
  - the cyclic GC                                              -> GC
  - _PyEval_EvalFrameDefault: if a C library routine (regex, str methods,
    hashing, I/O, ...) lies between it and the leaf            -> lib
    otherwise, with the runtime C it called (calls, allocation,
    dicts, attributes)                                         -> Py
  - a function in any shared object other than libpython,
    libc, libm and the loader (extensions, Cython, mypyc,
    Rust, numpy, torch, stdlib C modules)                       -> C
Stacks with none of these (truncated unwinds, the loader) are unattributed.
"""
import collections, re, sys

RUNTIME = re.compile(r'(^inlined$|libpython3|/python3\.\d+$|libc\.so|libm\.so|ld-linux|libpthread'
                     r'|\[vdso\]|\[unknown\]|\[kernel)')
LIB = re.compile(r'(_sre|sre_|SRE|pattern_|unicode_(split|rsplit|join|replace|lower|upper|find|count|encode|decode'
                 r'|translate|strip|startswith|endswith|partition|expandtabs|splitlines|isidentifier)|stringlib'
                 r'|list_sort|PyUnicode_Join|_PyUnicode_JoinArray|bytes_|_Py_dg_|siphash|long_(mul|div|pow|to_decimal)'
                 r'|k_mul|x_divrem|_PyLong_Format|marshal|_io_|textiowrapper|_bufferedreader|zlib|hashlib|_sha|_md5'
                 r'|_blake|_PyBytes_Join|PyUnicode_Format|unicode_format|do_string_format|_PyUnicodeWriter_WriteStr)')
GC = ('gc_collect_main', 'gc_collect_with_callback', 'gc_collect_generations', '_PyGC_Collect')


def owner(frames):
    for i, (sym, dso) in enumerate(frames):
        if sym.startswith(GC):
            return 'GC', None
        if sym.startswith('_PyEval_EvalFrameDefault'):
            if i and any(LIB.search(f[0]) for f in frames[:i]):
                return 'lib', None
            return 'Py', None
        if not RUNTIME.search(dso):
            return 'C', dso.rsplit('/', 1)[-1]
    return 'unattributed', None


counts, shared = collections.Counter(), collections.Counter()
frames = None


def flush():
    global frames
    if frames is not None:
        k, d = owner(frames)
        counts[k] += 1
        if d:
            shared[d] += 1
    frames = None


for line in sys.stdin:
    if not line.strip():
        flush()
    elif not line.startswith((' ', '\t')):
        flush()
        frames = []
    elif frames is not None:
        m = re.match(r'\s*[0-9a-f]+\s+(.*?)\s+\((.*)\)\s*$', line)
        if m:
            frames.append((m.group(1), m.group(2)))
flush()
total = sum(counts.values())
if not total:
    sys.exit('no samples')
pc = lambda n: 100.0 * n / total
print('samples %d: Py %.0f%%, GC %.0f%%, lib %.0f%%, C %.0f%%, unattributed %.0f%%' % (
    total, pc(counts['Py']), pc(counts['GC']), pc(counts['lib']), pc(counts['C']), pc(counts['unattributed'])))
print('  C by object: ' + ', '.join('%s %.0f%%' % (d, pc(n)) for d, n in shared.most_common(5)))
