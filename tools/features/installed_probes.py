"""Questions asked inside an installed CPython, for installed.py.

installed.py runs this file under the interpreter being built for, as
`python -I installed_probes.py [NAME...]`, and reads the JSON it prints:
the interpreter's version and include directory, its opcodes, jump opcodes
and inline caches, and an answer for each probe (all of them, or the NAMEs
given).

Each probe is a function named after the feature it answers, whose
docstring says what it checks. It returns (value, evidence) or raises
Unanswered. The probes look at the interpreter itself: the opcode
module, which functions libpython exports (ctypes), objects' attributes,
and a few lines of Python whose result shows the behaviour the feature
records. Some run a few instructions of hand-made bytecode, to see what one
instruction does. Nothing here imports lejit, and it runs on 3.7 and later.
"""
import ctypes
import dis
import json
import opcode
import sys
import sysconfig
import types


class Unanswered(Exception):
    pass


PROBES = []


def probe(f):
    PROBES.append(f)
    return f


# --- helpers ---------------------------------------------------------------

def _exported(name):
    """(libpython exports the function name, evidence): an extension can
    link to it."""
    return hasattr(ctypes.pythonapi, name), 'ctypes.pythonapi.' + name


def _real_op(name):
    """name is an instruction of the bytecode (3.12's opcode.opmap also
    names the compiler's pseudo-instructions, numbered from 256)"""
    return opcode.opmap.get(name, 256) < 256


def _need_ops(*names):
    missing = [n for n in names if not _real_op(n)]
    if missing:
        raise Unanswered('the probe needs opcode(s) %s, which this'
                         ' interpreter lacks' % ', '.join(missing))


def _code(ops, consts):
    """A function running the instructions ops, [(name, arg)], with the
    constants consts. Only for instructions without inline caches."""
    raw = b''.join(bytes([opcode.opmap[name], arg]) for name, arg in ops)
    template = (lambda: None).__code__
    if hasattr(template, 'replace'):
        code = template.replace(co_code=raw, co_consts=tuple(consts),
                                co_names=(), co_varnames=(), co_nlocals=0,
                                co_stacksize=16, co_name='_probe')
    else:
        code = types.CodeType(0, 0, 0, 16, template.co_flags, raw,
                              tuple(consts), (), (), template.co_filename,
                              '_probe', template.co_firstlineno,
                              template.co_lnotab, (), ())
    return types.FunctionType(code, {'__builtins__': __builtins__})


_unit = []


def _jump_unit():
    """How many bytes one unit of a relative jump's argument is: 1 where
    jumps count bytes, 2 where they count instructions. Found by running a
    JUMP_FORWARD over two NOPs: the instruction it lands on says which."""
    if not _unit:
        _need_ops('JUMP_FORWARD', 'NOP', 'LOAD_CONST', 'RETURN_VALUE')
        # offsets 0 JUMP_FORWARD 4, 2 NOP, 4 NOP, 6 LOAD_CONST 'bytes',
        # 8 RETURN_VALUE, 10 LOAD_CONST 'instructions', 12 RETURN_VALUE
        f = _code([('JUMP_FORWARD', 4), ('NOP', 0), ('NOP', 0),
                   ('LOAD_CONST', 0), ('RETURN_VALUE', 0),
                   ('LOAD_CONST', 1), ('RETURN_VALUE', 0)],
                  ['bytes', 'instructions'])
        landed = f()
        _unit.append({'bytes': 1, 'instructions': 2}[landed])
    return _unit[0]


def _setup_finally_ops():
    """The opcodes the block probes need, or Unanswered where the
    interpreter has no block stack (3.11 and later)."""
    _need_ops('SETUP_FINALLY', 'POP_BLOCK', 'POP_EXCEPT', 'LOAD_CONST',
              'RETURN_VALUE', 'RAISE_VARARGS', 'POP_TOP', 'CALL_FUNCTION')


def _error(f, *args):
    """The exception f(*args) raises, or None."""
    try:
        f(*args)
    except Exception as e:
        return e
    return None


# --- libpython's exports ---------------------------------------------------

@probe
def PYDICT_NEWPRESIZED():
    """libpython exports _PyDict_NewPresized (ctypes)"""
    return _exported('_PyDict_NewPresized')


