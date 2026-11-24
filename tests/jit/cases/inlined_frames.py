# A small body inlined into its compiled caller, seen from what it calls:
# the inlined frame's line, its own and its caller's locals, and the frame
# itself, one object for the call, which goes on with what was set on it,
# must be CPython's.
import sys


def where_called(x):
    # run by map, from C: the frames out of it
    f = sys._getframe(1)
    back = f.f_back
    return (x, f.f_code.co_name, f.f_lineno - f.f_code.co_firstlineno,
            back.f_code.co_name,
            sorted((k, v) for k, v in back.f_locals.items()
                   if not callable(v)))


def through_c(f, a, m):                # inlined: no loop, join or global
    b = a + 1
    return [*m(f, (a, b))]


def caller(n):
    out = {}
    for i in range(n):
        s = 's%d' % i
        t = i * 2.5
        out[i] = through_c(where_called, i, map)
    return out, s, t


class Seen:
    @property
    def place(self):
        f = sys._getframe(1)
        return (f.f_code.co_name, f.f_lineno - f.f_code.co_firstlineno,
                sorted(k for k in f.f_back.f_locals))


def read_place(o, k):                  # inlined; the property runs in C's call
    j = k * 3
    return j, o.place


def property_caller(n):
    o = Seen()
    out = {}
    for i in range(n):
        w = -i
        out[i] = read_place(o, i)
    return out, w


def start_tracing(tf):
    sys.settrace(tf)
    sys._getframe(1).f_trace = tf      # the inlined frame's


def traced_inline(tf, start):          # inlined: sets nothing itself
    start(tf)
    start = None


def traced(n):
    out = []
    for i in range(n):
        seen = []

        def tf(frame, event, arg):
            # (a resumed frame's caller during its return event isn't
            # CPython's yet)
            seen.append((frame.f_code.co_name, event,
                         event != 'return' and frame.f_back.f_code.co_name))
            return tf
        traced_inline(tf, start_tracing)
        sys.settrace(None)
        out.append(seen)
    return out


def same_frame():
    f = sys._getframe(1)
    return f is sys._getframe(1), f.f_code.co_name


def ask_twice(g):                      # inlined: asked for by its callee
    return g()


def ask_through_c(g):                  # inlined: asked for through map
    return [*map(lambda _: g(), (0,))]


def identity(n):
    return [(ask_twice(same_frame), ask_through_c(same_frame))
            for i in range(n)]


def caller_frame():
    return sys._getframe(1)


def line_now(f):
    return f.f_lineno - f.f_code.co_firstlineno


def two_calls(get, read):              # inlined: its frame outlives a call
    f = get()
    return f is get(), read(
        f)


def across_calls(n):
    return [two_calls(caller_frame, line_now) for i in range(n)]


def write_caller_local():
    sys._getframe(1).f_locals['x'] = 'written'


def writes_its_local(w):               # inlined: its local written by w
    x = 'mine'
    w()
    return x


def local_written(n):
    return [writes_its_local(write_caller_local) for i in range(n)]


def write_local_two_up():
    sys._getframe(2).f_locals['x'] = 'written'


def relay(w):                          # inlined: its callee writes its caller
    w()


def parent_local_written(n):
    out = []
    for i in range(n):
        x = 'mine'
        relay(write_local_two_up)
        out.append(x)
    return out


def callers_locals(up=1):
    return sorted((k, v) for k, v in sys._getframe(up).f_locals.items()
                  if not callable(v))


def own_locals(k, read):               # inlined: its callee reads its locals
    a = k + 1
    return read()


def own_locals_through_c(k, read):     # the same, through map
    a = k * 2
    return [*map(read, (1,))]


def locals_seen(n):
    return [(own_locals(i, callers_locals),
             own_locals_through_c(i, callers_locals))
            for i in range(n)]


def grab_then_fail(box):
    box.append(sys._getframe(1))
    return 1 / 0


def relay_fail(get, box):             # inlined: left by its callee's error
    x = 5
    return get(box)


def kept_after_error(n):
    # the frame outlives its code, which left it by an exception: it is
    # the traceback's, with the locals as they ended, and the next call has
    # a frame of its own
    out = []
    for i in range(n):
        box, tbs = [], []
        for j in range(2):
            try:
                relay_fail(grab_then_fail, box)
            except ZeroDivisionError as e:
                tbs.append(e.__traceback__.tb_next.tb_frame)
        out.append((box[0].f_code.co_name, box[0] is box[1],
                    box[0] is tbs[0], box[0].f_locals.get('x')))
    return out


CASES = [
    ('caller', [(3,)], {}),
    ('property_caller', [(3,)], {}),
    ('traced', [(3,)], {}),
    ('identity', [(3,)], {}),
    ('across_calls', [(3,)], {}),
    ('local_written', [(3,)], {}),
    ('parent_local_written', [(3,)], {}),
    ('locals_seen', [(3,)], {}),
    ('kept_after_error', [(3,)], {}),
]
