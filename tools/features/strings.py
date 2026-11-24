#!/usr/bin/env python3
"""Take the messages CPython shows from the interpreter LeJIT is built for.

Compiled code raises the same exceptions and warnings as CPython, with the
same text, and that text changes between releases. Rather than copying it,
each message is an entry here: a probe that makes the interpreter raise it
and the arguments the message is given. The probe runs in the target
interpreter (a subprocess with -I and a timeout; lejit is never imported) with
arguments chosen to be recognisable, and the C format string is recovered
from what comes back:

- the text is the same from run to run except where the arguments go, so
  two runs with different arguments show where each conversion is;
- a type name or a name taken as a C string is shown cut to its precision
  (%.200s), so a third run with names 1000+ characters long shows it;
- an object shown as %R is shown by repr, %d and the like by their value.

A precision can't be seen for a fixed C string LeJIT passes itself (an
operator's name, say): the entry gives that conversion, and the operator's
name is an entry of its own, a fragment, read from the message the operator
gives. A message the probe can't recover stops the build, never a guess.

    strings.py --python EXE --out DIR
        write DIR/le_strings_gen.h for the interpreter EXE: one
        `#define LE_MSG_<NAME> "..."` per message
    strings.py --python EXE --list
        print each message the interpreter gives

It runs under the Python being built for (3.7 and later), as does what it
runs in the target.
"""
import argparse
import ast
import json
import os
import re
import subprocess
import sys

HEADER = 'le_strings_gen.h'
TIMEOUT = 120


class StringError(Exception):
    """A message the probe couldn't recover: stop the build."""


# ---------------------------------------------------------------------------
# The arguments a message is given, in the order the C call passes them.

def T():
    """A type's name (tp_name), %s with the precision the message has. The
    probe gets a new class with a recognisable name."""
    return {'k': 'T'}


def S():
    """A C string the probe can choose (a variable's or a function's name),
    %s with its precision. The probe gets the name as a str."""
    return {'k': 'S'}


def Q():
    """A type shown by its fully qualified name: the module, a dot and the
    qualified name, the module left out for builtins and __main__. CPython
    passes %T an object or %N its type, which show the same; LeJIT passes
    the type, %N. The probe gets a new class with a recognisable name, as
    for T()."""
    return {'k': 'Q'}


def O(conv):
    """An object: 'U' or 'S' shows a str (the probe gets a recognisable
    one) as it is, 'R' by repr; 'V' is %V, a str or NULL and the text shown
    for NULL, which the call passes as "function". A str shows the same by
    %U, %S and %V, so the entry says which the call passes."""
    return {'k': 'O', 'conv': conv}


def I(conv):
    """A C integer, 'd', 'zd', 'ld', 'x' or 'c'. The probe gets an int (for
    'c', a character's code)."""
    return {'k': 'I', 'conv': conv}


def F(spec, text):
    """A C string LeJIT passes itself, shown as `text` in the probe's case:
    its conversion can't be seen, so it is given."""
    return {'k': 'F', 'spec': spec, 'text': text}


def P():
    """A word ending that depends on a count ('' or 's'); the probe says what
    it shows in shown[i]."""
    return {'k': 'P'}


def D():
    """A double shown by repr, %s of the C string LeJIT makes of it. The probe
    gets an int, makes its double from it and says what it shows in
    shown[i]."""
    return {'k': 'D'}


ENTRIES = []


def msg(name, what, exc, code, args=(), text=False, ints=None, requires=None,
        shapes=None):
    """A message. `code` runs in the target with args (a list, one value per
    argument) and must raise `exc` (or warn it, for a Warning). text: the
    message is set as it is (PyErr_SetString), not as a format. ints: the
    integers of the first and second run, if the defaults won't do.
    requires: an expression the target evaluates; false means the
    interpreter has no such case and the name is left undefined. shapes:
    [(suffix, args, code)] when the message's arguments differ between
    versions; exactly one shape must fit, and its name has the suffix."""
    if shapes is None:
        shapes = [('', list(args), code)]
    ENTRIES.append({'name': name, 'what': what, 'exc': exc, 'text': text,
                    'ints': ints, 'requires': requires,
                    'shapes': [{'suffix': s, 'args': a, 'code': c}
                               for s, a, c in shapes]})


def frag(name, what, parent, hole, exc, code, args):
    """A fragment: the text one argument of message `parent` shows in the
    case `code` raises (an operator's name). It is set as it is."""
    ENTRIES.append({'name': name, 'what': what, 'exc': exc, 'parent': parent,
                    'hole': hole, 'text': True, 'ints': None, 'requires': None,
                    'shapes': [{'suffix': '', 'args': args, 'code': code}]})


# ---------------------------------------------------------------------------
# The messages. Helpers the probes have: cls(name, *bases, **attrs) makes a
# class, capi(name, restype, *argtypes) a C API function (ctypes), coro_run
# runs a coroutine to its end without an event loop, newmod(name, **attrs)
# puts a module in sys.modules for the probe.

# math
msg('MATH_DOMAIN_ERROR', 'a math function given an argument outside its domain',
    'ValueError', 'import math; math.fmod(1.0, 0.0)', text=True)
# where a function's domain error names its argument
_MATH_NAMES = 'says("import math; math.sqrt(-1.5)").endswith("-1.5")'
for _f, _x in [('sqrt', '-(args[0] + 0.5)'), ('acos', 'args[0] + 1.5'),
               ('asin', 'args[0] + 1.5'), ('cos', '(-1) ** args[0] * math.inf'),
               ('sin', '(-1) ** args[0] * math.inf'),
               ('tan', '(-1) ** args[0] * math.inf')]:
    msg('MATH_DOMAIN_ERROR_' + _f.upper(),
        'math.%s given an argument outside its domain' % _f, 'ValueError',
        'import math\nx = %s\nshown[0] = repr(x)\nmath.%s(x)' % (_x, _f),
        [D()], requires=_MATH_NAMES)
msg('MATH_RANGE_ERROR', "a math function's result too large for a float",
    'OverflowError', 'import math; math.exp(1000.0)', text=True)

# abstract.c
msg('NOT_CALLABLE', "calling an object that isn't callable", 'TypeError',
    'args[0]()()', [T()])
