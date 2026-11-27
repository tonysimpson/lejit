# sys._getframe from compiled code: compiled callers have no real frame
# and are emulated. Only names and globals are compared;
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


def tb_inner(a):
    return a[5]


def tb_middle(a):
    x = 1
    return tb_inner(a) + x


# traceback frames are the compiled frames, linked to their callers
# (tb_frame.f_back is not None; CPython's test_exceptions)
def traceback_frames(a):
    try:
        tb_middle(a)
    except IndexError:
        tb = sys.exc_info()[2]
        out = []
        while tb is not None:
            f = tb.tb_frame
            out.append((f.f_code.co_name, tb.tb_lineno - f.f_code.co_firstlineno,
                        f.f_back.f_code.co_name if f.f_back else None))
            tb = tb.tb_next
        return out


class SeesCaller:
    def __init__(self, seen):
        self.seen = seen

    def __del__(self):
        f = sys._getframe(1)
        self.seen.append((f.f_code.co_name, sys._getframe(1).f_code.co_name))


def drops_local(seen):
    ref = SeesCaller(seen)


def drops_local_middle(seen):
    drops_local(seen)
    return 0


# a finaliser that a returning frame's locals run sees that frame's caller,
# as CPython pops the frame first (CPython's test_frame
# test_sneaky_frame_object_teardown)
def finaliser_caller():
    seen = []
    drops_local(seen)
    drops_local_middle(seen)
    list(map(drops_local, [seen]))
    return seen


def too_deep():
    return sys._getframe(10000)


# the frames sys._getframe's audit events name, while AUDITED is a list
AUDITED = None


def audit_hook(event, args):
    if event == 'sys._getframe' and AUDITED is not None:
        AUDITED.append(args[0])


if hasattr(sys, 'addaudithook'):
    sys.addaudithook(audit_hook)


def audited_inner(depth):
    try:
        f = sys._getframe(depth)
    except ValueError as e:
        f = e
    return f


def audited_names(n):
    # the frame each sys._getframe audit event names, as it returns: the
    # caller asked for (from 3.11) or the current frame; none for too deep
    # a stack from 3.11
    global AUDITED
    out = []
    for depth in (0, 1, 2, 1000):
        for i in range(n):
            AUDITED = []
            got = audited_inner(depth)
            out.append((depth, [f.f_code.co_name for f in AUDITED],
                        [f is got for f in AUDITED]))
            AUDITED = None
    return out


# sys._current_frames gives this thread's frame as sys._getframe does
def _this_thread_frame():
    import threading
    f = sys._current_frames()[threading.get_ident()]
    return [f.f_code.co_name, f.f_back.f_code.co_name,
            f is sys._getframe()]


def _middle_of_current():
    return _this_thread_frame()


def current_frames():
    return _middle_of_current()


CASES = [
    ('depth_names', "[(4,)]", {}),
    ('audited_names', [(2,)], {}),
    ('call_back_chain', "[()]", {}),
    ('call_caller_globals', "[()]", {}),
    ('make_namedtuple', "[()]", {'compiles_each_call': 'makes a namedtuple'}),
    ('too_deep', "[()]", {}),
    ('finaliser_caller', "[()]", {}),
    ('traceback_frames', "[([1],)]", {}),
    ('current_frames', [()], {}),
]
