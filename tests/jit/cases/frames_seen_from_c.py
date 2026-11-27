# What logging, warnings and line-number users see of compiled frames, from
# an adversarial test's frames_logging: the f_back of a
# frame a keyword call runs, warnings' stacklevel, which _warnings walks in
# C from tstate->frame, and emulated frames' current lines.
import gc
import inspect
import io
import logging
import operator
import sys
import traceback
import warnings


def chain_names(f, n):
    out = []
    while f is not None and len(out) < n:
        out.append(f.f_code.co_name)
        f = f.f_back
    return out


def kw_callee(x):
    return chain_names(sys._getframe(0), 3)


def kw_middle():
    return kw_callee(x=1)


def kw_call_chain():
    return kw_middle()


def pos_middle():
    return kw_callee(1)


def pos_call_chain():
    return pos_middle()


def deprecated():
    warnings.warn('old', DeprecationWarning, stacklevel=2)


def warn_caller():
    deprecated()


def warn_stacklevel():
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter('always')
        warn_caller()
    return [w.lineno - warn_caller.__code__.co_firstlineno for w in caught]


stream = io.StringIO()
log = logging.getLogger('frames_seen_from_c')
log.propagate = False
_handler = logging.StreamHandler(stream)
_handler.setFormatter(logging.Formatter('%(funcName)s %(lineno)d'))
log.addHandler(_handler)
log.setLevel(logging.INFO)


def log_here():
    x = 1
    log.info('here %d', x)


def log_caller():
    stream.seek(0)
    stream.truncate()
    log_here()
    name, line = stream.getvalue().split()
    return name, int(line) - log_here.__code__.co_firstlineno


def lineno_callee():
    return sys._getframe(1).f_lineno


def lineno_middle():
    x = 1
    return lineno_callee() - lineno_middle.__code__.co_firstlineno + x


def lineno_chain():
    return lineno_middle()


def lasti_seen(_):
    f = sys._getframe(1)
    return f.f_lasti, f.f_lineno - f.f_code.co_firstlineno


# the place of a frame calling Python through C, as f_lasti and as inspect
# finds it
def lasti_through_c():
    x = 1
    seen = list(map(lasti_seen, [x]))
    stack = list(map(lambda _: inspect.stack()[1], [x]))
    return seen, stack[0].lineno - lasti_through_c.__code__.co_firstlineno


class Where:
    @property
    def line(self):
        return sys._getframe(1).f_lineno


# the line of a frame in an attribute's Python method, not a call
def attr_middle(w):
    x = 1
    y = w.line - attr_middle.__code__.co_firstlineno
    return y + x


def attr_line():
    return attr_middle(Where())


def caller_seen():
    f = sys._getframe(1)
    return f.f_code.co_name, f.f_lineno - f.f_code.co_firstlineno


def seen_kw(k):
    return caller_seen()


class Seen:
    def __init__(self):
        self.seen = caller_seen()


def seen_gen():
    yield caller_seen()


# what sys._getframe(1) gives a callee of a function reached by a keyword
# call, of an __init__ and of a generator
def getframe_through():
    return seen_kw(k=1), Seen().seen, next(seen_gen())


# a frame sys._getframe gives, its callers read through f_back while the
# call runs, after the callers' locals change, and after it returns
def _shown_ints(d):
    return sorted((k, v) for k, v in d.items() if isinstance(v, int))


def _deep_leaf(n):
    f = sys._getframe(n)
    return f, chain_names(f, 3)


def _deep_mid(n):
    m = n * 3
    f, during = _deep_leaf(n)
    m += 1
    return f, during, [_shown_ints(g.f_locals) for g in (f, f.f_back)
                       if g.f_code.co_name == '_deep_mid'], m


def _deep_top(n):
    t = n
    f, during, mid, m = _deep_mid(n)
    t += 1
    return during, mid, chain_names(f, 3), m, t


# the callers, through f_back, of a frame returned through compiled
# callers that never asked for theirs: all of them are there
def _returns_frame1():
    return sys._getframe()


def _returns_frame2():
    return _returns_frame1()


def _returns_frame3():
    return _returns_frame2()


def _returns_frame4():
    return _returns_frame3()


def getframe_returned():
    return chain_names(_returns_frame4(), 6)


def getframe_deep():
    return [_deep_top(n) for n in range(2)]


# pyparsing's _trim_arity: call a parse action with fewer arguments until
# one fits, telling a wrong arity from a TypeError inside it by the line
# of the traceback's last frame
def trim_arity(func, max_limit=3):
    limit = [0]

    def wrapper(*args):
        while 1:
            try:
                return func(*args[limit[0]:])
            except TypeError as te:
                if limit[0] < max_limit:
                    tb = te.__traceback__
                    frames = traceback.extract_tb(tb, limit=2)
                    if frames[-1][:2] == called_at:
                        limit[0] += 1
                        continue
                raise
    here = traceback.extract_stack(limit=1)[-1]
    called_at = (here[0], here[1] - (trim_arity_line - wrapper_call_line))
    return wrapper