msg('BINOP_TYPE_ERROR', 'a binary operator neither operand supports',
    'TypeError', 'args[1]() - args[2]()', [F('%.100s', '-'), T(), T()])
msg('BINOP_PRINT_TYPE_ERROR', 'print >> x, with a hint', 'TypeError',
    'print >> args[2]()',
    [F('%.100s', '>>'), F('%.100s', 'builtin_function_or_method'), T()])
msg('POW3_TYPE_ERROR', 'pow(a, b, c) none of the operands supports',
    'TypeError', 'pow(args[0](), args[1](), args[2]())', [T(), T(), T()])
msg('SEQ_REPEAT_NON_INT', 'a sequence times a non-integer', 'TypeError',
    '[1] * args[0]()', [T()])
msg('MAPPING_SIZE_NOT_MAPPING', "PyMapping_Size of a sequence that isn't a mapping",
    'TypeError',
    'import collections\n'
    'capi("PyMapping_Size", "ssize", "obj")(collections.deque())',
    [F('%.200s', 'collections.deque')])
msg('MAPPING_NO_LEN', "PyMapping_Size of an object without a length",
    'TypeError', 'capi("PyMapping_Size", "ssize", "obj")(args[0]())', [T()])
msg('NO_LEN', "len() of an object that hasn't one", 'TypeError',
    'len(args[0]())', [T()])
msg('BAD_ABS', "abs() of an object that doesn't support it", 'TypeError',
    'abs(args[0]())', [T()])
msg('BAD_INVERT', "~ of an object that doesn't support it", 'TypeError',
    '~args[0]()', [T()])
msg('BAD_NEGATIVE', "unary - of an object that doesn't support it",
    'TypeError', '-args[0]()', [T()])
msg('BAD_POSITIVE', "unary + of an object that doesn't support it",
    'TypeError', '+args[0]()', [T()])
msg('SEQ_GET_NOT_SEQUENCE', "PySequence_GetItem of a mapping that isn't a sequence",
    'TypeError',
    'capi("PySequence_GetItem", "obj", "obj", "ssize")({}, 0)',
    [F('%.200s', 'dict')])
msg('SEQ_SET_NOT_SEQUENCE', "PySequence_SetItem of a mapping that isn't a sequence",
    'TypeError',
    'capi("PySequence_SetItem", "int", "obj", "ssize", "obj")({}, 0, 1)',
    [F('%.200s', 'dict')])
msg('SEQ_DEL_NOT_SEQUENCE', "PySequence_DelItem of a mapping that isn't a sequence",
    'TypeError',
    'capi("PySequence_DelItem", "int", "obj", "ssize")({}, 0)',
    [F('%.200s', 'dict')])
msg('SEQ_NO_GET', 'PySequence_GetItem of an object without items', 'TypeError',
    'capi("PySequence_GetItem", "obj", "obj", "ssize")(args[0](), 0)', [T()])
msg('SEQ_NO_SET', 'PySequence_SetItem of an object without items', 'TypeError',
    'capi("PySequence_SetItem", "int", "obj", "ssize", "obj")(args[0](), 0, 1)',
    [T()])
msg('SEQ_NO_DEL', 'PySequence_DelItem of an object without items', 'TypeError',
    'capi("PySequence_DelItem", "int", "obj", "ssize")(args[0](), 0)', [T()])
msg('SEQ_GET_SLICE', 'PySequence_GetSlice of an object without slices',
    'TypeError',
    'capi("PySequence_GetSlice", "obj", "obj", "ssize", "ssize")(args[0](), 0, 1)',
    [T()])
msg('SEQ_SET_SLICE', 'PySequence_SetSlice of an object without slices',
    'TypeError',
    'capi("PySequence_SetSlice", "int", "obj", "ssize", "ssize", "obj")('
    'args[0](), 0, 1, [])', [T()])
msg('INDEX_GET_NOT_INT', 'a sequence (no mapping) subscripted by a non-integer',
    'TypeError', 'import collections; collections.deque()[args[0]()]', [T()])
msg('INDEX_SET_NOT_INT', 'a sequence (no mapping) assigned at a non-integer',
    'TypeError', 'import collections; d = collections.deque(); d[args[0]()] = 1',
    [T()])
msg('INDEX_DEL_NOT_INT', 'a sequence (no mapping) deleted at a non-integer',
    'TypeError', 'import collections; d = collections.deque(); del d[args[0]()]',
    [T()])
msg('NOT_SUBSCRIPTABLE', "subscripting an object that can't be", 'TypeError',
    'args[0]()[0]', [T()])
# (a class's instances have the sequence methods, if empty, so these take
# an int, which has none)
msg('NO_ITEM_ASSIGNMENT', "assigning an item of an object with neither "
    "mapping nor sequence methods", 'TypeError', 'o = 1; o[0] = 1',
    [F('%.200s', 'int')])
msg('NO_ITEM_DELETION', "deleting an item of an object with neither "
    "mapping nor sequence methods", 'TypeError', 'o = 1; del o[0]',
    [F('%.200s', 'int')])
msg('NOT_ITERABLE', "iter() of an object that isn't iterable", 'TypeError',
    'iter(args[0]())', [T()])
msg('NEXT_NOT_ITERATOR', "next() of an object that isn't an iterator, as "
    "a for loop's iterator with no __next__ slot", 'TypeError',
    'next(args[0]())', [T()])

# the operators' names, as a binary operator's message shows them
for _name, _code in [
        ('ADD', 'a + b'), ('SUB', 'a - b'), ('MUL', 'a * b'),
        ('MATMUL', 'a @ b'), ('TRUEDIV', 'a / b'), ('FLOORDIV', 'a // b'),
        ('MOD', 'a % b'), ('DIVMOD', 'divmod(a, b)'), ('LSHIFT', 'a << b'),
        ('RSHIFT', 'a >> b'), ('AND', 'a & b'), ('OR', 'a | b'),
        ('XOR', 'a ^ b'),
        ('IADD', 'a += b'), ('ISUB', 'a -= b'), ('IMUL', 'a *= b'),
        ('IMATMUL', 'a @= b'), ('ITRUEDIV', 'a /= b'), ('IFLOORDIV', 'a //= b'),
        ('IMOD', 'a %= b'), ('ILSHIFT', 'a <<= b'), ('IRSHIFT', 'a >>= b'),
        ('IAND', 'a &= b'), ('IOR', 'a |= b'), ('IXOR', 'a ^= b'),
        ('IPOW', 'a **= b')]:
    frag('OP_' + _name, 'the name a type error of %s shows' % _code,
         'BINOP_TYPE_ERROR', 0, 'TypeError',
         'a = args[1](); b = args[2](); ' + _code, [F('%.100s', ''), T(), T()])

