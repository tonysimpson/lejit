# The with statement: SETUP_WITH, WITH_CLEANUP_START, WITH_CLEANUP_FINISH,
# on LeJIT's finally machinery (the three finally states: None, a pseudo-
# exception's 3-tuple, an exception's six items).
import contextlib
import sys
import threading


class CM:
    """Records what happens; __exit__ returns `suppress`."""
    def __init__(self, log, name='cm', suppress=False, enter_value=None):
        self.log = log
        self.name = name
        self.suppress = suppress
        self.enter_value = enter_value

    def __enter__(self):
        self.log.append(('enter', self.name))
        return self if self.enter_value is None else self.enter_value

    def __exit__(self, tp, val, tb):
        self.log.append(('exit', self.name, tp and tp.__name__,
                         str(val) if val is not None else None,
                         tb is not None,
                         sys.exc_info()[0] and sys.exc_info()[0].__name__))
        return self.suppress


class Falsy:
    def __bool__(self):
        return False


class Truthy:
    def __bool__(self):
        return True


class BadBool:
    def __bool__(self):
        raise RuntimeError('bad bool')


class ResultCM:
    """__exit__ returns a given object (tests its truth value)."""
    def __init__(self, result):
        self.result = result

    def __enter__(self):
        return 1

    def __exit__(self, *a):
        return self.result


class RaisingExit:
    def __enter__(self):
        return 1

    def __exit__(self, *a):
        raise KeyError('from exit')


class RaisingEnter:
    def __enter__(self):
        raise ValueError('from enter')

    def __exit__(self, *a):
        raise AssertionError('not reached')


class NoEnter:
    def __exit__(self, *a):
        pass


class NoExit:
    def __enter__(self):
        return 1


class InstanceEnter:
    """__enter__/__exit__ on the instance only: special lookup ignores them."""
    def __init__(self):
        self.__enter__ = lambda: 1
        self.__exit__ = lambda *a: None


def normal(x):
    log = []
    with CM(log) as c:
        log.append(('body', x, c.name))
    return log


def value_from_enter(x):
    log = []
    with CM(log, enter_value=x) as v:
        r = v * 2
    return r, log


def no_target(x):
    log = []
    with CM(log):
        x += 1
    return x, log


def propagated(x):
    log = []
    try:
        with CM(log):
            log.append('before')
            1 / x
            log.append('after')
    except ZeroDivisionError as e:
        log.append(('caught', str(e), sys.exc_info()[0].__name__))
    log.append(('after try', sys.exc_info()[0]))
    return log


def suppressed(x):
    log = []
    with CM(log, suppress=True):
        log.append('before')
        if x:
            raise ValueError('v%d' % x)
        log.append('no raise')
    log.append(('after', sys.exc_info()[0]))
    return log


def exit_result(r):
    # __exit__'s result decides by truth value, only for an exception
    out = []
    for raise_it in (False, True):
        try:
            with ResultCM(r):
                if raise_it:
                    raise ValueError('x')
            out.append('fell through')
        except ValueError:
            out.append('raised')
    return out


def return_inside(x):
    log = []
    with CM(log):
        if x:
            return ('returned', log)
        log.append('no return')
    return ('end', log)


def return_value_kept(x):
    log = []

    def inner():
        with CM(log, suppress=True):
            return x * 3
    return inner(), log


def break_continue(n):
    log = []
    for i in range(n):
        with CM(log, name=i):
            if i == 1:
                continue
            if i == 3:
                break
            log.append(('body', i))
    return log


def loop_with(n):
    t = 0
    lock = threading.Lock()
    for i in range(n):
        with lock:
            t += i
    return t, lock.locked()


def nested(x):
    log = []
    with CM(log, 'a'):
        with CM(log, 'b', suppress=x > 1):
            with CM(log, 'c'):
                if x:
                    raise KeyError(x)
    return log


def nested_outer_suppresses(x):
    log = []
    with CM(log, 'a', suppress=True):
        with CM(log, 'b'):
            raise KeyError(x)
    return log


def two_managers(x):
    log = []
    with CM(log, 'a') as a, CM(log, 'b', suppress=x) as b:
        log.append((a.name, b.name))
        raise IndexError('ix')
    return log


def exit_raises(x):
    log = []
    try:
        with RaisingExit():
            if x:
                raise ValueError('body')
    except KeyError as e:
        c = e.__context__
        log.append(('key', str(e), type(c).__name__ if c else None))
    return log


def enter_raises(x):
    try:
        with RaisingEnter():
            return 'body'
    except ValueError as e:
        return str(e)


def enter_missing(x):
    try:
        with NoEnter():
            return 'body'
    except AttributeError as e:
        return str(e), e.args


