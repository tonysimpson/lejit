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

L = [1, 2]

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
]