# comparisons
msg('COMPARE_NOT_SUPPORTED', 'an order comparison neither operand supports',
    'TypeError', 'args[1]() < args[2]()', [F('%s', '<'), T(), T()])
# (== and != never fail: without __eq__ and __ne__ they compare identity)
for _name, _op in [('LT', '<'), ('LE', '<='), ('GT', '>'), ('GE', '>=')]:
    frag('CMP_' + _name, 'the name a type error of a %s b shows' % _op,
         'COMPARE_NOT_SUPPORTED', 0, 'TypeError',
         'args[1]() %s args[2]()' % _op, [F('%s', ''), T(), T()])

# bytes, float, list, tuple
msg('BYTES_INDEX_RANGE', 'a bytes index out of range', 'IndexError',
    "b'a'[5]", text=True)
# (from 3.15 these name their types by qualified name: shape QUALIFIED)
_FLOAT_NON_FLOAT = ('r = args[1]; c = cls(args[0].__name__, __float__=lambda s: r())\n'
                    'capi("PyFloat_AsDouble", "double", "obj")(c())')
msg('FLOAT_RETURNED_NON_FLOAT', "PyFloat_AsDouble of an object whose __float__ "
    "returns a non-float", 'TypeError', None, shapes=[
        ('', [T(), T()], _FLOAT_NON_FLOAT),
        ('QUALIFIED', [Q(), Q()], _FLOAT_NON_FLOAT)])
_FLOAT_SUBCLASS = ('r = cls(args[1].__name__, float); c = cls(args[0].__name__, '
                   '__float__=lambda s: r(1.5))\n'
                   'capi("PyFloat_AsDouble", "double", "obj")(c())')
msg('FLOAT_RETURNED_FLOAT_SUBCLASS', "PyFloat_AsDouble of an object whose "
    "__float__ returns a float subclass", 'DeprecationWarning', None, shapes=[
        ('', [T(), T()], _FLOAT_SUBCLASS),
        ('QUALIFIED', [Q(), Q()], _FLOAT_SUBCLASS)])
msg('MUST_BE_REAL_NUMBER', 'PyFloat_AsDouble of an object without __float__',
    'TypeError', 'capi("PyFloat_AsDouble", "double", "obj")(args[0]())', [T()])
msg('FLOAT_DIVISION_BY_ZERO', 'a float divided by zero', 'ZeroDivisionError',
    '1.5 / 0.0', text=True)
msg('FLOAT_MODULO_BY_ZERO', 'a float modulo zero', 'ZeroDivisionError',
    '1.5 % 0.0', text=True)
msg('LIST_INDEX_RANGE', 'a list index out of range', 'IndexError', '[][0]',
    text=True)
msg('LIST_ASSIGN_INDEX_RANGE', 'a list assignment index out of range',
    'IndexError', 'l = []; l[0] = 1', text=True)
msg('TUPLE_CONCAT', 'a tuple plus something not a tuple', 'TypeError',
    '() + args[0]()', [T()])
msg('TUPLE_INDEX_RANGE', 'a tuple index out of range', 'IndexError', '()[0]',
    text=True)

# type
msg('INIT_RETURNED', '__init__ returning something other than None',
    'TypeError', 'r = args[0]; cls("C", __init__=lambda s: r())()', [T()])

# str
msg('STR_INDEX_RANGE', 'a str index out of range', 'IndexError', "''[0]",
    text=True)

# sys, builtins
msg('CALL_STACK_NOT_DEEP', 'sys._getframe deeper than the stack', 'ValueError',
    'import sys; sys._getframe(100000)', text=True)
msg('HASATTR_NAME_NOT_STR', 'hasattr() given a name that is not a str',
    'TypeError', 'hasattr(1, 1)', text=True)
msg('GETATTR_NAME_NOT_STR', 'getattr() given a name that is not a str',
    'TypeError', 'getattr(1, 1)', text=True)
msg('SUPER_NO_ARGS', 'super() with no arguments where there are none',
    'RuntimeError', 'exec("super()", {})', text=True)
msg('SUPER_ARG0_DELETED', "super() after the method's first argument is deleted",
    'RuntimeError',
    'exec("class C:\\n def f(self):\\n  del self\\n  super()\\nC().f()", {})',
    text=True)
msg('SUPER_NO_CLASS_CELL', 'super() in a function outside a class',
    'RuntimeError', 'exec("def f(self):\\n super()\\nf(1)", {})', text=True)
msg('SUPER_EMPTY_CLASS_CELL', 'super() before its class is made',
    'RuntimeError',
    'exec("class C:\\n def f(self):\\n  super()\\n f(1)", {})', text=True)
msg('SUPER_CLASS_NOT_TYPE', "super() whose __class__ isn't a type",
    'RuntimeError',
    'exec("def g(v):\\n __class__ = v\\n def f(self):\\n  __class__\\n'
    '  super()\\n return f\\ng(c())(1)", {"c": args[0]})', [T()])
msg('MINMAX_EMPTY', 'min() or max() of an empty sequence', 'ValueError',
    'min([])', [F('%s', 'min')])
frag('MINMAX_NAME_MIN', "the name min()'s error shows", 'MINMAX_EMPTY', 0,
     'ValueError', 'min([])', [F('%s', '')])
frag('MINMAX_NAME_MAX', "the name max()'s error shows", 'MINMAX_EMPTY', 0,
     'ValueError', 'max([])', [F('%s', '')])