def exit_missing(x):
    try:
        with NoExit():
            return 'body'
    except AttributeError as e:
        return str(e)


def instance_methods(x):
    with InstanceEnter():
        return 'body'


def not_a_manager(x):
    with x:
        return 'body'


def bad_bool(x):
    try:
        with ResultCM(BadBool()):
            raise ValueError('v')
    except RuntimeError as e:
        return str(e), type(e.__context__).__name__


def bad_bool_no_exception(x):
    # no exception: __exit__'s result isn't tested at all
    with ResultCM(BadBool()):
        pass
    return 'ok'


@contextlib.contextmanager
def managed(log, swallow):
    log.append('setup')
    try:
        yield 42
    except ValueError:
        log.append('handled')
        if not swallow:
            raise
    finally:
        log.append('cleanup')


def contextlib_manager(x):
    log = []
    try:
        with managed(log, x > 1) as v:
            log.append(v)
            if x:
                raise ValueError(x)
    except ValueError as e:
        log.append(('out', str(e)))
    return log


def contextlib_suppress(x):
    with contextlib.suppress(KeyError, IndexError):
        {}[x]
    return 'suppressed'


def exitstack(n):
    log = []
    with contextlib.ExitStack() as stack:
        for i in range(n):
            stack.enter_context(CM(log, i))
        log.append('body')
    return log


def with_in_except(x):
    log = []
    try:
        raise KeyError('outer')
    except KeyError:
        with CM(log, suppress=True):
            raise ValueError('inner')
        log.append(('still handling', sys.exc_info()[0].__name__))
    log.append(('after', sys.exc_info()[0]))
    return log


def with_in_finally(x):
    log = []
    try:
        try:
            if x:
                raise KeyError('k')
        finally:
            with CM(log):
                log.append(('in finally', sys.exc_info()[0] and
                            sys.exc_info()[0].__name__))
    except KeyError:
        log.append('caught')
    return log


def try_finally_in_with(x):
    log = []
    with CM(log, suppress=True):
        try:
            if x:
                raise KeyError('k')
            return log
        finally:
            log.append('finally')
    log.append('after')
    return log


def exc_info_restored(x):
    # after a suppressed exception, the handled exception is the outer one
    out = []
    try:
        raise TypeError('outer')
    except TypeError:
        with CM([], suppress=True):
            raise ValueError('inner')
        out.append(sys.exc_info()[0].__name__)
    out.append(sys.exc_info()[0])
    return out


def loop_mixed(n):
    # every exit path in one hot loop: the rarer ones compile later
    log = []
    t = 0
    for i in range(n):
        try:
            with CM(log, i, suppress=i % 5 == 0):
                if i % 3 == 0:
                    raise ValueError(i)
                if i % 7 == 0:
                    continue
                if i == n - 2:
                    break
                t += i
        except ValueError:
            t -= 1
    return t, len(log)


def file_like(x):
    import io
    with io.StringIO() as f:
        f.write('abc' * x)
        v = f.getvalue()
    return v, f.closed


CASES = [
    ('normal', [(1,), (2,)], {}),
    ('value_from_enter', [(3,), (4.5,)], {}),
    ('no_target', [(1,)], {}),
    ('propagated', [(0,), (1,)], {}),
    ('suppressed', [(0,), (1,), (2,)], {}),
    ('exit_result', "[(None,), (0,), (1,), ('',), ('x',), (Falsy(),), "
                    "(Truthy(),), ([],)]", {}),
    ('return_inside', [(0,), (1,)], {}),
    ('return_value_kept', [(2,)], {}),
    ('break_continue', [(5,), (2,)], {}),
    ('loop_with', [(100,)], {}),
    ('nested', [(0,), (1,), (2,)], {}),
    ('nested_outer_suppresses', [(1,)], {}),
    ('two_managers', [(False,), (True,)], {}),
    ('exit_raises', [(0,), (1,)], {}),
    ('enter_raises', [(0,)], {}),
    ('enter_missing', [(0,)], {}),
    ('exit_missing', [(0,)], {}),
    ('instance_methods', [(0,)], {}),
    ('not_a_manager', [(1,), (None,)], {}),
    ('bad_bool', [(0,)], {}),
    ('bad_bool_no_exception', [(0,)], {}),
    ('contextlib_manager', [(0,), (1,), (2,)], {}),
    ('contextlib_suppress', [(1,), ('k',)], {}),
    ('exitstack', [(0,), (3,)], {}),
    ('with_in_except', [(0,)], {}),
    ('with_in_finally', [(0,), (1,)], {}),
    ('try_finally_in_with', [(0,), (1,)], {}),
    ('exc_info_restored', [(0,)], {}),
    ('loop_mixed', [(40,), (3,)], {}),
    ('file_like', [(2,)], {}),
]