@probe
def AUDIT_HOOKS():
    """libpython exports PySys_Audit (ctypes)"""
    return _exported('PySys_Audit')


@probe
def ARG_CHECK_POSITIONAL():
    """libpython exports _PyArg_CheckPositional (ctypes)"""
    return _exported('_PyArg_CheckPositional')


@probe
def OBJECTDICT_SETITEM_LINKABLE():
    """libpython exports _PyObjectDict_SetItem, so an extension can link
    to it (ctypes)"""
    return _exported('_PyObjectDict_SetItem')


@probe
def CORO_GET_AWAITABLE_ITER_LINKABLE():
    """libpython exports _PyCoro_GetAwaitableIter, so an extension can link
    to it (ctypes)"""
    return _exported('_PyCoro_GetAwaitableIter')


@probe
def ITER_CHECK_FUNCTION():
    """libpython exports PyIter_Check, a function (where it is only a macro
    there is no such symbol) (ctypes)"""
    return _exported('PyIter_Check')


@probe
def PRIVATE_NUMBER_INDEX():
    """libpython exports _PyNumber_Index (ctypes)"""
    return _exported('_PyNumber_Index')


@probe
def FUNCTION_STR():
    """libpython exports _PyObject_FunctionStr (ctypes)"""
    return _exported('_PyObject_FunctionStr')


@probe
def FASTCALL_DICT_TSTATE():
    """libpython exports _PyObject_FastCallDictTstate (ctypes)"""
    return _exported('_PyObject_FastCallDictTstate')


# --- what the bytecode and its instructions do -----------------------------

@probe
def CMP_OUTCOME():
    """COMPARE_OP's arguments include is, in and exception match
    (opcode.cmp_op)"""
    ops = tuple(opcode.cmp_op)
    if 'is' in ops and 'exception match' in ops:
        return True, 'opcode.cmp_op %r' % (ops,)
    if '==' in ops and 'is' not in ops and 'exception match' not in ops:
        return False, 'opcode.cmp_op %r' % (ops,)
    raise Unanswered('opcode.cmp_op is %r' % (ops,))


@probe
def MAP_ADD_VALUE_ON_TOP():
    """a dict comprehension pushes the value after the key, so MAP_ADD
    finds the value on top: the instruction before MAP_ADD loads the
    value (dis)"""
    def walk(code):
        yield code
        for c in code.co_consts:
            if isinstance(c, types.CodeType):
                for inner in walk(c):
                    yield inner
    for code in walk(compile('{kkk: vvv for x in y}', '<probe>', 'eval')):
        ins = list(dis.get_instructions(code))
        for i, x in enumerate(ins):
            if x.opname == 'MAP_ADD' and i > 0:
                last = ins[i - 1].argval
                if last in ('kkk', 'vvv'):
                    return last == 'vvv', 'dis: %s %s, MAP_ADD' % (
                        ins[i - 1].opname, last)
                raise Unanswered('MAP_ADD follows %s %r'
                                 % (ins[i - 1].opname, last))
    raise Unanswered('a dict comprehension has no MAP_ADD')


@probe
def JUMP_IN_INSTRUCTIONS():
    """a JUMP_FORWARD over two NOPs lands where an argument counting
    instructions, not bytes, takes it (run)"""
    unit = _jump_unit()
    return unit == 2, 'JUMP_FORWARD 4 run: the argument counts %s' % (
        'instructions' if unit == 2 else 'bytes')


@probe
def POP_BLOCK_UNWINDS():
    """POP_BLOCK drops a value pushed after its SETUP_FINALLY: run
    LOAD_CONST A, SETUP_FINALLY, LOAD_CONST B, POP_BLOCK, RETURN_VALUE
    and see whether A or B is returned (run); no POP_BLOCK at all is no"""
    if not _real_op('POP_BLOCK'):
        return False, 'opcode: no POP_BLOCK instruction'
    _setup_finally_ops()
    f = _code([('LOAD_CONST', 0), ('SETUP_FINALLY', 0), ('LOAD_CONST', 1),
               ('POP_BLOCK', 0), ('RETURN_VALUE', 0)], ['A', 'B'])
    got = f()
    return got == 'A', 'POP_BLOCK run: returned %s' % got