# ceval.c
# where Python calls have a count of their own (with sys.monitoring, from
# 3.12), set as it is; before, ceval adds a 'where' to it
msg('RECURSION_DEPTH', 'a Python call deeper than the recursion limit',
    'RecursionError', 'f = lambda: f()\nf()', text=True,
    requires='hasattr(__import__("sys"), "monitoring")')
msg('CATCHING_NON_EXCEPTION', 'except given a class not derived from '
    'BaseException', 'TypeError',
    'try:\n    1 / 0\nexcept 1:\n    pass', text=True)
_BEFORE_WITH = '"BEFORE_WITH" in __import__("opcode").opmap'
msg('WITH_NO_ENTER', 'with given an object without __enter__', 'TypeError',
    'with args[0](): pass', [T()], requires=_BEFORE_WITH)
msg('WITH_NO_EXIT', 'with given an object with __enter__ but no __exit__',
    'TypeError',
    'C = cls(args[0].__name__, __enter__=lambda s: s)\nwith C(): pass',
    [T()], requires=_BEFORE_WITH)
msg('IMPORT_NAME_UNKNOWN_LOCATION', "from m import x, without x, m without a file",
    'ImportError',
    'newmod(args[1])\nexec("from %s import %s" % (args[1], args[0]), {})',
    [O('R'), O('R')])
msg('IMPORT_NAME_FROM_FILE', 'from m import x, without x, m with a file',
    'ImportError',
    'newmod(args[1], __file__=args[2])\n'
    'exec("from %s import %s" % (args[1], args[0]), {})',
    [O('R'), O('R'), O('S')])
msg('IMPORT_NAME_CIRCULAR', 'from m import x, without x, m still being '
    'imported', 'ImportError',
    'import types\n'
    'newmod(args[1], __file__=args[2],'
    ' __spec__=types.SimpleNamespace(_initializing=True))\n'
    'exec("from %s import %s" % (args[1], args[0]), {})',
    [O('R'), O('R'), O('S')])
frag('UNKNOWN_MODULE_NAME', 'what an import error shows for a module without '
     'a name', 'IMPORT_NAME_UNKNOWN_LOCATION', 1, 'ImportError',
     'm = newmod("LeNoName")\ndel m.__name__\n'
     'exec("from LeNoName import %s" % args[0], {})',
     [O('R'), F('%R', '')])
msg('NO_DISPLAYHOOK', 'an interactive statement without sys.displayhook',
    'RuntimeError',
    'import sys\nh = sys.displayhook\ndel sys.displayhook\n'
    'try:\n    exec(compile("1", "<probe>", "single"), {})\n'
    'finally:\n    sys.displayhook = h', text=True)
msg('ASYNC_FOR_NO_AITER', 'async for over an object without __aiter__',
    'TypeError',
    'o = args[0]()\nasync def f():\n    async for x in o:\n        pass\n'
    'coro_run(f())', [T()])
msg('ASYNC_FOR_AITER_NO_ANEXT', 'async for whose __aiter__ returns an object '
    'without __anext__', 'TypeError',
    'r = args[0]; o = cls("A", __aiter__=lambda s: r())()\n'
    'async def f():\n    async for x in o:\n        pass\ncoro_run(f())', [T()])
msg('AWAIT_NOT_AWAITABLE', "await on an object that can't be awaited",
    'TypeError', 'o = args[0]()\nasync def f():\n    await o\ncoro_run(f())',
    [T()])
msg('AWITH_ENTER_NOT_AWAITABLE', 'async with whose __aenter__ returns an '
    "object that can't be awaited", 'TypeError',
    'r = args[0]; o = cls("A", __aenter__=lambda s: r(),\n'
    '                     __aexit__=lambda s, *a: r())()\n'
    'async def f():\n    async with o:\n        pass\ncoro_run(f())', [T()])
msg('AWITH_EXIT_NOT_AWAITABLE', 'async with whose __aexit__ returns an '
    "object that can't be awaited", 'TypeError',
    'async def e(s):\n    pass\n'
    'r = args[0]; o = cls("A", __aenter__=e, __aexit__=lambda s, *a: r())()\n'
    'async def f():\n    async with o:\n        pass\ncoro_run(f())', [T()])
# (from 3.15 these name the awaited object's type: shape QUALIFIED)
_AWAIT_COROUTINE = ('async def c():\n    pass\nk = c()\nk.close()\n'
                    'o = cls(%s, __await__=lambda s: k)()\n'
                    'async def f():\n    await o\ncoro_run(f())')
msg('AWAIT_RETURNED_COROUTINE', '__await__ returning a coroutine', 'TypeError',
    None, text=True, shapes=[
        ('', [], _AWAIT_COROUTINE % '"A"'),
        ('QUALIFIED', [Q()], _AWAIT_COROUTINE % 'args[0].__name__')])
msg('AWAIT_NON_ITERATOR', '__await__ returning something not an iterator',
    'TypeError', None, shapes=[
        ('', [T()], 'r = args[0]; o = cls("A", __await__=lambda s: r())()\n'
         'async def f():\n    await o\ncoro_run(f())'),
        ('QUALIFIED', [Q(), Q()],
         'r = args[1]; o = cls(args[0].__name__, __await__=lambda s: r())()\n'
         'async def f():\n    await o\ncoro_run(f())')])
msg('CORO_AWAITED_ALREADY', 'await on a coroutine that is awaiting',
    'RuntimeError',
    'import types\n@types.coroutine\ndef y():\n    yield\n'
    'async def i():\n    await y()\nk = i()\nk.send(None)\n'
    'async def f():\n    await k\ncoro_run(f())', text=True)
msg('NO_BUILD_CLASS', 'a class statement without __build_class__', 'NameError',
    'exec("class C: pass", {"__builtins__": {}})', text=True)
msg('NAME_NOT_DEFINED', 'a global name not found', 'NameError',
    'exec(args[0], {})', [S()])
msg('LOCAL_UNBOUND', 'a local read before it is set', 'UnboundLocalError',
    'exec("def f():\\n print(%s)\\n %s = 1\\nf()" % (args[0], args[0]), {})',
    [S()])
msg('FREE_UNBOUND', "a free variable read before it is set", 'NameError',
    'exec("def f():\\n def g():\\n  return %s\\n g()\\n %s = 1\\nf()"'
    ' % (args[0], args[0]), {})', [S()])
