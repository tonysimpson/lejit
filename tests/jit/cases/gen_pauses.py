# A compiled generator body pauses its frame (or a virtual generator's
# record) itself at each yield: the place, the stack's depth and the state
# CPython's YIELD_VALUE leaves, and what Python wrote in the locals meanwhile
# taken out first (pycompiler.c:gen_take_written, gen_frame_pause). Each
# case shows what a tracer, a debugger reading a paused frame, and
# gi_frame.f_locals see, through next(), send(), a for loop and yield from.
import gc, inspect, sys


def work(x):
    return x


def compiled(*fns):
    lejit = sys.modules.get('lejit')
    # a release build can't say what compiled
    if lejit is None or not hasattr(lejit._core, '_code_info'):
        return True
    return all(lejit._core._code_info(f.__code__) == 'compiled'
               for f in fns)


def where(frame):
    # what a debugger reads of a paused frame, as lines from the code's
    # first, so the case doesn't depend on where it sits in the file
    if frame is None:
        return None
    first = frame.f_code.co_firstlineno
    return (frame.f_code.co_name, frame.f_lineno - first, frame.f_lasti)


def state(g):
    out = [inspect.getgeneratorstate(g), where(g.gi_frame)]
    if hasattr(g, 'gi_suspended'):
        out.append(g.gi_suspended)
    return out


def chain(g):
    # each generator a paused yield from chain steps, outermost first
    out = []
    while g is not None:
        out.append(where(g.gi_frame))
        g = getattr(g, 'gi_yieldfrom', None)
    return out


def counter(n):
    total = 0
    for i in range(n):
        total += i
        x = yield work(total)
        if x is not None:
            total = x
    yield ('end', total)
    return total


def two_yields(n):
    a = n
    yield work(a)
    b = a + 1
    yield work(b)
    try:
        c = b + 1
        yield work(c)
    finally:
        a = None
    yield work((a, b, c))


class Tree:
    def __init__(self, left, value, right):
        self.left = left
        self.value = value
        self.right = right

    def __iter__(self):
        if self.left:
            yield from self.left
        yield self.value
        if self.right:
            yield from self.right


def tree(values):
    n = len(values)
    if n == 0:
        return None
    i = n // 2
    return Tree(tree(values[:i]), values[i], tree(values[i + 1:]))


def walk(t):
    for x in t:
        yield x


# A debugger reading the paused frame: its line, f_lasti, the generator's
# state, between resumes by next() and send(), and down a yield from chain.
def read_while_paused(n):
    out = []
    for k in range(2):
        g = counter(n)
        out.append(state(g))
        out.append((next(g), state(g)))
        out.append((g.send(10), state(g)))
        for _ in range(n):
            try:
                out.append((next(g), state(g)))
            except StopIteration as e:
                out.append(('stop', e.value, state(g)))
                break
        g = two_yields(n)
        for x in g:
            out.append((x, state(g)))
        out.append(state(g))
        g = iter(tree(range(n * 3)))
        for x in g:
            out.append((x, chain(g)))
        g = walk(tree(range(n)))
        for x in g:
            out.append((x, state(g), chain(g)))
    return out, compiled(counter, two_yields)


# A for loop over a generator that it reads while paused, each time round.
def read_in_loop(n):
    out = []
    for k in range(2):
        g = counter(n)
        for x in g:
            out.append((x, where(g.gi_frame)))
        out.append(state(g))
    return out, compiled(counter)


# gi_frame.f_locals read after each pause: locals the body changed, bound
# late, deleted, or bound in a handler, as the pause left them.
def locals_body(n):
    a = n
    yield work(1)
    b = [a]
    a = 'changed'
    yield work(2)
    del b
    for i in range(n):
        c = i * 2
        yield work(c)
    try:
        raise KeyError(a)
    except KeyError as e:
        d = str(e)
    yield work(d)
    yield work(3)


def read_locals(n):
    out = []
    for k in range(2):
        g = locals_body(n)
        out.append(sorted(g.gi_frame.f_locals.items()))
        for x in g:
            out.append((x, sorted(g.gi_frame.f_locals.items())))
        out.append(g.gi_frame is None)
        g = locals_body(n)
        for x in g:
            pass
        out.append(g.gi_frame is None)
    return out, compiled(locals_body)


# The body asks for its own frame and writes its locals through it while it
# runs, then changes them itself: the pause keeps the body's values.
def writes_own(n):
    f = sys._getframe()
    a = n
    yield work(sorted(f.f_locals))
    f.f_locals['a'] = 'written'
    yield work(a)
    a = 'own'
    b = a
    f.f_locals['b'] = 'written'
    yield work((a, b))
    for i in range(n):
        f.f_locals['i'] = 'written'
        a = i
        yield work(a)
    del f


def read_own_writes(n):
    out = []
    for k in range(2):
        g = writes_own(n)
        for x in g:
            loc = g.gi_frame.f_locals
            out.append((x, loc.get('a'), loc.get('b'), loc.get('i')))
        g = writes_own(n)
        out.append(next(g))
        g.gi_frame.f_locals['a'] = 'outside'
        out.append([x for x in g])
    gc.collect()
    return out, compiled(writes_own)


# A tracer on while generators are resumed, started between resumes and
# started from inside a body: the events and lines it sees.
EVENTS = []


def tracer(frame, event, arg):
    if frame.f_code.co_name in ('counter', 'two_yields', '__iter__'):
        first = frame.f_code.co_firstlineno
        EVENTS.append((frame.f_code.co_name, event, frame.f_lineno - first))
        if event == 'line':
            EVENTS.append(('lasti', frame.f_lasti))
    return tracer


def starts_tracing(n):
    yield work(1)
    sys.settrace(tracer)
    yield work(2)
    sys.settrace(None)
    yield work(3)


def traced(n):
    out = []
    for k in range(2):
        g = counter(n)
        out.append(next(g))
        sys.settrace(tracer)
        try:
            out.append(g.send(5))
            out.append(next(g))
        finally:
            sys.settrace(None)
        out.append(next(g))
        out.append(state(g))
        sys.settrace(tracer)
        try:
            out.append(list(two_yields(n)))
            out.append(list(tree(range(n))))
        finally:
            sys.settrace(None)
        out.append(list(starts_tracing(n)))
        sys.settrace(None)
        out.append(list(counter(n)))
        out.append(EVENTS[:])
        del EVENTS[:]
    return out, compiled(two_yields)


CASES = [
    ('read_while_paused', [(3,)], {}),
    ('read_in_loop', [(3,)], {}),
    ('read_locals', [(2,)], {}),
    ('read_own_writes', [(2,)], {}),
    ('traced', [(2,)], {}),
]