trim_arity_line = trim_arity.__code__.co_firstlineno + 15
wrapper_call_line = trim_arity.__code__.co_firstlineno + 6


def one_arg_action(toks):
    return [t.upper() for t in toks]


def raising_action(toks):
    raise TypeError('inside')


def trim_arity_case():
    out = [trim_arity(one_arg_action)('s', 0, ['a', 'b'])]
    try:
        trim_arity(raising_action)('s', 0, ['a'])
    except TypeError as e:
        out.append(str(e))
    return out


def logged():
    out = stream.getvalue().split()
    stream.seek(0)
    stream.truncate()
    return out


def log_kw(msg):
    log.info(msg, extra={'k': 1})


def log_kw_caller():
    logged()
    log_kw(msg='kw')
    return logged()


class Logs:
    def __init__(self, name):
        self.name = name
        log.info('init %s', name)


def log_init_caller():
    logged()
    Logs('a')
    return logged()


def log_gen():
    for i in range(2):
        log.info('gen %d', i)
        yield i


def log_gen_caller():
    logged()
    total = sum(log_gen())
    return logged(), total


def warn_kw(stacklevel):
    warnings.warn('kw', UserWarning, stacklevel=stacklevel)


def warn_gen(stacklevel):
    warnings.warn('gen', UserWarning, stacklevel=stacklevel)
    yield 1


class Warns:
    def __init__(self, stacklevel):
        warnings.warn('init', UserWarning, stacklevel=stacklevel)


def warn_through(level):
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter('always')
        warn_kw(stacklevel=level)
        Warns(level)
        list(warn_gen(level))
    return [(str(w.message), w.lineno - warn_through.__code__.co_firstlineno)
            for w in caught]


class FloatSubclass(float):
    pass


class GivesFloatSubclass:
    def __float__(self):
        return FloatSubclass(0.5)


def c_warning_callee():
    float(GivesFloatSubclass())


def c_warning_places():
    """a warning C raises names where it was raised: in the frame ceval
    entered, and in a callee that frame calls directly; recorded, then
    shown with its source line"""
    shown = io.StringIO()
    stderr = sys.stderr
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter('always')
        float(GivesFloatSubclass())
        c_warning_callee()
    with warnings.catch_warnings():
        warnings.simplefilter('always')
        sys.stderr = shown
        try:
            float(GivesFloatSubclass())
            c_warning_callee()
        finally:
            sys.stderr = stderr
    first = c_warning_places.__code__.co_firstlineno
    return ([(w.filename == __file__, w.lineno - first) for w in caught],
            shown.getvalue().replace(__file__, 'FILE'))


def back_names(n):
    f = sys._getframe(1)
    out = []
    while f is not None and len(out) < n:
        out.append(f.f_code.co_name)
        f = f.f_back
    return out


def own_frame(_):
    return sys._getframe()


def own_frame_through_c():
    return list(map(own_frame, [0]))[0]


# a frame that outlives its call through C from a compiled callee keeps
# that callee as f_back
def returned_back_names():
    f = own_frame_through_c()
    out = []
    while f is not None and len(out) < 3:
        out.append(f.f_code.co_name)
        f = f.f_back
    return out


def own_frame_interpreted(_):          # locals(): never compiled
    locals()
    return sys._getframe()


def interpreted_through_c():
    return list(map(own_frame_interpreted, [0]))[0]


# the same for a frame the interpreter runs
def returned_back_names_interpreted():
    f = interpreted_through_c()
    out = []
    while f is not None and len(out) < 3:
        out.append(f.f_code.co_name)
        f = f.f_back
    return out


def own_frame_raises(_):              # locals(): never compiled
    locals()
    raise ValueError


def raises_through_c():
    return list(map(own_frame_raises, [0]))


# the same for a frame the interpreter runs that an exception leaves,
# whose f_back is the frame of the callee's traceback entry
def raised_back_names_interpreted():
    try:
        raises_through_c()
    except ValueError as e:
        tb = e.__traceback__
    entries = []
    while tb is not None:
        entries.append(tb.tb_frame)
        tb = tb.tb_next
    f = entries[-1]
    out = []
    while f is not None and len(out) < 3:
        out.append(f.f_code.co_name)
        f = f.f_back
    return out, entries[-1].f_back is entries[-2]


def own_frame_or_none(keep, collect):  # locals(): never compiled
    locals()
    f = sys._getframe()
    if collect:
        gc.collect()
    if keep:
        return f
    del f
    return None


# a C function that calls with no object of its own made
_call = getattr(operator, 'call', lambda f, *a: f(*a))


def drops_own_frame_through_c():
    return _call(own_frame_or_none, False, False)


def keeps_own_frame_through_c():
    return _call(own_frame_or_none, True, True)