msg('LIST_STAR_NOT_ITERABLE', '[*x] of an x that is not iterable', 'TypeError',
    '[*args[0]()]', [T()])
msg('DICT_STAR_NOT_MAPPING', '{**x} of an x that is not a mapping', 'TypeError',
    None, shapes=[('', [T()], '{**args[0]()}'),
                  ('QUALIFIED', [Q()], '{**args[0]()}')])

# a call's * and ** arguments: the function is shown by its name and
# description until 3.8, by its qualified name from 3.9
_FN = ('def f(*a, **k): pass\n'
       'f.__name__ = args[0]; f.__qualname__ = "LeQualname"; '
       'f.__module__ = "builtins"\n')
_FN_STR = ('def f(*a, **k): pass\n'
           'f.__name__ = "LeName"; f.__qualname__ = args[0]; '
           'f.__module__ = "builtins"\n')
# and from 3.15 not at all
_NO_FN = ('def f(*a, **k): pass\n'
          'f.__name__ = f.__qualname__ = "LeShown"\n'
          'try:\n    %s\nexcept TypeError as x:\n'
          '    if "LeShown" in str(x):\n'
          '        raise ValueError("the function is shown: %%s" %% x)\n'
          '    raise\n')
msg('STAR_NOT_ITERABLE', 'f(*x) of an x that is not iterable', 'TypeError',
    None, shapes=[
        ('FUNC_NAME', [S(), F('%.200s', '()'), T()], _FN + 'f(*args[2]())'),
        ('FUNCTION_STR', [O('U'), T()],
         _FN_STR + 'shown[0] = args[0] + "()"; f(*args[1]())'),
        ('NO_FUNCTION', [T()], _NO_FN % 'f(*args[0]())')])
msg('KWARGS_NOT_MAPPING', 'f(**x) of an x that is not a mapping', 'TypeError',
    None, shapes=[
        ('FUNC_NAME', [S(), F('%.200s', '()'), T()], _FN + 'f(**args[2]())'),
        ('FUNCTION_STR', [O('U'), T()],
         _FN_STR + 'shown[0] = args[0] + "()"; f(**args[1]())'),
        ('NO_FUNCTION', [Q()], _NO_FN % 'f(**args[0]())')])
# (from 3.9 a key that is not a str, given twice, is shown as a duplicate)
# (from 3.15 a function whose name can't be made, its __qualname__ raising,
# is shown as "function" (%V), not the error: shape OR_FUNCTION)
_NAMELESS = ('class Nameless:\n'
             '    def __call__(self, *a, **k): pass\n'
             '    def __getattribute__(self, n):\n'
             '        if n == "__qualname__": raise RuntimeError("no name")\n'
             '        return object.__getattribute__(self, n)\n'
             'try:\n    Nameless()(**{1: 1}, **{1: 2})\n'
             'except TypeError as x:\n    shown_as_function = str(x).startswith("function ")\n'
             'except RuntimeError:\n    shown_as_function = False\n')
_UNNAMED_ERROR = _NAMELESS + ('if shown_as_function:\n'
                              '    raise ValueError("a nameless function is shown as function")\n')
_UNNAMED_FUNCTION = _NAMELESS + ('if not shown_as_function:\n'
                                 '    raise ValueError("a nameless function\'s error is raised")\n')
msg('KWARGS_NOT_STRINGS', 'f(**x, **y) with a key that is not a str in both',
    'TypeError', None, shapes=[
        ('FUNC_NAME', [S(), F('%.200s', '()')],
         _FN + 'f(**{1: 1}, **{1: 2})'),
        ('FUNCTION_STR', [O('U'), F('%S', '1')], _UNNAMED_ERROR +
         _FN_STR + 'shown[0] = args[0] + "()"; f(**{1: 1}, **{1: 2})'),
        ('OR_FUNCTION', [O('V'), F('%S', '1')], _UNNAMED_FUNCTION +
         _FN_STR + 'shown[0] = args[0] + "()"; f(**{1: 1}, **{1: 2})')])
msg('KWARGS_DUPLICATE', 'f(x=1, **{"x": 2})', 'TypeError', None, shapes=[
    ('FUNC_NAME', [S(), F('%.200s', '()'), O('U')],
     _FN + 'f(**{args[2]: 1}, **{args[2]: 2})'),
    ('FUNCTION_STR', [O('U'), O('S')], _UNNAMED_ERROR +
     _FN_STR + 'shown[0] = args[0] + "()"; f(**{args[1]: 1}, **{args[1]: 2})'),
    ('OR_FUNCTION', [O('V'), O('S')], _UNNAMED_FUNCTION +
     _FN_STR + 'shown[0] = args[0] + "()"; f(**{args[1]: 1}, **{args[1]: 2})')])

msg('UNPACK_NON_ITERABLE', 'unpacking an object that is not iterable',
    'TypeError', 'a, b = args[0]()', [T()])
_TARGETS = '", ".join("v%d" % i for i in range({0}))'
msg('UNPACK_NOT_ENOUGH', 'a, b = fewer values', 'ValueError',
    'exec(%s + " = range(args[1])", {"args": args})' % _TARGETS.format('args[0]'),
    [I('d'), I('d')], ints=[[7, 3], [9, 4]])
msg('UNPACK_NOT_ENOUGH_STAR', 'a, b, *c = fewer values than before the star',
    'ValueError',
    'exec(%s + ", *s = range(args[1])", {"args": args})'
    % _TARGETS.format('args[0]'), [I('d'), I('d')], ints=[[7, 3], [9, 4]])
msg('UNPACK_TOO_MANY', 'a, b = more values', 'ValueError',
    'exec(%s + ", = range(args[0] + 3)", {"args": args})'
    % _TARGETS.format('args[0]'), [I('d')], ints=[[7], [9]])
msg('UNPACK_NOT_ENOUGH_AFTER_STAR', 'a, *b, c, d = fewer values than after '
    'the star', 'ValueError',
    'exec("v0, *s, " + ", ".join("w%d" % i for i in range(args[0] - 1))'
    ' + " = range(args[1])", {"args": args})',
    [I('d'), I('zd')], ints=[[7, 3], [9, 4]])
