# Exceptions: catching (3.7's EXCEPT_HANDLER block), exc_info, raise forms,
# finally, and leaving an except clause by return/break/continue/raise.
import sys
exc_info = sys.exc_info  # a global call: LOAD_METHOD isn't supported

def catch_as(a, b):
    try:
        return a // b
    except ZeroDivisionError as e:
        return type(e).__name__

def exc_info_inside(a):
    try:
        a[5]
    except IndexError:
        return exc_info()[0] is IndexError

def exc_info_after(a):
    try:
        a[5]
    except IndexError:
        pass
    return exc_info()

def no_match(a):
    try:
        return a[5]
    except KeyError:
        return 'wrong'

def reraise(a):
    try:
        a[5]
    except IndexError:
        raise

def bare_raise_nothing():
    raise

def raise_from(a):
    try:
        a[5]
    except IndexError as e:
        raise ValueError('v') from e

def cause_kept(a):
    try:
        raise_from(a)
    except ValueError as e:
        return type(e.__cause__).__name__

def raise_class():
    raise KeyError

def raise_not_exception():
    raise 3

def nested(a):
    try:
        try:
            a[5]
        except KeyError:
            return 'inner'
    except IndexError:
        return 'outer'

def nested_exc_info(a):
    try:
        a[5]
    except IndexError:
        try:
            {}[1]
        except KeyError:
            pass
        return exc_info()[0] is IndexError

def finally_on_exception(a):
    r = []
    try:
        try:
            a[5]
        finally:
            r = r + [1]
    except IndexError:
        r = r + [2]
    return r

def loop_catch(n):
    c = 0
    for i in range(n):
        try:
            if i % 3 == 0:
                raise ValueError
            c = c + 1
        except ValueError:
            c = c + 10
    return c

def break_in_except(n):
    for i in range(n):
        try:
            raise ValueError
        except ValueError:
            break
    return exc_info()[0]

def continue_in_except(n):
    c = 0
    for i in range(n):
        try:
            raise ValueError
        except ValueError:
            c = c + 1
            continue
    return c, exc_info()[0]

def except_tuple(a):
    try:
        a[5]
    except (KeyError, IndexError):
        return 'caught'

def raise_in_except(a):
    try:
        a[5]
    except IndexError:
        {}[1]

def context_kept(a):
    try:
        raise_in_except(a)
    except KeyError as e:
        return type(e.__context__).__name__

# a finally block entered by an exception has it as the handled one, as
# ceval does (CPython's test_contextlib)
def raise_nested(inner, outer):
    try:
        raise inner
    finally:
        raise outer

def finally_context(a):
    out = []
    try:
        raise_nested(KeyError(1), ValueError(2))
    except ValueError as e:
        out.append(repr(e.__context__))
    try:
        raise TypeError('outer')
    except TypeError:
        try:
            raise_nested(KeyError(1), ValueError(2))
        except ValueError as e:
            out.append(repr(e.__context__))
            out.append(repr(e.__context__.__context__))
    return out

def exc_info_in_finally(a):
    try:
        try:
            a[5]
        finally:
            seen = exc_info()[0]
    except IndexError:
        pass
    return seen, exc_info()[0]

# leaving the finally block early drops the exception and restores exc_info
def return_from_finally(a):
    try:
        a[5]
    finally:
        return exc_info()[0]

def finally_return_then_info(a):
    return return_from_finally(a), exc_info()[0]

def break_in_finally(n):
    out = []
    for i in range(n):
        try:
            raise KeyError(i)
        finally:
            out.append(exc_info()[0])
            break
    out.append(exc_info()[0])
    return out

def reraise_from_finally(a):
    try:
        try:
            a[5]
        finally:
            a.append(0)
    except IndexError as e:
        return type(e).__name__, len(a), exc_info()[0]

L = [1, 2]

# 3.7's NameError message ("name ... is not defined")
def undefined_global(a):
    try:
        return a + not_defined_anywhere
    except NameError as e:
        return str(e)


