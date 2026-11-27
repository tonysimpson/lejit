# A function whose __code__ is replaced while it runs, as lazily compiled
# decorators do (networkx's argmap): the call runs the old code once, and
# only later calls run the new. Each function has the code it is reset to
# from the start, so the calls are compiled for that code.

log = []


def real(tag):
    log.append(tag)
    return tag


def lazy_self_call(tag):
    lazy_self_call.__code__ = real.__code__
    return lazy_self_call(tag)


def lazy_swap_return(tag):
    lazy_swap_return.__code__ = real.__code__
    return 'old ' + tag


def swap_callee():
    lazy_swap_in_callee.__code__ = real.__code__


def lazy_swap_in_callee(tag):
    swap_callee()
    return 'old ' + tag


def lazy_star_self_call(*args, **kwargs):
    lazy_star_self_call.__code__ = real.__code__
    return lazy_star_self_call(*args, **kwargs)


def with_extra(tag, extra=0):
    log.append((tag, extra))
    return tag, extra


def lazy_new_defaults(tag, extra=0):
    lazy_new_defaults.__defaults__ = (1,)
    lazy_new_defaults.__code__ = with_extra.__code__
    return lazy_new_defaults(tag)


LAZY = [lazy_self_call, lazy_swap_return, lazy_swap_in_callee,
        lazy_star_self_call, lazy_new_defaults]
FIRST = {f: (f.__code__, f.__defaults__) for f in LAZY}


def reset(f):
    f.__code__, f.__defaults__ = FIRST[f]
    del log[:]


def self_call(tag):
    out = []
    for t in (tag, tag * 2):
        reset(lazy_self_call)
        out.append((lazy_self_call(t), lazy_self_call(t + '!'), list(log)))
    return out


def swap_return(tag):
    out = []
    for t in (tag, tag * 2):
        reset(lazy_swap_return)
        out.append((lazy_swap_return(t), lazy_swap_return(t + '!'), list(log)))
    return out


def swap_in_callee(tag):
    out = []
    for t in (tag, tag * 2):
        reset(lazy_swap_in_callee)
        out.append((lazy_swap_in_callee(t), lazy_swap_in_callee(t + '!'),
                    list(log)))
    return out


def star_self_call(tag):
    out = []
    for t in (tag, tag * 2):
        reset(lazy_star_self_call)
        out.append((lazy_star_self_call(t), lazy_star_self_call(t + '!'),
                    list(log)))
    return out


def new_defaults(tag):
    out = []
    for t in (tag, tag * 2):
        reset(lazy_new_defaults)
        out.append((lazy_new_defaults(t), lazy_new_defaults(t + '!'),
                    list(log)))
    return out


# called from C: map calls the function, as it calls a decorator's wrapper
def from_c(tag):
    out = []
    for tags in ([tag], [tag, tag * 2]):
        reset(lazy_self_call)
        out.append((list(map(lazy_self_call, tags)), list(log)))
    return out


# a closure that replaces its own code: called with its cells read at run
# time, while it has the code it was compiled for
def make_lazy_closure(seen):
    def lazy_closure(x):
        lazy_closure.__code__ = closure_code
        return seen and lazy_closure(x)
    return lazy_closure


def make_real_closure(seen):
    def lazy_closure(x):
        seen.append(x)
        return lazy_closure and x
    return lazy_closure


closure_code = make_real_closure(None).__code__


def closure_self_call(tag):
    out = []
    for t in (tag, tag * 2):
        seen = [None]
        lazy_closure = make_lazy_closure(seen)
        out.append((lazy_closure(t), lazy_closure(t + '!'), seen))
    return out


CASES = [
    ('self_call', [('a',), ('b',)], {}),
    ('swap_return', [('a',), ('b',)], {}),
    ('swap_in_callee', [('a',), ('b',)], {}),
    ('star_self_call', [('a',), ('b',)], {}),
    ('new_defaults', [('a',), ('b',)], {}),
    ('from_c', [('a',), ('b',)], {}),
    ('closure_self_call', [('a',), ('b',)], {}),
]
