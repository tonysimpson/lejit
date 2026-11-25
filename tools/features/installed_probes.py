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
    constants consts. Each instruction is followed by its inline caches,
    zeroed, which a jump's argument counts."""
    caches = _caches()
    raw = b''.join(bytes([opcode.opmap[name], arg])
                   + bytes(2 * caches.get(name, 0)) for name, arg in ops)
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
def CTYPES_CALL_AUDITED():
    """a call of a ctypes function raises the audit event
    ctypes.call_function (run)"""
    if not hasattr(sys, 'addaudithook'):
        return False, 'no sys.addaudithook'
    seen = []
    sys.addaudithook(lambda event, args: event == 'ctypes.call_function'
                     and seen.append(args))
    ctypes.CDLL(None).abs(-3)
    return bool(seen), 'CDLL(None).abs(-3) audited: %s' % bool(seen)


@probe
def ARG_CHECK_POSITIONAL():
    """libpython exports _PyArg_CheckPositional (ctypes)"""
    return _exported('_PyArg_CheckPositional')


@probe
def DEALLOC_HOOKS():
    """libpython exports PyRefTracer_SetTracer (ctypes)"""
    return _exported('PyRefTracer_SetTracer')


@probe
def OBJECTDICT_SETITEM_LINKABLE():
    """libpython exports _PyObjectDict_SetItem, so an extension can link
    to it (ctypes)"""
    return _exported('_PyObjectDict_SetItem')


@probe
def CODE_GET_CODE():
    """libpython exports PyCode_GetCode (ctypes)"""
    return _exported('PyCode_GetCode')


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
def COROUTINE_WRAPPER():
    """sys.set_coroutine_wrapper exists, kept in the thread state"""
    return hasattr(sys, 'set_coroutine_wrapper'), 'sys.set_coroutine_wrapper'


@probe
def ITER_SEND():
    """libpython exports PyIter_Send (ctypes)"""
    return _exported('PyIter_Send')


@probe
def FASTCALL_DICT_TSTATE():
    """libpython exports _PyObject_FastCallDictTstate (ctypes)"""
    return _exported('_PyObject_FastCallDictTstate')


# C API functions that LeJIT calls where libpython exports them (as
# features.py's EXPORTS)
for _feature, _function in [
        ('DICT_MERGE_EX_LINKABLE', '_PyDict_MergeEx'),
        ('HASH_BYTES_LINKABLE', '_Py_HashBytes'),
        ('PRIVATE_IS_FINALIZING_LINKABLE', '_Py_IsFinalizing'),
        ('LOOKUP_ATTR_LINKABLE', '_PyObject_LookupAttr'),
        ('FRAME_CLEAR_AND_POP_LINKABLE', '_PyEval_FrameClearAndPop'),
        ('HANDLE_PENDING_LINKABLE', '_Py_HandlePending'),
        ('SUPER_LOOKUP_LINKABLE', '_PySuper_Lookup'),
        ('LAZY_IMPORT_REIFY_LINKABLE', '_PyLazyImport_Reify')]:
    def _export_probe(function=_function):
        return _exported(function)
    _export_probe.__name__ = _feature
    _export_probe.__doc__ = 'libpython exports %s (ctypes)' % _function
    probe(_export_probe)


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
def CODE_POSONLY_ARGS():
    """code objects have co_posonlyargcount"""
    return (hasattr((lambda: None).__code__, 'co_posonlyargcount'),
            'code.co_posonlyargcount')


@probe
def CODE_QUALNAME():
    """code objects have co_qualname"""
    return (hasattr((lambda: None).__code__, 'co_qualname'),
            'code.co_qualname')


@probe
def EXCEPTION_TABLE():
    """code objects have co_exceptiontable"""
    return (hasattr((lambda: None).__code__, 'co_exceptiontable'),
            'code.co_exceptiontable')


def _match_pushes_bool(name, with_bool, without):
    """(name's stack effect is with_bool, evidence); False where there is
    no such instruction"""
    if not _real_op(name):
        return False, 'opcode.opmap has no %s' % name
    op = opcode.opmap[name]
    effect = dis.stack_effect(op, 0) if op >= opcode.HAVE_ARGUMENT \
        else dis.stack_effect(op)
    if effect not in (with_bool, without):
        raise Unanswered('dis.stack_effect(%s, 0) is %d' % (name, effect))
    return effect == with_bool, 'dis.stack_effect(%s, 0) == %d' % (name, effect)


