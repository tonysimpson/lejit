# yield from inside a virtual generator: the generator it steps stays
# virtual too, its record held in the stepping one's frame while that one
# is paused, so a paused chain is records only. Each case checks that what
# Python sees is CPython's: values, sends and returns down the chain,
# exceptions and tracebacks through it, frames from inside it, the order
# things die in when the chain is dropped, closed or thrown into, and each
# way a generator of the chain is made real, followed by its use.
import gc, sys

LOG = []


class Noted:
    def __init__(self, name):
        self.name = name

    def __del__(self):
        LOG.append(('del', self.name))


def work(x):
    return x


def compiled(*fns):
    lejit = sys.modules.get('lejit')
    # a release build can't say what compiled
    if lejit is None or not hasattr(lejit._core, '_code_info'):
        return True
    return all(lejit._core._code_info(f.__code__) == 'compiled'
               for f in fns)


def run(f):
    try:
        return f()
    except Exception as e:
        return ('raised', type(e).__name__, str(e))


def tb_names(e):
    out = []
    tb = e.__traceback__
    while tb is not None:
        out.append((tb.tb_frame.f_code.co_name,
                    tb.tb_lineno - tb.tb_frame.f_code.co_firstlineno))
        tb = tb.tb_next
    return out


def caller_names(f):
    out = []
    while f is not None and len(out) < 5:
        out.append(f.f_code.co_name)
        f = f.f_back
    return out


# a tree whose __iter__ recurses by yield from, as pyperformance's
# generators benchmark does
class Tree:
    def __init__(self, left, value, right):
        self.left = left
        self.value = value
        self.right = right

    def __iter__(self):
        if self.left:
            yield from self.left
        yield work(self.value)
        if self.right:
            yield from self.right


def tree(values):
    if not values:
        return None
    i = len(values) // 2
    return Tree(tree(values[:i]), values[i], tree(values[i + 1:]))


def walk(t):
    out = []
    for x in t:
        out.append(x)
    return out


def trees(n):
    t = tree(list(range(n)))
    return [walk(t) for _ in range(3)], compiled(Tree.__iter__)


# sends, return values and the chain's end
def leaf(n):
    got = []
    for i in range(n):
        got.append((yield work(i)))
    return ('leaf', got)


def mid(n):
    r = yield from leaf(n)
    yield r
    return 'mid'


def top(n):
    r = yield from mid(n)
    yield r


def sends(n):
    out = []
    for x in top(n):
        out.append(x)
    g = top(n)
    out.append(next(g))
    for i in range(n + 2):
        out.append(run(lambda: g.send(i * 10)))
    return out, compiled(leaf, mid, top)


# exceptions out of the innermost, caught and not
def raising_leaf(n):
    yield work(n)
    raise ValueError(n)


def catching_mid(n):
    try:
        yield from raising_leaf(n)
    except ValueError as e:
        yield ('caught', str(e), tb_names(e))
    yield from raising_leaf(n + 1)


def raises(n):
    out = []
    try:
        for x in catching_mid(n):
            out.append(x)
    except ValueError as e:
        out.append(('out', str(e), tb_names(e)))
    return out


def stopping_leaf(n):
    yield work(n)
    raise StopIteration('inside')


def stopping_top(n):
    yield from stopping_leaf(n)


def pep479(n):
    out = []
    try:
        for x in stopping_top(n):
            out.append(x)
    except RuntimeError as e:
        out.append((str(e), repr(e.__cause__), tb_names(e)))
    return out


# frames from the innermost body: its callers are the chain, then the
# consumer
def frame_leaf(n):
    for i in range(n):
        f = sys._getframe()
        yield work((caller_names(f), f.f_back.f_lineno -
                    f.f_back.f_code.co_firstlineno))


def frame_mid(n):
    yield from frame_leaf(n)


def frame_top(n):
    yield from frame_mid(n)


def frames(n):
    out = []
    for x in frame_top(n):
        out.append(x)
    return out


# a frame object of a generator of the chain kept while it is paused
KEPT = []


def keeping_leaf(n):
    KEPT.append(sys._getframe(1))
    for i in range(n):
        yield work(i)


def keeping_top(n):
    q = Noted('top_local')
    yield from keeping_leaf(n)
    yield 'end'


def kept_frames(n):
    out = []
    LOG.clear()
    for x in keeping_top(n):
        f = KEPT[-1]
        out.append((x, f.f_code.co_name, f.f_lineno - f.f_code.co_firstlineno,
                    sorted(f.f_locals)))
    f = KEPT.pop()
    out.append((list(LOG), f.f_code.co_name))
    del f
    return out


