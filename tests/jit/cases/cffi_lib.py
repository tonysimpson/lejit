# The C functions the cffi cases call: built once per interpreter, into
# build/cffi-cases/, as a cffi API-mode module (api, api_ffi); ABI mode
# opens the same shared library with ffi.dlopen (abi, abi_ffi). Without
# cffi installed, api and abi are None and the cases have nothing to run.
import hashlib
import os
import shutil
import sys

try:
    import cffi
except ImportError:
    cffi = None

CDEF = '''
struct point { double x, y; };
enum color { RED, GREEN = 5, BLUE = 300 };
int add_int(int a, int b);
double add_double(double a, double b);
long long id_ll(long long a);
unsigned long long id_ull(unsigned long long a);
unsigned long long max_ull(int k);
size_t id_size(size_t a);
unsigned int id_u32(unsigned int a);
int from_i8(signed char a);
int from_u16(unsigned short a);
signed char to_i8(int a);
unsigned short to_u16(int a);
unsigned char to_u8(int a);
_Bool not_(_Bool a);
enum color next_color(enum color c);
void *id_ptr(void *p);
struct point *id_point(struct point *p);
double point_x(struct point *p);
double scale(struct point *p, double k, int n);
int six(int a, int b, int c, int d, int e, int f);
int seven(int a, int b, int c, int d, int e, int f, int g);
double mixed(int a, double b, int c, double d, long e, double f);
float half(float x);
void set_errno(int e);
int get_errno(void);
void nothing_void(void);
int nothing(void);
int call_cb(void *cb, int a);
void sleep_ms(int ms);
'''

SOURCE = r'''
#include <errno.h>
#include <stddef.h>
#include <time.h>
struct point { double x, y; };
enum color { RED, GREEN = 5, BLUE = 300 };
int add_int(int a, int b) { return a + b; }
double add_double(double a, double b) { return a + b; }
long long id_ll(long long a) { return a; }
unsigned long long id_ull(unsigned long long a) { return a; }
unsigned long long max_ull(int k) { return ~0ULL - (unsigned)k; }
size_t id_size(size_t a) { return a; }
unsigned int id_u32(unsigned int a) { return a; }
int from_i8(signed char a) { return a; }
int from_u16(unsigned short a) { return a; }
signed char to_i8(int a) { return (signed char)a; }
unsigned short to_u16(int a) { return (unsigned short)a; }
unsigned char to_u8(int a) { return (unsigned char)a; }
_Bool not_(_Bool a) { return !a; }
enum color next_color(enum color c)
{
    return c == RED ? GREEN : c == GREEN ? BLUE : RED;
}
void *id_ptr(void *p) { return p; }
struct point *id_point(struct point *p) { return p; }
double point_x(struct point *p) { return p->x; }
double scale(struct point *p, double k, int n)
{
    return (p->x + p->y) * k + n;
}
int six(int a, int b, int c, int d, int e, int f)
{
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
}
int seven(int a, int b, int c, int d, int e, int f, int g)
{
    return six(a, b, c, d, e, f) + 7 * g;
}
double mixed(int a, double b, int c, double d, long e, double f)
{
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6;
}
float half(float x) { return x / 2; }
void set_errno(int e) { errno = e; }
int get_errno(void) { return errno; }
void nothing_void(void) {}
int nothing(void) { return 0; }
int call_cb(void *cb, int a) { return ((int (*)(int))cb)(a) + 1; }
void sleep_ms(int ms)
{
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, 0);
}
'''

NAME = '_lejit_cffi_cases'


def _build():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))))
    key = hashlib.sha1((CDEF + SOURCE + cffi.__version__).encode())
    tag = '%s-%s' % (sys.implementation.cache_tag, key.hexdigest()[:12])
    out = os.path.join(root, 'build', 'cffi-cases', tag)
    if not os.path.isdir(out):
        tmp = '%s.tmp-%d' % (out, os.getpid())
        ffi = cffi.FFI()
        ffi.cdef(CDEF)
        ffi.set_source(NAME, SOURCE)
        ffi.compile(tmpdir=tmp)
        try:
            os.rename(tmp, out)
        except OSError:  # another process built it first
            shutil.rmtree(tmp)
    sys.path.insert(0, out)
    import _lejit_cffi_cases
    return _lejit_cffi_cases


api = api_ffi = abi = abi_ffi = None
# tests/jit/run.py reads every file here for its CASES, with no __file__
if cffi is not None and '__file__' in globals():
    _module = _build()
    api, api_ffi = _module.lib, _module.ffi
    abi_ffi = cffi.FFI()
    abi_ffi.cdef(CDEF)
    abi = abi_ffi.dlopen(_module.__file__)