@probe
def MATCH_CLASS_PUSHES_BOOL():
    """MATCH_CLASS leaves two values for its three (a bool over the
    attributes or the subject), not one (dis.stack_effect)"""
    return _match_pushes_bool('MATCH_CLASS', -1, -2)


@probe
def MATCH_KEYS_PUSHES_BOOL():
    """MATCH_KEYS pushes two values (the values or None, and a bool), not
    one (dis.stack_effect)"""
    return _match_pushes_bool('MATCH_KEYS', 2, 1)


@probe
def MONITORING():
    """sys.monitoring exists"""
    return hasattr(sys, 'monitoring'), 'sys.monitoring'


@probe
def KWARGS_MERGE_EX():
    """CALL_FUNCTION_EX given a dict subclass whose keys() repeats a key
    reports the repeat (TypeError "multiple values"), as _PyDict_MergeEx
    does, rather than keeping the last value as PyDict_Update does, or
    passing the dict on as it is, as where DICT_MERGE has made every
    mapping a dict (run)"""
    _need_ops('CALL_FUNCTION_EX', 'LOAD_CONST', 'RETURN_VALUE')
    # the callable is pushed with CALL_FUNCTION, or (from 3.11) with a
    # NULL beside it, on the side the compiler puts it
    if _real_op('CALL_FUNCTION'):
        start = [('LOAD_CONST', 0)]
    elif _real_op('PUSH_NULL'):
        def g(f, a, k):
            return f(*a, **k)
        ops = [x.opname for x in dis.get_instructions(g)]
        first_load = [o for o in ops if o.startswith('LOAD_FAST')][0]
        if ops.index('PUSH_NULL') < ops.index(first_load):
            start = [('PUSH_NULL', 0), ('LOAD_CONST', 0)]
        else:
            start = [('LOAD_CONST', 0), ('PUSH_NULL', 0)]
    else:
        raise Unanswered('the probe needs opcode CALL_FUNCTION or PUSH_NULL,'
                         ' which this interpreter lacks')

    # a dict, which CALL_FUNCTION_EX may take as it is; its own __iter__
    # makes a merge read keys()
    class M(dict):
        def keys(self):
            return ['a', 'a']

        def __iter__(self):
            return iter(self.keys())

        def __getitem__(self, key):
            return 1

    def f(**kw):
        return kw
    call = _code(start + [('LOAD_CONST', 1), ('LOAD_CONST', 2),
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
def UNPACK_KEEPS_ATTRIBUTE_ERROR():
    """{**x} and f(**x) of an x whose keys() raises AttributeError keep it
    (run)"""
    class Keys(object):
        def keys(self):
            raise AttributeError('keys')

    def f(**kw):
        return kw
    kinds = {type(_error(lambda: {**Keys()})), type(_error(lambda: f(**Keys())))}
    if kinds == {AttributeError}:
        return True, '{**x}, f(**x) run: AttributeError'
    if kinds == {TypeError}:
        return False, '{**x}, f(**x) run: TypeError'
    raise Unanswered('{**x}, f(**x) run: %r' % kinds)


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
def BREAKER_SKIPPED_AFTER_LOAD_SPECIAL():
    """a signal that __enter__ (a C call, LOAD_SPECIAL's) marks as arrived
    is handled inside the with block, where __exit__ sees its exception,
    not as the call ends (run)"""
    if not _real_op('LOAD_SPECIAL'):
        return False, 'no LOAD_SPECIAL'
    import _thread, functools, signal

    class Raised(Exception):
        pass

    def handler(signum, frame):
        raise Raised()

    exited = []

    class Manager:
        # (os.kill would run the handler itself)
        __enter__ = staticmethod(functools.partial(_thread.interrupt_main,
                                                   signal.SIGUSR1))

        def __exit__(self, *exc):
            exited.append(exc[0])

    old = signal.signal(signal.SIGUSR1, handler)
    try:
        try:
            with Manager():
                len(())
        except Raised:
            pass
        else:
            raise Unanswered('the handler raised nothing')
    finally:
        signal.signal(signal.SIGUSR1, old)
    return bool(exited), '__exit__ saw %r' % exited


def _table_entries(code):
    """code's exception table: [start, end, target, depth << 1 | lasti],
    in code units"""
    entries, item, entry = [], 0, []
    for byte in code.co_exceptiontable:
        item = item << 6 | byte & 63
        if not byte & 64:
            entry.append(item)
            item = 0
            if len(entry) == 4:
                entry[1] += entry[0]
                entries.append(entry)
                entry = []
    return entries


def _table(entries):
    """the exception table of entries, as _table_entries gives them"""
    out = bytearray()
    for start, end, target, depth_lasti in entries:
        for n, item in enumerate((start, end - start, target, depth_lasti)):
            chunks = []
            while True:
                chunks.insert(0, item & 63)
                item >>= 6
                if not item:
                    break
            for i, chunk in enumerate(chunks):
                out.append(chunk | (64 if i < len(chunks) - 1 else 0)
                           | (128 if n == 0 and i == 0 else 0))
    return bytes(out)


@probe
def BREAKER_AFTER_BACK_JUMP():
    """a signal handled at a loop's backward jump is caught by an exception
    table entry for the code unit before the loop head alone (run: a loop
    whose table is cut to that entry, with another thread sending the
    signal)"""
    if not _real_op('JUMP_BACKWARD'):
        return False, 'no JUMP_BACKWARD'
    import _thread, signal, threading, time

    class Raised(Exception):
        pass

    def handler(signum, frame):
        raise Raised()

    def loop():
        try:
            while True:
                pass
        except Raised:
            return 'caught'

    code = loop.__code__
    jumps = [i for i in dis.get_instructions(code)
             if i.opname == 'JUMP_BACKWARD']
    if len(jumps) != 1:
        raise Unanswered('the loop has %d JUMP_BACKWARD' % len(jumps))
    head = jumps[0].argval // 2
    entries = _table_entries(code)
    body = [e for e in entries if e[0] <= head < e[1]]
    if len(body) != 1 or head == 0:
        raise Unanswered('no one exception table entry covers the loop head')
    handler_at = body[0][2]
    cut = [[head - 1, head, handler_at, body[0][3]]] + [
        e for e in entries if e[0] >= handler_at]
    f = types.FunctionType(code.replace(co_exceptiontable=_table(cut)),
                           globals(), 'loop', None, loop.__closure__)
    old = signal.signal(signal.SIGUSR1, handler)
    sender = threading.Thread(
        target=lambda: (time.sleep(0.01), _thread.interrupt_main(signal.SIGUSR1)))
    try:
        sender.start()
        try:
            result = f()
        except Raised:
            result = 'raised from the loop'
    finally:
        sender.join()
        signal.signal(signal.SIGUSR1, old)
    return result == 'caught', result


@probe
def RAISE_CAUSE_CHECKED():
    """raise ... from a class whose call returns None is a TypeError
    (run)"""
    class NoCause(BaseException):
        def __new__(cls):
            return None
    e = _error(exec, 'raise IndexError from C', {'C': NoCause})
    if not isinstance(e, (TypeError, IndexError)):
        raise Unanswered('raise run: %r' % e)
    return isinstance(e, TypeError), '%s: %s' % (type(e).__name__, e)


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
def FRAME_CLEARED_LAST_FIRST():
    """a function's three locals are released last to first when it
    returns (run: their finalisers)"""
    order = []

    class Handle:
        def __init__(self, name):
            self.name = name

        def __del__(self):
            order.append(self.name)

    def f():
        a = Handle('a')
        b = Handle('b')
        c = Handle('c')
        return a is b is c
    f()
    if order not in (['a', 'b', 'c'], ['c', 'b', 'a']):
        raise Unanswered('locals released in the order %r' % order)
    return order == ['c', 'b', 'a'], 'run: locals released %r' % order


@probe
def GETFRAME_AUDITS_RESULT():
    """sys._getframe(1)'s audit event names the caller's frame, which it
    returns, not the current one (run: an audit hook)"""
    if not hasattr(sys, 'addaudithook'):
        return False, 'no sys.addaudithook'
    seen = []

    def hook(event, args):
        if event == 'sys._getframe' and seen is not None:
            seen.append(args[0])
    sys.addaudithook(hook)

    def inner():
        return sys._getframe(1), sys._getframe(0)
    try:
        caller, current = inner()
        named = seen[-2]
    finally:
        seen = None
    if named is not caller and named is not current:
        raise Unanswered('the audit event named neither frame: %r' % named)
    return named is caller, 'run: the event named the %s frame' % (
        'returned' if named is caller else 'current')


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
def LASTI_PAST_PREFIXES():
    """a traceback's tb_lasti for an instruction with an EXTENDED_ARG prefix
    (a local numbered 300, unbound) is the instruction's own unit, not the
    prefix"""
    src = ('def f():\n' + ''.join('    v%d = 0\n' % i for i in range(301))
           + '    del v300\n    return v300\n')
    g = {}
    exec(src, g)
    try:
        g['f']()
    except NameError as e:
        lasti = e.__traceback__.tb_next.tb_lasti
    else:
        raise Unanswered('reading an unbound local raised nothing')
    code = g['f'].__code__.co_code
    prefix = opcode.opmap['EXTENDED_ARG']
    if code[lasti] == prefix:
        return False, 'tb_lasti %d: the EXTENDED_ARG prefix' % lasti
    if lasti >= 2 and code[lasti - 2] == prefix:
        return True, 'tb_lasti %d: the instruction after its prefix' % lasti
    raise Unanswered('tb_lasti %d is neither a prefix nor after one' % lasti)


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
def MANAGED_DICT():
    """a plain class's instances keep their dict at a negative offset
    (__dictoffset__ < 0), where the interpreter manages it"""
    class C(object):
        pass
    return C.__dictoffset__ < 0, 'class C: __dictoffset__ == %d' % C.__dictoffset__


@probe
def MANAGED_VALUES_POINTER():
    """an instance with an attribute and no __dict__ made yet has a values
    pointer four words before it and no dict three words before; once
    __dict__ is made, the values pointer is gone and the dict is there"""
    class C(object):
        pass
    c = C()
    c.a = 1
    word = ctypes.c_void_p.from_address
    before = word(id(c) - 32).value, word(id(c) - 24).value
    d = c.__dict__
    after = word(id(c) - 32).value, word(id(c) - 24).value
    found = (C.__dictoffset__ < 0 and before[0] is not None and
             before[1] is None and after == (None, id(d)))
    return found, 'class C: words -4 and -3 %r, then with __dict__ %r' % (
        before, after)


@probe
def TAGGED_DICT_OR_VALUES():
    """an instance with an attribute and no __dict__ made yet has, three
    words before it, its values pointer less one (the low bit set); once
    __dict__ is made, the dict is there"""
    class C(object):
        pass
    if C.__dictoffset__ >= 0:
        return False, 'class C: __dictoffset__ == %d' % C.__dictoffset__
    c = C()
    c.a = 1000
    word = ctypes.c_void_p.from_address
    before = word(id(c) - 24).value
    tagged = before is not None and before & 1 == 1
    first = tagged and word(before + 1).value == id(c.a)
    d = c.__dict__
    after = word(id(c) - 24).value
    found = tagged and first and after == id(d)
    if tagged and not found:
        raise Unanswered('class C: word -3 %#x, values[0] %r, then with'
                         ' __dict__ %r' % (before, first, after))
    return found, 'class C: word -3 %r, then with __dict__ %r' % (
        before, after)


@probe
def RANGE_ITER_INDEX():
    """an iterator over range(1, 7, 2) that has given one item pickles as the
    whole range and the index 1, not as range(3, 7, 2) with no index
    (rangeiter_reduce)"""
    it = iter(range(1, 7, 2))
    next(it)
    reduced = it.__reduce__() + (None,)
    seen = (reduced[1][0], reduced[2])
    if seen == (range(1, 7, 2), 1):
        return True, 'iter(range(1, 7, 2)) after one item: %r' % (seen,)
    if seen == (range(3, 7, 2), None):
        return False, 'iter(range(1, 7, 2)) after one item: %r' % (seen,)
    raise Unanswered('iter(range(1, 7, 2)) after one item: %r' % (seen,))


@probe
def INLINE_VALUES():
    """a plain class has the flag 1 << 2 (Py_TPFLAGS_INLINE_VALUES), and
    its instance with an attribute keeps it in the object, a word after
    its basicsize, with the byte three after the basicsize set (valid) and
    no dict three words before; the next byte (size) counts the attributes,
    and their indices follow the values, as many words as the first byte
    (capacity) says, in the order they were added; once __dict__ is made it
    is there, and the values stay valid until a __class__ assignment"""
    class C(object):
        pass

    class D(object):
        pass
    if not C.__flags__ & (1 << 2):
        return False, 'class C: no 1 << 2 in its flags'
    c = C()
    c.a = 1000
    c.b = 1001
    e = C()
    e.b = 1002
    e.a = 1003
    word = ctypes.c_void_p.from_address
    byte = ctypes.c_uint8.from_address
    at = id(c) + C.__basicsize__
    valid = byte(at + 3)
    order = []
    for o in (c, e):
        values = id(o) + C.__basicsize__
        order.append([byte(values + 8 + 8 * byte(values).value + i).value
                      for i in range(byte(values + 1).value)])
    if order != [[0, 1], [1, 0]]:
        return False, 'class C: insertion orders %r' % order
    seen = [(valid.value, word(at + 8).value == id(c.a), word(id(c) - 24).value)]
    d = c.__dict__
    seen.append((valid.value, word(at + 8).value == id(c.a), word(id(c) - 24).value == id(d)))
    c.__class__ = D
    seen.append(valid.value)
    found = seen == [(1, True, None), (1, True, True), 0]
    return found, 'class C: valid, value at +8, dict word %r' % seen


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
def TUPLE_UNTRACKED_AT_BUILD():
    """a tuple built at run time from an object() is not tracked before any
    collection (run)"""
    import gc
    o = object()
    t = (o,)
    return not gc.is_tracked(t), 'gc.is_tracked((object(),)) is %r' % gc.is_tracked(t)


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
    # the type's name, or (from 3.13) its qualified name
    if "S' object has no attribute" in msg:
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


@probe
def SUM_COMPENSATED():
    """sum([1e100, 1.0, -1e100]) keeps the 1.0 (run)"""
    r = sum([1e100, 1.0, -1e100])
    if r not in (0.0, 1.0):
        raise Unanswered('sum([1e100, 1.0, -1e100]) is %r' % r)
    return r == 1.0, 'sum([1e100, 1.0, -1e100]) is %r' % r


@probe
def SUM_INT_AS_DOUBLE():
    """sum([1e100, 1, -1e100]) keeps the int 1 (run)"""
    r = sum([1e100, 1, -1e100])
    if r not in (0.0, 1.0):
        raise Unanswered('sum([1e100, 1, -1e100]) is %r' % r)
    return r == 1.0, 'sum([1e100, 1, -1e100]) is %r' % r


@probe
def SUM_COMPLEX_COMPENSATED():
    """sum([1e100j, 1j, -1e100j]) keeps the 1j (run)"""
    r = sum([1e100j, 1j, -1e100j])
    if r not in (0j, 1j):
        raise Unanswered('sum([1e100j, 1j, -1e100j]) is %r' % r)
    return r == 1j, 'sum([1e100j, 1j, -1e100j]) is %r' % r


@probe
def SUM_EXACT_NUMBERS():
    """sum([1e100, True, -1e100]) loses the True, which leaves the float
    loop; where that loop adds no int compensated, it can't be seen, and
    the loop takes a subclass as its int, as it did then (run)"""
    if sum([1e100, 1, -1e100]) == 0.0:
        return False, 'sum([1e100, 1, -1e100]) is 0.0'
    r = sum([1e100, True, -1e100])
    if r not in (0.0, 1.0):
        raise Unanswered('sum([1e100, True, -1e100]) is %r' % r)
    return r == 0.0, 'sum([1e100, True, -1e100]) is %r' % r


@probe
def LOAD_GLOBAL_NULL_BIT():
    """a call of a global loads it with LOAD_GLOBAL whose argument is the
    name's index (0) shifted left once, plus one for the NULL it pushes
    (dis)"""
    def f():
        return zzz()  # noqa: F821
    for x in dis.get_instructions(f):
        if x.opname == 'LOAD_GLOBAL' and x.arg in (0, 1):
            return x.arg == 1, 'dis: LOAD_GLOBAL %d for zzz()' % x.arg
    raise Unanswered('zzz() is not LOAD_GLOBAL 0 or 1: %r'
                     % [(x.opname, x.arg) for x in dis.get_instructions(f)])


@probe
def IMPORT_NAME_LAZY_BITS():
    """an import in a function loads it with IMPORT_NAME whose argument is
    the name's index (0) shifted left twice, plus two for an import that is
    always eager (dis)"""
    def f():
        import zzz  # noqa: F401
    for x in dis.get_instructions(f):
        if x.opname == 'IMPORT_NAME' and x.arg in (0, 2):
            return x.arg == 2, 'dis: IMPORT_NAME %d for import zzz' % x.arg
    raise Unanswered('import zzz is not IMPORT_NAME 0 or 2: %r'
                     % [(x.opname, x.arg) for x in dis.get_instructions(f)])


@probe
def AWAIT_ERROR_BY_ARG():
    """async with's awaits of __aenter__'s and __aexit__'s results are
    GET_AWAITABLE 1 and 2, the argument ceval's error reads (dis)"""
    src = 'async def f(m):\n    async with m:\n        pass\n'
    g = {}
    exec(src, g)
    args = [x.arg for x in dis.get_instructions(g['f'])
            if x.opname == 'GET_AWAITABLE']
    if not args:
        raise Unanswered('async with has no GET_AWAITABLE')
    return args[:2] == [1, 2], 'dis: GET_AWAITABLE %r in async with' % args


@probe
def AWITH_EXIT_FIRST():
    """async with on an object with neither method names __aexit__ as the
    one missing (run)"""
    async def f():
        async with object():
            pass
    try:
        f().send(None)
    except AttributeError as e:
        return str(e) == '__aexit__', 'async with object(): %s' % e
    except TypeError as e:
        return False, 'async with object(): TypeError %s' % e
    raise Unanswered('async with object() raised nothing')


@probe
def LOAD_ATTR_METHOD_BIT():
    """reading an attribute loads it with LOAD_ATTR whose argument is the
    name's index (1) shifted left once (dis)"""
    def f(o):
        return o.yyy, o.zzz
    for x in dis.get_instructions(f):
        if x.opname == 'LOAD_ATTR' and x.argval == 'zzz' and x.arg in (1, 2):
            return x.arg == 2, 'dis: LOAD_ATTR %d for o.zzz' % x.arg
    raise Unanswered('o.zzz is not LOAD_ATTR 1 or 2: %r'
                     % [(x.opname, x.arg) for x in dis.get_instructions(f)])


@probe
def COMPARE_OP_SHIFT_4():
    """a > b compiles to COMPARE_OP whose argument is Py_GT (4), shifted
    left four times where it has the specialiser's bits too (dis)"""
    def f(a, b):
        return a > b
    for x in dis.get_instructions(f):
        if x.opname == 'COMPARE_OP':
            if x.arg == 4:
                return False, 'dis: COMPARE_OP 4 for a > b'
            if x.arg >> 4 == 4:
                return True, 'dis: COMPARE_OP %d for a > b' % x.arg
            if x.arg >> 5 == 4:
                return False, 'dis: COMPARE_OP %d for a > b, shifted five times' % x.arg
            raise Unanswered('a > b is COMPARE_OP %d' % x.arg)
    raise Unanswered('a > b has no COMPARE_OP: %r'
                     % [(x.opname, x.arg) for x in dis.get_instructions(f)])


@probe
def COMPARE_OP_SHIFT_5():
    """a > b compiles to COMPARE_OP whose argument is Py_GT (4) shifted
    left five times, with the specialiser's bits and the bool conversion's
    below (dis)"""
    def f(a, b):
        return a > b
    for x in dis.get_instructions(f):
        if x.opname == 'COMPARE_OP':
            if x.arg == 4 or x.arg >> 4 == 4:
                return False, 'dis: COMPARE_OP %d for a > b' % x.arg
            if x.arg >> 5 == 4:
                return True, 'dis: COMPARE_OP %d for a > b' % x.arg
            raise Unanswered('a > b is COMPARE_OP %d' % x.arg)
    raise Unanswered('a > b has no COMPARE_OP: %r'
                     % [(x.opname, x.arg) for x in dis.get_instructions(f)])


@probe
def CALLABLE_UNDER_NULL():
    """CALL_FUNCTION_EX given two callables under its arguments, and no
    NULL, calls the lower one (run)"""
    _need_ops('CALL_FUNCTION_EX', 'LOAD_CONST', 'RETURN_VALUE')

    def f(g, a):
        return g(*a)
    ops = [x.opname for x in dis.get_instructions(f)]
    # a slot for the keywords, NULL when there are none, where the compiler
    # pushes one
    kwargs = ops[ops.index('CALL_FUNCTION_EX') - 1] == 'PUSH_NULL'
    call = _code([('LOAD_CONST', 0), ('LOAD_CONST', 1), ('LOAD_CONST', 2)] +
                 [('PUSH_NULL', 0)] * kwargs +
                 [('CALL_FUNCTION_EX', 0), ('RETURN_VALUE', 0)],
                 [lambda: 'lower', lambda: 'upper', ()])
    called = call()
    return called == 'lower', 'CALL_FUNCTION_EX run: called the %s' % called


@probe
def FINISHED_GENERATOR_UNLINKED():
    """the frame of a generator that raised has no f_back (run)"""
    def g():
        yield 1
        raise ValueError
    try:
        list(g())
    except ValueError as e:
        back = e.__traceback__.tb_next.tb_frame.f_back
    return back is None, 'f_back of the frame of a generator that raised: %s' % (
        None if back is None else back.f_code.co_name)


@probe
def FOR_ITER_EXIT_PAST_POP_TOP():
    """a for loop ends with END_FOR then POP_TOP, END_FOR popping one value
    (dis, dis.stack_effect)"""
    if not _real_op('END_FOR'):
        return False, 'opcode.opmap: no END_FOR'

    def f(y):
        for x in y:
            pass
    ins = list(dis.get_instructions(f))
    for i, x in enumerate(ins[:-1]):
        if x.opname == 'END_FOR':
            pops = -dis.stack_effect(x.opcode)
            return (pops == 1 and ins[i + 1].opname == 'POP_TOP',
                    'dis: END_FOR (pops %d) then %s' % (pops, ins[i + 1].opname))
    raise Unanswered('a for loop has no END_FOR: %r'
                     % [x.opname for x in ins])


@probe
def GET_ITER_INDEX():
    """GET_ITER pushes two items, the iterator and an index or NULL
    (dis)"""
    op = dis.opmap['GET_ITER']
    pushes = dis.stack_effect(op, 0 if op >= dis.HAVE_ARGUMENT else None) + 1
    return pushes == 2, 'dis: GET_ITER pushes %d' % pushes


@probe
def AWAITABLE_PUSHES_SEND_ARGS():
    """GET_AWAITABLE pushes three items, the iterator, a NULL and None
    (dis)"""
    op = dis.opmap['GET_AWAITABLE']
    pushes = dis.stack_effect(op, 0 if op >= dis.HAVE_ARGUMENT else None) + 1
    return pushes == 3, 'dis: GET_AWAITABLE pushes %d' % pushes


@probe
def ANEXT_PUSHES_SEND_ARGS():
    """GET_ANEXT pushes four items, the async iterator, the awaitable, a
    NULL and None (dis)"""
    op = dis.opmap['GET_ANEXT']
    pushes = dis.stack_effect(op, 0 if op >= dis.HAVE_ARGUMENT else None) + 1
    return pushes == 4, 'dis: GET_ANEXT pushes %d' % pushes


@probe
def RUNNING_STACK_HIDDEN():
    """a running generator's referents (gc) don't include its locals"""
    import gc
    if not hasattr((lambda: None).__code__, 'co_exceptiontable'):
        return False, 'code objects have no co_exceptiontable'
    token = object()

    def gen():
        x = token
        yield token in gc.get_referents(it)
    it = gen()
    seen = next(it)
    return not seen, 'gc.get_referents of the running generator: local %s' % (
        'seen' if seen else 'not seen')


@probe
def TAGGED_LASTI():
    """a coroutine paused in an awaiting __aexit__ that an exception
    called holds the lasti under the exception as no object: the
    coroutine's referents (gc) have no int"""
    import gc
    if not hasattr((lambda: None).__code__, 'co_exceptiontable'):
        return False, 'code objects have no co_exceptiontable'

    class Aw:
        def __await__(self):
            yield
            return False

    class CM:
        async def __aenter__(self):
            pass

        def __aexit__(self, *a):
            return Aw()

    async def f():
        async with CM():
            raise KeyError
    c = f()
    c.send(None)
    ints = [r for r in gc.get_referents(c) if type(r) is int]
    c.close()
    return not ints, 'gc.get_referents: ints %r' % ints


@probe
def FOR_ITER_EXIT_KEEPS_ITER():
    """a for loop ends with END_FOR then POP_ITER (dis)"""
    if not _real_op('POP_ITER'):
        return False, 'opcode.opmap: no POP_ITER'

    def f(y):
        for x in y:
            pass
    ins = list(dis.get_instructions(f))
    for i, x in enumerate(ins[:-1]):
        if x.opname == 'END_FOR':
            return (ins[i + 1].opname == 'POP_ITER',
                    'dis: END_FOR then %s' % ins[i + 1].opname)
    raise Unanswered('a for loop has no END_FOR: %r'
                     % [x.opname for x in ins])


@probe
def CALL_EX_KWARGS_SLOT():
    """CALL_FUNCTION_EX pops as many values without keywords (argument 0)
    as with them (argument 1) (dis.stack_effect)"""
    _need_ops('CALL_FUNCTION_EX')
    without = dis.stack_effect(opcode.opmap['CALL_FUNCTION_EX'], 0)
    with_kw = dis.stack_effect(opcode.opmap['CALL_FUNCTION_EX'], 1)
    return (without == with_kw,
            'dis.stack_effect: CALL_FUNCTION_EX %d without keywords, %d with'
            % (without, with_kw))


@probe
def BINARY_OP_SUBSCR():
    """a[b] compiles to BINARY_OP with NB_SUBSCR (dis)"""
    nb_ops = [name for name, _ in getattr(opcode, '_nb_ops', ())]
    if 'NB_SUBSCR' not in nb_ops:
        return False, 'opcode._nb_ops: no NB_SUBSCR'

    def f(a, b):
        return a[b]
    ops = [(x.opname, x.arg) for x in dis.get_instructions(f)]
    return (('BINARY_OP', nb_ops.index('NB_SUBSCR')) in ops,
            'dis: a[b] is %r' % ops)


@probe
def FUNCTION_ANNOTATE():
    """a function with annotations gets an __annotate__ function (run)"""
    def f(x: int):
        pass
    if getattr(f, '__annotate__', None) is None:
        return False, 'def f(x: int): f has no __annotate__'
    return True, 'def f(x: int): f.__annotate__ is a function'


@probe
def DICT_KEYS_KIND():
    """a dict of 5 str keys is smaller than one of 5 int keys, because its
    entries hold no hash (sys.getsizeof)"""
    strs, ints = {}, {}
    for k in range(5):
        strs[str(k)] = ints[k] = k
    a, b = sys.getsizeof(strs), sys.getsizeof(ints)
    if a == b:
        return False, 'sys.getsizeof: str and int keys both %d' % a
    if a < b:
        return True, 'sys.getsizeof: str keys %d, int keys %d' % (a, b)
    raise Unanswered('sys.getsizeof: str keys %d, int keys %d' % (a, b))


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
        'jback': [[opcode.opname[n], n] for n in opcode.hasjrel
                  if getattr(dis, '_is_backward_jump', lambda op: False)(n)],
        'answers': answers,
    }
    sys.stdout.write(json.dumps(out) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