# dropped, closed and thrown into while paused deep in the chain
def finally_leaf(n):
    a = Noted('leaf_local')
    try:
        for i in range(n):
            yield work(i)
    finally:
        LOG.append('leaf finally')


def finally_mid(n):
    b = Noted('mid_local')
    try:
        yield from finally_leaf(n)
    finally:
        LOG.append('mid finally')


def plain_leaf(n):
    a = Noted('plain_leaf')
    for i in range(n):
        yield work(i)


def plain_mid(n):
    b = Noted('plain_mid')
    yield from plain_leaf(n)


def drops(n):
    out = []
    LOG.clear()
    for x in finally_mid(n):
        if x == 1:
            break
    out.append(list(LOG))
    LOG.clear()
    for x in plain_mid(n):
        if x == 1:
            break
    out.append(list(LOG))
    LOG.clear()
    for x in plain_mid(n):
        pass
    out.append(list(LOG))
    LOG.clear()
    try:
        for x in finally_mid(n):
            work(1) // 0
    except ZeroDivisionError:
        out.append(list(LOG))
    LOG.clear()
    return out


def catching_leaf(n):
    for i in range(n):
        try:
            yield work(i)
        except KeyError as e:
            LOG.append(('leaf caught', str(e)))
            yield 'after'


def closes(n):
    out = []
    LOG.clear()
    g = finally_mid(n)
    out.append(next(g))
    g.close()
    out.append((list(LOG), g.gi_frame))
    LOG.clear()
    g = plain_mid(n)
    out.append(next(g))
    out.append(type(g.gi_yieldfrom).__name__)
    out.append(g.gi_yieldfrom.gi_code.co_name)
    out.append(g.gi_yieldfrom.gi_frame.f_lineno -
               plain_leaf.__code__.co_firstlineno)
    out.append(next(g))
    del g
    out.append(list(LOG))
    LOG.clear()
    g = top_catching(n)
    out.append(next(g))
    out.append(g.throw(KeyError('k')))
    out.append(next(g))
    out.append(run(lambda: g.throw(ValueError('v'))))
    out.append(list(LOG))
    return out


def top_catching(n):
    yield from catching_leaf(n)


# made real while paused: the generator that steps the chain is stored,
# then each use of it and of the one it steps
HELD = []


def holds(n):
    out = []
    g = plain_mid(n)
    for x in g:
        out.append(x)
        if x == 1:
            HELD.append(g)
            break
    g = HELD.pop()
    inner = g.gi_yieldfrom
    out.append((g.gi_running, inner.gi_running, inner.gi_code.co_name,
                sorted(inner.gi_frame.f_locals)))
    out.append(list(g))
    out.append(run(lambda: next(inner)))
    out.append(gc.is_tracked(inner))
    return out


# tracing started from the innermost body: the chain goes on in ceval
def tracing_leaf(n):
    yield work(1)
    sys.settrace(lambda *a: None)
    yield 2
    sys.settrace(None)
    yield caller_names(sys._getframe())


def tracing_top(n):
    yield from tracing_leaf(n)
    yield 'top'


# the same once the chain has paused and been resumed
def later_tracing_leaf(n):
    for i in range(n):
        yield work(i)
    sys.settrace(lambda *a: None)
    yield 'traced'
    sys.settrace(None)
    yield caller_names(sys._getframe())


def later_tracing_top(n):
    yield from later_tracing_leaf(n)
    yield caller_names(sys._getframe())


def traces(n):
    out = []
    for x in tracing_top(n):
        out.append(x)
    sys.settrace(None)
    for x in later_tracing_top(n):
        out.append(x)
    sys.settrace(None)
    return out


# a chain as deep as the recursion allows a run of
def chain(n):
    if n > 0:
        yield from chain(n - 1)
    yield work(n)


def deep(n):
    return walk(chain(n))


CASES = [
    ('trees', [(1,), (2,), (20,)], {}),
    ('sends', [(2,)], {}),
    ('raises', [(3,)], {}),
    ('pep479', [(3,)], {}),
    ('frames', [(2,)], {}),
    ('kept_frames', [(3,)], {}),
    ('drops', [(3,)], {}),
    ('closes', [(3,)], {}),
    ('holds', [(3,)], {}),
    ('traces', [(3,)], {}),
    ('deep', [(40,), (300,)], {}),
]
