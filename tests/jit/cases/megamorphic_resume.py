# A frame that goes on in ceval (a type-promotion site past
# MEGAMORPHIC_MAX types, or tracing started by a call) must resume exactly
# where CPython would be. An inlined callee has no blocks of its own: the
# ones the compiler holds while it runs are its caller's, which goes on
# compiled after the call (they were once given to the callee's frame
# and then dropped). Fresh classes on every call, so each
# site goes past the limit.
import sys


class Base:
    def __call__(self):
        return 1

    def meth(self):
        return 2

    def __bool__(self):
        return True


def fresh(k, **ns):
    return type('K%d' % k, (Base,), ns)()


def _call(f):
    return f()                         # inlined: the call site promotes f's type


def _method(o):
    return o.meth()


def _truth(o):
    return not o.inner                 # the attribute's value is truth-tested


def _raises(k):
    def __call__(self):
        raise ValueError(k)
    return __call__


def call_in_except(n):
    out = []
    for k in range(n):
        try:
            out.append(_call(fresh(k)))
            if k % 3 == 0:
                raise KeyError(k)
        except KeyError as e:
            out.append(('caught', e.args))
    return out


def call_raises_in_except(n):
    # the callee raises in ceval: its caller's handler catches it
    out = []
    for k in range(n):
        try:
            out.append(_call(fresh(k, __call__=_raises(k))))
        except ValueError as e:
            out.append(('caught', e.args))
    return out


def call_in_finally(n):
    out = []
    for k in range(n):
        try:
            try:
                out.append(_call(fresh(k)))
                if k % 4 == 0:
                    raise ValueError(k)
            finally:
                out.append('f')
        except ValueError:
            out.append('v')
    return out


class Takes:
    def __call__(self, *args, **kwargs):
        return len(args), kwargs

    def meth(self, *args, **kwargs):
        return len(args), kwargs


def _call_kw(f, a, b):
    return f(a, b, addr=a + b)         # the call site promotes f's type


def _method_kw(o, a):
    return o.meth(a, key=a, other=0)


def call_with_keywords(n):
    # past the limit ceval runs the call, with its keyword names
    out = []
    for k in range(n):
        out.append(_call_kw(type('T%d' % k, (Takes,), {})(), k, 1))
        out.append(_method_kw(type('T%d' % k, (Takes,), {})(), k))
    return out


class Ctx:
    def __init__(self, out):
        self.out = out

    def __enter__(self):
        self.out.append('enter')
        return self

    def __exit__(self, *exc):
        self.out.append(('exit', exc[0] is not None))
        return exc[0] is KeyError


def call_in_with(n):
    out = []
    for k in range(n):
        with Ctx(out):
            out.append(_call(fresh(k)))
            if k % 3 == 0:
                raise KeyError(k)
    return out


def call_in_loop(n):
    out = []
    for k in range(n):
        for j in range(3):
            if j == 1:
                continue
            out.append(_call(fresh(k)) + j)
            if k % 5 == 4:
                break
        else:
            out.append('else')
    return out


class Holder:
    def __init__(self, inner):
        self.inner = inner


def attr_in_except(n):
    out = []
    for k in range(n):
        o = Holder(fresh(k))
        try:
            out.append(_method(fresh(k)))
            out.append(_truth(o))
            if k % 2:
                raise KeyError(k)
        except KeyError:
            out.append('k')
    return out


def _loops(f):
    # not inlinable: the frame is compiled on its own, with its own blocks
    r = 0
    for i in range(2):
        try:
            r += f()
            if i:
                raise ValueError(i)
        except ValueError:
            r += 10
    return r


def own_frame_in_except(n):
    out = []
    for k in range(n):
        try:
            out.append(_loops(fresh(k)))
            raise KeyError
        except KeyError:
            out.append('k')
    return out


def traceback_lines(n):
    out = []
    for k in range(n):
        try:
            _call(fresh(k, __call__=_raises(k)))
        except ValueError as e:
            tb, lines = e.__traceback__, []
            while tb is not None:
                lines.append((tb.tb_frame.f_code.co_name,
                              tb.tb_lineno - tb.tb_frame.f_code.co_firstlineno))
                tb = tb.tb_next
            out.append(lines)
    return out


events = []


def _trace(frame, event, arg):
    if frame.f_code.co_name in ('traced_in_except', '_probe'):
        events.append((event, frame.f_code.co_name,
                       frame.f_lineno - frame.f_code.co_firstlineno))
    return _trace


def _start_tracing(settrace, trace, x):
    settrace(trace)                    # inlined: tracing starts in a callee
    return x


def _probe(x):
    return x + 1


def traced_in_except():
    del events[:]
    out = []
    try:
        try:
            out.append(_start_tracing(sys.settrace, _trace, 1))
            raise KeyError(2)
        except KeyError as e:
            out.append(_probe(e.args[0]))
        out.append(3)
    finally:
        sys.settrace(None)
    return out, events[:]


CASES = [
    ('call_in_except', [(12,)], {}),
    ('call_raises_in_except', [(12,)], {}),
    ('call_in_finally', [(12,)], {}),
    ('call_in_with', [(12,)], {}),
    ('call_with_keywords', [(12,)], {}),
    ('call_in_loop', [(12,)], {}),
    ('attr_in_except', [(12,)], {}),
    ('own_frame_in_except', [(12,)], {}),
    ('traceback_lines', [(12,)], {}),
    ('traced_in_except', [()], {}),
]