# a frame that outlives its call from the frame itself keeps it as f_back,
# though the next call through C from a compiled callee makes a frame
# object of the same code and lets it go; one kept through a collection
# keeps the callee
def back_names_kept_and_dropped():
    direct = own_frame_or_none(True, False)
    drops_own_frame_through_c()
    collected = keeps_own_frame_through_c()
    out = []
    for f, n in ((direct, 2), (collected, 3)):
        names = []
        while f is not None and len(names) < n:
            names.append(f.f_code.co_name)
            f = f.f_back
        out.append(names)
    return out


def module_names():
    getname = getattr(sys, '_getframemodulename', None)
    if getname is None:                  # before 3.12
        return [sys._getframe(d).f_globals['__name__'] for d in range(4)]
    return [getname(d) for d in range(4)]


def ask_module_names():
    for _ in range(1):
        pass
    return module_names()


# sys._getframemodulename counts compiled frames: the last is the harness's
def caller_module_name():
    return ask_module_names()


# TypeVar, ParamSpec, TypeVarTuple, TypeAliasType and sentinel take
# __module__ from the frame of the function that makes them, which C reads
# itself where they are C; here that function is compiled and another
# module's, and called from the case's frame
_made_elsewhere = {'__name__': 'tp_elsewhere'}
exec('def make(kind, name):\n'
     '    return kind(name).__module__\n'
     'def make_alias(kind, name):\n'
     '    return kind(name, int).__module__\n', _made_elsewhere)


def module_of_made_objects():
    import typing
    import builtins
    make = _made_elsewhere['make']
    out = [make(typing.TypeVar, 'T')]
    if hasattr(typing, 'ParamSpec'):
        out.append(make(typing.ParamSpec, 'P'))
    if hasattr(typing, 'TypeVarTuple'):
        out.append(make(typing.TypeVarTuple, 'Ts'))
    if hasattr(typing, 'TypeAliasType'):
        out.append(_made_elsewhere['make_alias'](typing.TypeAliasType, 'A'))
    if hasattr(builtins, 'sentinel'):
        out.append(make(builtins.sentinel, 'S'))
    return out


def gen_frames():
    yield back_names(3)


def init_frames_caller():
    class C:
        def __init__(self):
            self.names = back_names(3)
    return C().names, next(gen_frames())


def raised_back():
    try:
        Raises(kw=1)
    except RuntimeError as e:
        tb = e.__traceback__
        while tb.tb_next is not None:
            tb = tb.tb_next
        return tb.tb_frame.f_code.co_name, tb.tb_frame.f_back.f_code.co_name


class Raises:
    def __init__(self, kw):
        raise RuntimeError(kw)


def raised_middle():
    return raised_back()


def raiser(k):
    raise KeyError(k)


def caught_chain():
    out = []
    for call in (lambda: Raises(kw=1), lambda: raiser(k=2)):
        try:
            call()
        except Exception as e:
            tb = e.__traceback__
            names = []
            while tb is not None:
                names.append((tb.tb_frame.f_code.co_name,
                              tb.tb_frame.f_back.f_code.co_name))
                tb = tb.tb_next
            out.append(names)
    return out


def caught_middle():
    return caught_chain()


# LeJIT replaces these two with its own functions
def replaced_functions():
    import _warnings
    import gc
    out = []
    for fn in (_warnings.warn, gc.get_referents):
        try:
            signature = str(inspect.signature(fn))
        except ValueError as e:
            signature = str(e)
        out.append((fn.__doc__, fn.__self__, fn.__module__, signature))
    return out


# a log record holds the time it was made: whether its ints are new objects
# or cached small ones depends on when it runs
LOGS = {'objects': "a log record's time decides whether its ints are new objects"}

CASES = [
    ('pos_call_chain', "[()]", {}),
    ('kw_call_chain', "[()]", {}),
    ('warn_stacklevel', "[()]", {}),
    ('log_caller', "[()]", LOGS),
    ('log_kw_caller', "[()]", LOGS),
    ('log_init_caller', "[()]", LOGS),
    ('log_gen_caller', "[()]", LOGS),
    ('warn_through', "[(1,), (2,)]", {}),
    ('c_warning_places', "[()]", {}),
    ('init_frames_caller', "[()]", {}),
    ('returned_back_names', "[()]", {}),
    ('returned_back_names_interpreted', "[()]", {}),
    ('raised_back_names_interpreted', "[()]", {}),
    ('back_names_kept_and_dropped', "[()]", {}),
    ('caller_module_name', "[()]", {}),
    ('module_of_made_objects', "[(), ()]", {}),
    ('raised_middle', "[()]", {}),
    ('caught_middle', "[()]",
     {'warmup': (20, 'a call site that gets new lambdas each round compiles '
                     'for them until it goes megamorphic, about 11 rounds')}),
    ('lineno_chain', "[()]", {}),
    ('attr_line', "[()]", {}),
    ('lasti_through_c', "[()]", {}),
    ('getframe_through', "[()]", {}),
    ('getframe_deep', "[()]", {}),
    ('getframe_returned', "[()]", {}),
    ('trim_arity_case', "[()]", {}),
    ('replaced_functions', "[()]", {}),
]