CASES = [
    ('catch_as', [(5, 2), (5, 0)], {}),
    ('exc_info_inside', '[(L,)]', {}),
    ('exc_info_after', '[(L,)]', {}),
    ('no_match', '[(L,), ([0]*6,)]', {}),
    ('reraise', '[(L,)]', {}),
    ('bare_raise_nothing', [()], {}),
    ('raise_from', '[(L,)]', {}),
    ('cause_kept', '[(L,)]', {}),
    ('raise_class', [()], {}),
    ('raise_not_exception', [()], {}),
    ('nested', '[(L,)]', {}),
    ('nested_exc_info', '[(L,)]', {}),
    ('finally_on_exception', '[(L,)]', {}),
    ('loop_catch', [(0,), (10,)], {}),
    ('break_in_except', [(3,)], {}),
    ('continue_in_except', [(3,)], {}),
    ('except_tuple', '[(L,)]', {}),
    ('raise_in_except', '[(L,)]', {}),
    ('context_kept', '[(L,)]', {}),
    ('finally_context', '[(L,)]', {}),
    ('exc_info_in_finally', '[(L,)]', {}),
    ('finally_return_then_info', '[(L,)]', {}),
    ('break_in_finally', [(3,)], {}),
    ('reraise_from_finally', '[([1, 2],)]', {}),
    ('undefined_global', [(1,)], {}),
]


# The same with the classes passed in, so that no global is read: these
# compile wherever the handlers do. 'E' is caught, 'F' isn't.
def arg_catch_as(a, b, E):
    try:
        x = a // b
    except E as e:
        return e
    return x


def arg_bare_except(a, b):
    try:
        return a // b
    except:
        return -1


def arg_finally(a, b, E):
    r = 0
    try:
        try:
            r = a // b
        finally:
            r = r + 100
    except E:
        return -r
    return r


def arg_reraise(a, b, E):
    try:
        a // b
    except E:
        raise


def arg_raise_in_except(a, b, E, F):
    try:
        a // b
    except E:
        raise F


def arg_nested(a, b, E, F):
    try:
        try:
            a // b
        except F:
            return 'inner'
    except E:
        return 'outer'


def arg_loop_catch(n, E):
    c = i = 0
    while i < n:
        try:
            c = c + 10 // (i % 3)
        except E:
            c = c + 1
        i = i + 1
    return c


def arg_not_a_class(a, b, what):
    try:
        a // b
    except what:
        return 'caught'


def arg_raise_from(e, cause):
    raise e from cause


Z = ZeroDivisionError
CASES += [
    ('arg_catch_as', '[(5, 2, Z), (5, 0, Z)]', {}),
    ('arg_bare_except', [(5, 2), (5, 0)], {}),
    ('arg_finally', '[(5, 2, Z), (5, 0, Z)]', {}),
    ('arg_reraise', '[(5, 0, Z), (5, 1, Z)]', {}),
    ('arg_raise_in_except', '[(5, 0, Z, ValueError), (5, 0, Z, KeyError(3))]',
     {}),
    ('arg_nested', '[(5, 0, Z, KeyError), (5, 0, KeyError, Z),'
     ' (5, 0, KeyError, ValueError)]', {}),
    ('arg_loop_catch', '[(0, Z), (10, Z), (5, ValueError)]', {}),
    ('arg_not_a_class', '[(5, 0, 1), (5, 0, (Z, 1)), (5, 1, 1)]', {}),
    ('arg_raise_from', '[(ValueError(1), KeyError(2)), (ValueError, KeyError),'
     ' (ValueError(1), None), (ValueError(1), 3)]', {}),
]


# except with something that is not an exception class (or a tuple of
# them) raises TypeError, as ceval.c:cmp_outcome, rather than matching or
# letting the exception through (CPython's test_baseexception)
def except_not_a_class(what):
    try:
        try:
            raise ValueError
        except what:
            return 'caught'
    except TypeError as e:
        return 'TypeError: %s' % e
    except ValueError:
        return 'ValueError escaped'


CASES += [
    ('except_not_a_class',
     "[('spam',), (int,), (BaseException(),), ((ValueError, 'x'),),"
     " ((KeyError, ValueError),)]", {}),
]


