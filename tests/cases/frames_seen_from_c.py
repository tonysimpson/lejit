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


CASES = [
    ('pos_call_chain', "[()]", {}),
    ('kw_call_chain', "[()]",
     {'xfail': "C11: a keyword call's real callee frame has f_back "
               'skipping the compiled caller'}),
    ('warn_stacklevel', "[()]", {}),
    ('log_caller', "[()]",
     {'xfail': "C11: logging's findCaller crosses a keyword call, whose "
               "real frame's f_back skips the compiled caller"}),
    ('lineno_chain', "[()]", {}),
]