msg('NO_ACTIVE_EXCEPTION', 'raise with no exception being handled',
    'RuntimeError', 'raise', text=True)
msg('RAISE_NOT_INSTANCE', "raise of a class whose call doesn't make an "
    "exception", 'TypeError',
    'R = cls(args[1]); r = R()\n'
    'E = cls(args[0], BaseException, __new__=lambda c: r)\n'
    'shown[0] = repr(E); shown[1] = repr(R)\nraise E', [O('R'), O('R')])
msg('RAISE_NOT_EXCEPTION', 'raise of an object that is not an exception',
    'TypeError', 'raise 1', text=True)
msg('RAISE_CAUSE_NOT_EXCEPTION', 'raise ... from an object that is not an '
    'exception', 'TypeError', 'raise ValueError from 1', text=True)
msg('NO_IMPORT', 'an import statement without __import__', 'ImportError',
    'exec("import sys", {"__builtins__": {}})', text=True)
msg('GENERATOR_STOP_ITERATION', 'a generator raising StopIteration',
    'RuntimeError',
    'def g():\n    raise StopIteration\n    yield\nnext(g())', text=True)
msg('CORO_STOP_ITERATION', 'a coroutine raising StopIteration',
    'RuntimeError',
    'async def f():\n    raise StopIteration\nf().send(None)', text=True)
msg('NEGATIVE_SHIFT', 'a shift by a negative count', 'ValueError', '1 << -1',
    text=True)

# 3.10's match statement
_MATCH = 'compiles("match 1:\\n case 1: pass")'
msg('MATCH_DUPLICATE_ATTRIBUTE', 'a class pattern naming an attribute twice',
    'TypeError',
    'C = cls(args[0].__name__, __match_args__=(args[1],), **{args[1]: 1})\n'
    'exec("match C():\\n case C(1, %s=2): pass" % args[1], {"C": C})',
    [T(), O('R')], requires=_MATCH)
msg('MATCH_NOT_A_TYPE', 'a class pattern of something that is not a type',
    'TypeError', 'exec("match 1:\\n case C(): pass", {"C": 1})', text=True,
    requires=_MATCH)
msg('MATCH_ARGS_NOT_TUPLE', "a class pattern whose __match_args__ isn't a "
    "tuple", 'TypeError',
    'C = cls(args[0].__name__, __match_args__=args[1]())\n'
    'exec("match C():\\n case C(1): pass", {"C": C})', [T(), T()],
    requires=_MATCH)
msg('MATCH_TOO_MANY_POSITIONAL', 'a class pattern with more positional '
    'patterns than __match_args__', 'TypeError',
    'C = cls(args[0].__name__, __match_args__=tuple("a%d" % i for i in '
    'range(args[1])))\nshown[2] = "" if args[1] == 1 else "s"\n'
    'exec("match C():\\n case C(%s): pass" % ", ".join(["1"] * args[3]),'
    ' {"C": C})',
    [T(), I('zd'), P(), I('zd')], ints=[[0, 1, 0, 3], [0, 2, 0, 5]],
    requires=_MATCH)
msg('MATCH_ARGS_NOT_STR', "a class pattern whose __match_args__ holds a "
    "non-str", 'TypeError',
    'C = cls("C", __match_args__=(args[0](),))\n'
    'exec("match C():\\n case C(1): pass", {"C": C})', [T()],
    requires=_MATCH)
msg('MATCH_DUPLICATE_KEY', 'a mapping pattern whose keys are equal',
    'ValueError',
    'K = cls("K", a=args[0], b=args[0])\n'
    'exec("match d:\\n case {K.a: 1, K.b: 2}: pass",'
    ' {"K": K, "d": {args[0]: 1, 0: 2}})',
    [O('R')],
    requires=_MATCH)


# ---------------------------------------------------------------------------
# What runs in the target: each probe three times, the messages as JSON.

# the module of the probes' classes (T(), Q(), cls): a message that shows
# it names a type by its qualified name, which Q() recovers
MODULE = 'lemod'

DRIVER = r'''
import json, sys, io, warnings
entries = json.loads(sys.stdin.read())
MARK = [lambda i: "Zq%dj" % i, lambda i: "Wv%dk" % i,
        lambda i: "L%d" % i + "abcdefghij" * 120]
DEFAULT_INTS = [[11, 23, 37, 53], [13, 29, 41, 61]]
MODS = []

def cls(name, *bases, **attrs):
    attrs.setdefault("__module__", "lemod")
    return type(name, bases or (object,), attrs)

def capi(name, restype, *argtypes):
    import ctypes
    t = {"obj": ctypes.py_object, "ssize": ctypes.c_ssize_t,
         "int": ctypes.c_int, "double": ctypes.c_double}
    f = getattr(ctypes.pythonapi, name)
    f.restype = t[restype]
    f.argtypes = [t[a] for a in argtypes]
    return f

def coro_run(c):
    try:
        while True:
            c.send(None)
    except StopIteration:
        pass

def newmod(name, **attrs):
    import types
    m = types.ModuleType(name)
    for k, v in attrs.items():
        setattr(m, k, v)
    sys.modules[name] = m
    MODS.append(name)
    return m

def says(src):
    try:
        exec(src, {})
    except Exception as x:
        return str(x)
    return ""

def compiles(src):
    try:
        compile(src, "<probe>", "exec")
        return True
    except SyntaxError:
        return False

def make_args(spec, run, ints):
    args, shown = [], []
    ints = ints[0 if run == 2 else run] if ints else DEFAULT_INTS[0 if run == 2 else run]
    for i, a in enumerate(spec):
        k = a["k"]
        if k == "T":
            name = MARK[run](i)
            args.append(type(name, (object,), {"__module__": "lemod"}))
            shown.append(name)
        elif k == "Q":
            name = MARK[run](i)
            args.append(type(name, (object,), {"__module__": "lemod"}))
            shown.append("lemod." + name)
        elif k == "S" or (k == "O" and a["conv"] in "USV"):
            args.append(MARK[run](i))
            shown.append(MARK[run](i))
        elif k == "O":
            args.append(MARK[run](i))
            shown.append(repr(MARK[run](i)))
        elif k == "I":
            v = ints[i]
            args.append(v)
            shown.append({"c": chr(v), "x": "%x" % v}.get(a["conv"], str(v)))
        elif k == "F":
            args.append(a["text"])
            shown.append(a["text"])
        else:
            args.append(ints[i])
            shown.append(None)
    return args, shown

def run_probe(e, shape, run):
    args, shown = make_args(shape["args"], run, e["ints"])
    ns = {"args": args, "shown": shown, "cls": cls, "capi": capi,
          "coro_run": coro_run, "newmod": newmod, "__name__": "niprobe"}
    out = sys.stdout
    sys.stdout = io.StringIO()
    got = None
    try:
        with warnings.catch_warnings(record=True) as w:
            warnings.simplefilter("always")
            try:
                exec(shape["code"], ns)
            except BaseException as x:
                got = x
            seen = [m for m in w if m.category.__name__ == e["exc"]]
    finally:
        sys.stdout = out
        for m in MODS:
            sys.modules.pop(m, None)
        del MODS[:]
    if e["exc"].endswith("Warning"):
        if not seen:
            return {"error": "no %s (got %r)" % (e["exc"], got)}
        return {"msg": str(seen[0].message), "shown": shown}
    if got is None:
        return {"error": "no exception"}
    if type(got).__name__ != e["exc"]:
        return {"error": "%s: %s, not %s" % (type(got).__name__, got, e["exc"])}
    m = got.args[0] if got.args else ""
    if not isinstance(m, str):
        return {"error": "the exception's argument is %r" % (m,)}
    return {"msg": m, "shown": shown}

result = {}
for e in entries:
    if e["requires"] and not eval(e["requires"], {"compiles": compiles,
                                               "says": says}):
        result[e["name"]] = None
        continue
    result[e["name"]] = [[run_probe(e, s, run) for run in range(3)]
                         for s in e["shapes"]]
sys.stdout.write("\n--le-strings--\n" + json.dumps(result))
'''