# an exception the compiler finds inside an except clause (iterating an int
# it knows) has the exception being handled as its context
def context_in_except(a):
    try:
        try:
            a // 0
        except ZeroDivisionError:
            for i in a:
                pass
    except TypeError as e:
        return repr(e), repr(e.__context__)


CASES += [
    ('context_in_except', [(1,)], {}),
]


# 'print >> f' gets 3.7's hint (CPython's test_print)
def print_rshift(f):
    try:
        print >> f
    except TypeError as e:
        return str(e)


def other_rshift(f):
    try:
        len >> f
    except TypeError as e:
        return str(e)


CASES += [
    ('print_rshift', '[(sys.stderr,), (1,)]', {}),
    ('other_rshift', '[(sys.stderr,)]', {}),
]


# a bare raise adds no traceback entry for its own frame (ceval goes to
# exception_unwind); with no active exception the RuntimeError gets one
def tb_lines(e):
    out = []
    tb = e.__traceback__
    while tb is not None:
        out.append((tb.tb_frame.f_code.co_name, tb.tb_lineno))
        tb = tb.tb_next
    return out


def _fail():
    raise ValueError(1)


def _reraise_in_except():
    try:
        _fail()
    except ValueError:
        raise


def _reraise_outside():
    raise


def _reraise_from_callee():
    try:
        _fail()
    except ValueError:
        _reraise_outside()


def bare_raise_traceback(which):
    f = [_reraise_in_except, _reraise_outside, _reraise_from_callee][which]
    try:
        f()
    except (ValueError, RuntimeError) as e:
        return type(e).__name__, tb_lines(e)


CASES += [
    ('bare_raise_traceback', [(0,), (1,), (2,)], {}),
]


# A message naming a class is made when compiling, behind a guard on the
# class: renamed afterwards, the class is named by its new name, as CPython
# names it at each raise. Any other change to the class fails the guard
# too, and the message is still right.
import math


class Named:
    pass


class Other:
    pass


def message(f, *args):
    try:
        f(*args)
    except TypeError as e:
        return str(e)


def len_of(x):
    try:
        return len(x)
    except TypeError as e:
        return str(e)


def add(x, y):
    try:
        return x + y
    except TypeError as e:
        return str(e)


def less(x, y):
    try:
        return x < y
    except TypeError as e:
        return str(e)


def iterate(x):
    try:
        for _ in x:
            pass
    except TypeError as e:
        return str(e)


def tuple_concat(x):
    try:
        return (1,) + x
    except TypeError as e:
        return str(e)


def real_number(x):
    try:
        return math.sqrt(x)
    except TypeError as e:
        return str(e)


async def async_iterate(x):
    try:
        async for _ in x:
            pass
    except TypeError as e:
        return str(e)


def run_async(x):
    try:
        async_iterate(x).send(None)
    except StopIteration as s:
        return s.value


class NoAnext:
    def __aiter__(self):
        return Named()


def changed_class(cls, change, undo, calls):
    """each call before, after change(cls) and after undo(cls)"""
    r = []
    for _ in range(3):
        r.append([f(*args) for f, args in calls])
    change(cls)
    r.append([f(*args) for f, args in calls])
    undo(cls)
    r.append([f(*args) for f, args in calls])
    return r


def rename(cls):
    cls.__name__ = 'Renamed'


def unrename(cls):
    cls.__name__ = cls.__qualname__


def add_attribute(cls):
    cls.extra = 1


def remove_attribute(cls):
    del cls.extra


def named_calls():
    a, b = Named(), Other()
    return [(len_of, (a,)), (add, (a, 1)), (add, (a, b)), (add, (b, a)),
            (less, (a, b)), (iterate, (a,)), (tuple_concat, (a,)),
            (real_number, (a,)), (run_async, (a,)), (run_async, (NoAnext(),))]


def renamed_class():
    return changed_class(Named, rename, unrename, named_calls())


def changed_unrenamed_class():
    return changed_class(Named, add_attribute, remove_attribute,
                         named_calls())


CASES += [
    ('renamed_class', [()], {}),
    ('changed_unrenamed_class', [()], {}),
]