def _pop_except_probe():
    """Run: SETUP_FINALLY to a handler, raise, and in the handler pop the
    exception, push A and POP_EXCEPT; then return sys.exc_info()[0]. An
    unwinding POP_EXCEPT drops A and restores the outer exception (a
    KeyError); one that doesn't takes A as the exception type."""
    unit = _jump_unit()
    exc_type = lambda: sys.exc_info()[0]   # noqa: E731
    # 0 SETUP_FINALLY ->6, 2 LOAD_CONST E, 4 RAISE_VARARGS 1,
    # 6 POP_TOP x3, 12 LOAD_CONST A, 14 POP_EXCEPT, 16 LOAD_CONST exc_type,
    # 18 CALL_FUNCTION 0, 20 RETURN_VALUE
    f = _code([('SETUP_FINALLY', 4 // unit), ('LOAD_CONST', 0),
               ('RAISE_VARARGS', 1), ('POP_TOP', 0), ('POP_TOP', 0),
               ('POP_TOP', 0), ('LOAD_CONST', 1), ('POP_EXCEPT', 0),
               ('LOAD_CONST', 2), ('CALL_FUNCTION', 0), ('RETURN_VALUE', 0)],
              [ValueError('inner'), 'A', exc_type])
    try:
        raise KeyError('outer')
    except KeyError:
        got = f()
        result = got is KeyError, got == 'A'
    return result


@probe
def POP_EXCEPT_UNWINDS():
    """POP_EXCEPT drops a value pushed in its handler before restoring the
    outer exception (run: see _pop_except_probe); no SETUP_FINALLY
    instruction, so no block for it to unwind to, is no"""
    if not _real_op('SETUP_FINALLY'):
        return False, 'opcode: no SETUP_FINALLY instruction'
    _setup_finally_ops()
    unwinds, took_a = _pop_except_probe()
    if unwinds == took_a:
        raise Unanswered('POP_EXCEPT run: neither the outer exception nor A')
    return unwinds, 'POP_EXCEPT run: %s' % (
        'outer exception restored' if unwinds else 'A taken as the exception')


@probe
def KWARGS_MERGE_EX():
    """CALL_FUNCTION_EX given a mapping whose keys() repeats a key reports
    the repeat (TypeError "multiple values"), as _PyDict_MergeEx does,
    rather than keeping the last value as PyDict_Update does (run)"""
    _need_ops('CALL_FUNCTION_EX', 'CALL_FUNCTION', 'LOAD_CONST', 'RETURN_VALUE')

    class M(object):
        def keys(self):
            return ['a', 'a']

        def __getitem__(self, key):
            return 1

    def f(**kw):
        return kw
    call = _code([('LOAD_CONST', 0), ('LOAD_CONST', 1), ('LOAD_CONST', 2),
                  ('CALL_FUNCTION_EX', 1), ('RETURN_VALUE', 0)],
                 [f, (), M()])
    e = _error(call)
    if e is None:
        return False, 'CALL_FUNCTION_EX run: a repeated key is kept'
    if isinstance(e, TypeError) and 'multiple values' in str(e):
        return True, 'CALL_FUNCTION_EX run: %s' % e
    raise Unanswered('CALL_FUNCTION_EX run: %r' % e)


@probe
def DELETE_GLOBAL_KEYERROR_ONLY():
    """`del` of a global whose lookup raises RuntimeError (a key in the
    globals with the same hash and an __eq__ that raises) gives that
    RuntimeError, not NameError (run)"""
    class Clash(object):
        def __hash__(self):
            return hash('zzz')

        def __eq__(self, other):
            raise RuntimeError('clash')
    g = {'__builtins__': __builtins__}
    exec('def f():\n global zzz\n del zzz\n', g)
    g[Clash()] = 1
    e = _error(g['f'])
    if isinstance(e, RuntimeError):
        return True, 'DELETE_GLOBAL run: RuntimeError'
    if isinstance(e, NameError):
        return False, 'DELETE_GLOBAL run: NameError'
    raise Unanswered('DELETE_GLOBAL run: %r' % e)


@probe
def AENTER_LOOKUP_FIRST():
    """`async with` an object with neither __aenter__ nor __aexit__ fails
    on __aenter__ (run)"""
    class X(object):
        pass

    async def c():
        async with X():
            pass
    co = c()
    try:
        e = _error(co.send, None)
    finally:
        co.close()
    msg = str(e)
    if isinstance(e, AttributeError) and ('__aenter__' in msg) != ('__aexit__' in msg):
        return '__aenter__' in msg, 'async with run: AttributeError %s' % msg
    raise Unanswered('async with run: %r' % e)


@probe
def CALL_TRACE_METHOD_DESCR():
    """a method descriptor called with f(*args) gives the profiler a
    c_call event (sys.setprofile)"""
    events = []

    def prof(frame, event, arg):
        if frame.f_code.co_name == 'g' and event == 'c_call':
            events.append(arg)
    up = str.upper
    args = ('x',)

    def g():
        return up(*args)
    sys.setprofile(prof)
    try:
        g()
    finally:
        sys.setprofile(None)
    seen = [a for a in events if getattr(a, '__name__', None) == 'upper']
    return bool(seen), 'sys.setprofile: %d c_call for str.upper(*args)' % len(seen)


@probe
def NAME_ERROR_NAME():
    """a NameError from ceval has the missing name in .name (run)"""
    e = _error(eval, 'undefined_zzz', {})
    if not isinstance(e, NameError):
        raise Unanswered('no NameError: %r' % e)
    return getattr(e, 'name', None) == 'undefined_zzz', 'NameError.name'


@probe
def IMPORT_FROM_SPEC():
    """`from m import x`, for a module whose __spec__ says it is still
    initialising, says it is "partially initialized" (run)"""
    name = '_le_probe_initialising'
    m = types.ModuleType(name)
    m.__spec__ = types.SimpleNamespace(name=name, _initializing=True)
    m.__file__ = name + '.py'
    sys.modules[name] = m
    try:
        e = _error(exec, 'from %s import missing_zzz' % name, {})
    finally:
        del sys.modules[name]
    if not isinstance(e, ImportError):
        raise Unanswered('from-import run: %r' % e)
    return 'partially initialized' in str(e), 'ImportError: %s' % e


@probe
def LINENO_WHEN_SET():
    """a frame given an f_trace while no trace function runs reports the
    line it is on, not the line where f_trace was set (frame.f_lineno)"""
    def g():
        f = sys._getframe()
        f.f_trace = lambda *a: None
        n = f.f_lineno
        n = f.f_lineno
        f.f_trace = None
        return n, f.f_code.co_firstlineno
    if sys.gettrace() is not None:
        raise Unanswered('a trace function is set')
    n, first = g()
    if n == first + 4:
        return True, 'frame.f_lineno: the current line'
    if n == first + 2:
        return False, 'frame.f_lineno: the line where f_trace was set'
    raise Unanswered('frame.f_lineno %d, the function starts at %d' % (n, first))


@probe
def LASTI_IN_INSTRUCTIONS():
    """the frame's int field that follows frame.f_lasti (in bytes, as
    Python sees it) holds it in instructions (ctypes, reading the frame
    object's memory at three points)"""
    samples = []

    def snap():
        f = sys._getframe(1)
        size = type(f).__basicsize__
        samples.append((f.f_lasti, ctypes.string_at(id(f), size)))
    pad = ['x = 1'] * 150
    src = '\n'.join(['def g(snap):'] + ['    ' + s for s in
                     pad[:90] + ['snap()'] + pad[:61] + ['snap()']
                     + pad[:133] + ['snap()']])
    g = {}
    exec(src, g)
    g['g'](snap)
    lastis = [s[0] for s in samples]
    if len(set(lastis)) != 3 or any(x % 2 for x in lastis):
        raise Unanswered('f_lasti %r' % lastis)
    found = {1: [], 2: []}
    size = min(len(s[1]) for s in samples)
    for off in range(0, size - 3, 4):
        vals = [int.from_bytes(s[1][off:off + 4], sys.byteorder, signed=True)
                for s in samples]
        for unit in (1, 2):
            if vals == [x // unit for x in lastis]:
                found[unit].append(off)
    if bool(found[1]) == bool(found[2]):
        raise Unanswered('no single frame field follows f_lasti %r: %r'
                         % (lastis, found))
    unit = 2 if found[2] else 1
    return unit == 2, 'frame memory: f_lasti %r is %r at offset %d' % (
        lastis, [x // unit for x in lastis], found[unit][0])


# --- what objects and functions do -----------------------------------------

@probe
def FLOAT_ASDOUBLE_INDEX():
    """math.sqrt, which converts with PyFloat_AsDouble, accepts an object
    with only __index__ (run)"""
    import math

    class I(object):
        def __index__(self):
            return 4
    e = _error(math.sqrt, I())
    if e is None:
        return True, 'math.sqrt(__index__ only) works'
    if isinstance(e, TypeError):
        return False, 'math.sqrt(__index__ only): TypeError'
    raise Unanswered('math.sqrt: %r' % e)


@probe
def FRAME_BUILTINS_OF_FUNCTION():
    """a function's frame keeps the builtins its globals had when the
    function was made, after its globals' __builtins__ is replaced (run)"""
    g = {'__builtins__': {'len': lambda x: 'made'}}
    exec('def f():\n return len(1)\n', g)
    g['__builtins__'] = {'len': lambda x: 'replaced'}
    got = g['f']()
    return got == 'made', 'a call after replacing __builtins__ used the %s ones' % got


def _flags_offset(cls):
    """The offset of tp_flags in the type object cls, found as the one
    unsigned long there equal to cls.__flags__."""
    size = ctypes.sizeof(ctypes.c_ulong)
    raw = ctypes.string_at(id(cls), type(cls).__basicsize__)
    hits = [off for off in range(0, len(raw) - size + 1, size)
            if int.from_bytes(raw[off:off + size], sys.byteorder) == cls.__flags__]
    if len(hits) != 1:
        raise Unanswered('tp_flags is not one word in the type object: %r' % hits)
    return hits[0]


@probe
def IMMUTABLE_TYPE_FLAG():
    """a class given the flag 1 << 8 (Py_TPFLAGS_IMMUTABLETYPE) refuses
    attribute assignment and __class__ assignment of its instances
    (ctypes sets the flag; run)"""
    class C(object):
        pass

    class D(object):
        pass
    flags = ctypes.c_ulong.from_address(id(C) + _flags_offset(C))
    old = flags.value
    flags.value = old | (1 << 8)
    try:
        a = isinstance(_error(setattr, C, 'x', 1), TypeError)
        b = isinstance(_error(setattr, C(), '__class__', D), TypeError)
    finally:
        flags.value = old
    if a != b:
        raise Unanswered('setattr refused %s, __class__ assignment %s' % (a, b))
    return a, 'with 1 << 8 set: setattr and __class__ assignment %s' % (
        'refused' if a else 'allowed')


@probe
def VALID_VERSION_TAG_FLAG():
    """a class has the flag 1 << 19 (Py_TPFLAGS_VALID_VERSION_TAG) after a
    lookup on it, and loses it when the class changes (run)"""
    class C(object):
        pass
    getattr(C, 'x', None)
    looked = bool(C.__flags__ & (1 << 19))
    C.x = 1
    changed = bool(C.__flags__ & (1 << 19))
    if looked and changed:
        raise Unanswered('1 << 19 stayed set after the class changed')
    return looked, 'after a lookup on a class, 1 << 19 is %s' % (
        'set' if looked else 'not set')


@probe
def TERNARY_IOP():
    """`x **= y` falls back to y.__rpow__ when x.__ipow__ returns
    NotImplemented (run)"""
    class A(object):
        def __ipow__(self, other):
            return NotImplemented

    class B(object):
        def __rpow__(self, other):
            return 'rpow'
    a = A()
    try:
        a **= B()
    except TypeError:
        return False, '**= run: TypeError'
    if a != 'rpow':
        raise Unanswered('**= run: %r' % a)
    return True, '**= run: __rpow__'


@probe
def NOT_A_SEQUENCE_ERROR():
    """PySequence_GetItem on a dict says it "is not a sequence" (ctypes
    call)"""
    f = ctypes.pythonapi.PySequence_GetItem
    f.argtypes = (ctypes.py_object, ctypes.c_ssize_t)
    f.restype = ctypes.py_object
    e = _error(f, {}, 0)
    msg = str(e)
    if isinstance(e, TypeError) and 'is not a sequence' in msg:
        return True, 'PySequence_GetItem({}): %s' % msg
    if isinstance(e, TypeError) and 'does not support indexing' in msg:
        return False, 'PySequence_GetItem({}): %s' % msg
    raise Unanswered('PySequence_GetItem({}): %r' % e)


@probe
def CONTAINS_ITEM_FIRST():
    """`x in (y,)` calls y.__eq__ before x.__eq__ (run)"""
    order = []

    class A(object):
        def __eq__(self, other):
            order.append('value')
            return NotImplemented

    class B(object):
        def __eq__(self, other):
            order.append('item')
            return NotImplemented
    A() in (B(),)
    if order not in (['value', 'item'], ['item', 'value']):
        raise Unanswered('__eq__ calls %r' % order)
    return order[0] == 'item', '`in` called %s.__eq__ first' % order[0]


@probe
def FORMATCHAR_INDEX_ONLY():
    """'%c' % 2**100 is an OverflowError (out of range), not a TypeError
    (run)"""
    e = _error(lambda: '%c' % (2 ** 100))
    if isinstance(e, OverflowError):
        return True, "'%c' % 2**100: OverflowError"
    if isinstance(e, TypeError):
        return False, "'%c' % 2**100: TypeError"
    raise Unanswered("'%%c' %% 2**100: %r" % e)


@probe
def HASH_DOUBLE_OBJECT():
    """two NaN floats hash differently (by object), not both to 0 (run)"""
    a, b = float('nan'), float('nan')
    if hash(a) == hash(b) == 0:
        return False, 'hash(nan) is 0'
    if hash(a) != hash(b):
        return True, 'hash(nan) differs between NaNs'
    raise Unanswered('hash(nan) is %d for both' % hash(a))


@probe
def MEMBER_EX_ERROR_FROM_TYPE():
    """reading an unset slot (a T_OBJECT_EX member) says "'S' object has
    no attribute", not just the name (run)"""
    class S(object):
        __slots__ = ('zzz',)
    e = _error(getattr, S(), 'zzz')
    if not isinstance(e, AttributeError):
        raise Unanswered('unset slot: %r' % e)
    msg = str(e)
    if msg == 'zzz':
        return False, 'unset slot: AttributeError %s' % msg
    if "'S' object has no attribute" in msg:
        return True, 'unset slot: AttributeError %s' % msg
    raise Unanswered('unset slot: AttributeError %s' % msg)


@probe
def POW_KEYWORDS():
    """pow(base=2, exp=3) works (run)"""
    e = _error(lambda: pow(base=2, exp=3))
    if e is None:
        return True, 'pow(base=, exp=) works'
    if isinstance(e, TypeError):
        return False, 'pow(base=, exp=): TypeError'
    raise Unanswered('pow(base=, exp=): %r' % e)


def _caches():
    """{name: code units of inline cache after the instruction} (from 3.11:
    a list by opcode to 3.12, a dict by name from 3.13)"""
    entries = getattr(opcode, '_inline_cache_entries', {})
    if isinstance(entries, list):
        entries = {opcode.opname[op]: n for op, n in enumerate(entries)}
    return {name: n for name, n in entries.items() if n}


def main(argv):
    names = set(argv)
    answers = {}
    for f in PROBES:
        if names and f.__name__ not in names:
            continue
        try:
            value, evidence = f()
            answers[f.__name__] = [int(bool(value)), evidence]
        except Unanswered as e:
            answers[f.__name__] = {'unanswered': str(e)}
        except Exception as e:
            answers[f.__name__] = {'unanswered': 'the probe failed: %r' % e}
    out = {
        'version': sys.version.split()[0],
        'include': sysconfig.get_paths()['include'],
        'opcodes': dict(opcode.opmap),
        'hasjrel': [[opcode.opname[n], n] for n in opcode.hasjrel],
        'hasjabs': [[opcode.opname[n], n] for n in opcode.hasjabs],
        'caches': _caches(),
        'answers': answers,
    }
    sys.stdout.write(json.dumps(out) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