def run_target(python, entries=None):
    """Run every probe in `python`; the raw results by name."""
    entries = ENTRIES if entries is None else entries
    try:
        r = subprocess.run([python, '-I', '-c', DRIVER],
                           input=json.dumps(entries), stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, universal_newlines=True,
                           timeout=TIMEOUT)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise StringError('cannot run the probes in %s: %s' % (python, e))
    out = r.stdout.rsplit('\n--le-strings--\n', 1)
    if r.returncode != 0 or len(out) != 2:
        raise StringError('the probes failed in %s:\n%s' % (python, r.stderr))
    return json.loads(out[1])


# ---------------------------------------------------------------------------
# Recovering a format from the runs.

def _placements(text, values, start=0):
    """Every way to find values, in order, in text: lists of positions."""
    if not values:
        yield []
        return
    v = values[0]
    pos = text.find(v, start)
    while pos != -1:
        for rest in _placements(text, values[1:], pos + len(v)):
            yield [pos] + rest
        pos = text.find(v, pos + 1)


def _pieces(text, values, positions):
    out, at = [], 0
    for v, p in zip(values, positions):
        out.append(text[at:p])
        at = p + len(v)
    out.append(text[at:])
    return out


def _join(pieces, values):
    out = pieces[0]
    for v, p in zip(values, pieces[1:]):
        out += v + p
    return out


def split(runs, nargs):
    """The literal pieces between the arguments, the same in both runs."""
    (m0, s0), (m1, s1) = [(r['msg'], r['shown']) for r in runs[:2]]
    if None in s0 or None in s1:
        raise StringError('the probe left an argument unshown')
    found = set()
    for pos in _placements(m0, s0):
        pieces = _pieces(m0, s0, pos)
        if _join(pieces, s1) == m1:
            found.add(tuple(pieces))
    if len(found) != 1:
        raise StringError('%s the arguments in %r and %r'
                          % ('cannot find' if not found else 'more than one'
                             ' way to place', m0, m1))
    return list(found.pop())


def _is_name(a):
    return a['k'] in 'TSQ' or a['k'] == 'O' and a['conv'] != 'R'


def long_shown(pieces, args, long_run):
    """What each argument shows in the run with long names, cut or not,
    found between the pieces."""
    pat = ''
    for j, p in enumerate(pieces):
        pat += re.escape(p)
        if j < len(args):
            pat += ('(.*?)' if _is_name(args[j])
                    else '(' + re.escape(long_run['shown'][j]) + ')')
    m = re.match(pat + r'\Z', long_run['msg'], re.S)
    if not m:
        raise StringError('the run with long names does not fit: %r'
                          % long_run['msg'][:300])
    return m.groups()


def precision(got, full):
    """The precision a name `full` shown as `got` was cut to; None for all
    of it."""
    if got == full:
        return None
    if got and full.startswith(got):
        return len(got)
    raise StringError('a long name is shown as %r' % got[:40])


def recover(entry, shape, runs):
    """The C text of one shape of an entry, or raise StringError."""
    for r in runs:
        if 'error' in r:
            raise StringError(r['error'])
    args = shape['args']
    if entry['text'] and not args:
        if MODULE + '.' in runs[0]['msg']:
            raise StringError('a type is shown with its module: %r' % runs[0]['msg'])
        if runs[0]['msg'] != runs[1]['msg']:
            raise StringError('the message changes from run to run: %r, %r'
                              % (runs[0]['msg'], runs[1]['msg']))
        return runs[0]['msg']
    pieces = split(runs, len(args))
    if any(MODULE + '.' in p for p in pieces):
        raise StringError('a type is shown with its module: %r' % runs[0]['msg'])
    longs = long_shown(pieces, args, runs[2])
    specs = []
    for i, a in enumerate(args):
        k = a['k']
        if k in 'TS':
            n = precision(longs[i], runs[2]['shown'][i])
            specs.append('%s' if n is None else '%%.%ds' % n)
        elif k == 'O':
            if _is_name(a) and precision(longs[i], runs[2]['shown'][i]):
                raise StringError('an object is shown cut short')
            specs.append('%' + a['conv'])
        elif k == 'Q':
            if precision(longs[i], runs[2]['shown'][i]):
                raise StringError('a type is shown cut short')
            specs.append('%N')
        elif k == 'I':
            specs.append('%' + a['conv'])
        elif k == 'F':
            specs.append(a['spec'])
        else:
            specs.append('%s')
    return _join([p.replace('%', '%%') for p in pieces], specs)


