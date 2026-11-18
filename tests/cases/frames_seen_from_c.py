# What logging, warnings and line-number users see of compiled frames, from
# the adversarial suite's frames_logging (KNOWN_BUGS C11): the f_back of a
# frame a keyword call runs, warnings' stacklevel, which _warnings walks in
# C from tstate->frame, and emulated frames' current lines.
import io
import logging
import sys
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


def back_names(n):
    f = sys._getframe(1)
    out = []
    while f is not None and len(out) < n:
        out.append(f.f_code.co_name)
        f = f.f_back
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


CASES = [
    ('pos_call_chain', "[()]", {}),
    ('kw_call_chain', "[()]", {}),
    ('warn_stacklevel', "[()]", {}),
    ('log_caller', "[()]", {}),
    ('log_kw_caller', "[()]", {}),
    ('log_init_caller', "[()]", {}),
    ('log_gen_caller', "[()]", {}),
    ('warn_through', "[(1,), (2,)]", {}),
    ('init_frames_caller', "[()]", {}),
    ('raised_middle', "[()]", {}),
    ('caught_middle', "[()]", {}),
    ('lineno_chain', "[()]", {}),
]
