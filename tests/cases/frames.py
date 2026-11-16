# sys._getframe from compiled code: compiled callers have no real frame
# and are emulated (KNOWN_BUGS C11; history B2). Only names and globals are compared;
# emulated frames have no locals and report the first line.
import collections
import sys

MARK = 'frames module'


def names_up(n):
    out = []
    for i in range(n):
        out.append(sys._getframe(i).f_code.co_name)
    return out


def middle(n):
    return names_up(n)


def outer(n):
    return middle(n)


def depth_names(n):
    return outer(n)[:4]


def back_chain():
    f = sys._getframe(0)
    out = []
    for i in range(4):
        out.append(f.f_code.co_name)
        f = f.f_back
    return out


def call_back_chain():
    return back_chain()


def caller_globals():
    return sys._getframe(1).f_globals['MARK']


def call_caller_globals():
    return caller_globals()


def make_namedtuple():
    # namedtuple sets __module__ from sys._getframe(1)
    P = collections.namedtuple('P', 'x y')
    return P.__module__, P(1, 2)


def too_deep():
    return sys._getframe(10000)


CASES = [
    ('depth_names', "[(4,)]", {}),
    ('call_back_chain', "[()]", {}),
    ('call_caller_globals', "[()]", {}),
    ('make_namedtuple', "[()]", {'leaks': 'C14 (each namedtuple compiles new code)'}),
    ('too_deep', "[()]", {}),
]