def _format_pieces(fmt):
    """A format's literal pieces (unescaped) and its conversions."""
    parts = re.split(r'(%%|%[-.0-9]*(?:z|l)?[a-zA-Z])', fmt)
    pieces, convs, cur = [], [], ''
    for i, p in enumerate(parts):
        if i % 2 == 0:
            cur += p
        elif p == '%%':
            cur += '%'
        else:
            pieces.append(cur)
            convs.append(p)
            cur = ''
    pieces.append(cur)
    return pieces, convs


def fragment(entry, parent_fmt, runs):
    """The text argument `hole` of the parent message shows."""
    for r in runs:
        if 'error' in r:
            raise StringError(r['error'])
    pieces, convs = _format_pieces(parent_fmt)
    hole = entry['hole']
    got = set()
    for r in runs[:2]:
        pat = ''
        for j, p in enumerate(pieces):
            pat += re.escape(p)
            if j < len(convs):
                pat += '(.+?)' if j == hole else re.escape(r['shown'][j])
        m = re.match(pat + r'\Z', r['msg'], re.S)
        if not m:
            raise StringError('%r is not a %s message' % (r['msg'], entry['parent']))
        text = m.group(1)
        if convs[hole] == '%R':
            text = ast.literal_eval(text)
        got.add(text)
    if len(got) != 1:
        raise StringError('the fragment changes from run to run: %r' % got)
    return got.pop()


def probe(python):
    """{NAME: text or None (the interpreter has no such case)}: every
    message the interpreter gives, a shape's name with its suffix."""
    raw = run_target(python)
    out, errors = {}, []
    for e in ENTRIES:
        res = raw[e['name']]
        if res is None:
            out[e['name']] = None
            continue
        if 'parent' in e:
            continue
        found, why = [], []
        for shape, runs in zip(e['shapes'], res):
            try:
                found.append((shape['suffix'], recover(e, shape, runs)))
            except StringError as x:
                why.append('%s: %s' % (shape['suffix'] or 'probe', x))
        if len(found) != 1:
            errors.append('%s (%s): %s' % (e['name'], e['what'],
                          '; '.join(why) if not found else
                          'more than one shape fits'))
            continue
        suffix, text = found[0]
        out[e['name'] + ('_' + suffix if suffix else '')] = text
    for e in ENTRIES:
        if 'parent' not in e:
            continue
        try:
            parent = out.get(e['parent'])
            if parent is None:
                raise StringError('no %s' % e['parent'])
            out[e['name']] = fragment(e, parent, raw[e['name']][0])
        except StringError as x:
            errors.append('%s (%s): %s' % (e['name'], e['what'], x))
    if errors:
        raise StringError('cannot recover from %s:\n  %s'
                          % (python, '\n  '.join(errors)))
    return out


def c_string(text):
    out = []
    for b in text.encode('utf-8'):
        c = chr(b)
        if c in '"\\':
            out.append('\\' + c)
        elif 32 <= b < 127 and c != '?':
            out.append(c)
        else:
            out.append('\\%03o' % b)   # never runs into a following digit
    return '"%s"' % ''.join(out)


def entry_of(name):
    """The entry a header name (with its shape's suffix) comes from."""
    for e in ENTRIES:
        if name == e['name'] or name.startswith(e['name'] + '_') and \
                name[len(e['name']) + 1:] in [s['suffix'] for s in e['shapes']]:
            return e
    raise KeyError(name)


def build(python, version):
    """The header's text for the interpreter `python`."""
    found = probe(python)
    lines = ['/* Generated by tools/features/strings.py from CPython %s: do not'
             ' edit. */' % version,
             '#ifndef LE_STRINGS_GEN_H', '#define LE_STRINGS_GEN_H', '']
    for name in sorted(found):
        what = entry_of(name)['what'].replace('*/', '* /')
        if found[name] is None:
            lines.append('/* %s: this interpreter has no such case */' % what)
            continue
        lines += ['/* %s */' % what,
                  '#define LE_MSG_%s %s' % (name, c_string(found[name]))]
    lines += ['', '/* for lejit._core._build: every message */',
              '#define LE_MESSAGES(M) \\']
    lines += ['  M(LE_MSG_%s) \\' % name for name in sorted(found)
              if found[name] is not None]
    lines += ['', '#endif /* LE_STRINGS_GEN_H */', '']
    return '\n'.join(lines)


def target_version(python):
    try:
        r = subprocess.run([python, '-I', '-c',
                            'import sys; print(sys.version.split()[0])'],
                           stdout=subprocess.PIPE, universal_newlines=True,
                           timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise StringError('cannot run %s: %s' % (python, e))
    if r.returncode != 0:
        raise StringError('cannot run %s' % python)
    return r.stdout.strip()


def build_header(python, out_dir):
    """Write out_dir/le_strings_gen.h for the interpreter `python`; the
    path. Rewritten only when its text changes, so an unchanged header
    doesn't rebuild lejit._core. Raises StringError."""
    path = os.path.join(out_dir, HEADER)
    try:
        text = build(python, target_version(python))
    except StringError:
        if os.path.exists(path):
            os.remove(path)
        raise
    os.makedirs(out_dir, exist_ok=True)
    if os.path.exists(path):
        with open(path) as f:
            if f.read() == text:
                return path
    with open(path + '.tmp', 'w') as f:
        f.write(text)
    os.replace(path + '.tmp', path)
    print('%s: %d messages' % (path, text.count('\n#define LE_MSG_')))
    return path


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--python', required=True)
    ap.add_argument('--out')
    ap.add_argument('--list', action='store_true')
    args = ap.parse_args(argv)
    try:
        if args.list or not args.out:
            for name, text in sorted(probe(args.python).items()):
                print('%-36s %s' % (name, 'absent' if text is None
                                    else c_string(text)))
        else:
            build_header(args.python, args.out)
    except StringError as e:
        print('strings.py: %s' % e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
