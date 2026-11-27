# A caught exception makes no more frame objects than CPython makes for its
# traceback (counted in lejit._core._stats()['frame_objects']).
import sys

CHECKED = {'checked': 'reads lejit._core._stats'}


def _made():
    lejit = sys.modules.get('lejit')
    if lejit is None or not lejit._started:
        return 0
    return lejit._core._stats()['frame_objects']


def none_made(work, *args):
    before = _made()
    result = work(*args)
    return result, _made() - before


def _fails(i):
    if i % 3 == 0:
        raise ValueError(i)
    return i


def _catches(n):
    out = []
    for i in range(n):
        try:
            out.append(_fails(i))
        except ValueError:
            out.append(-1)
    return out


def exceptions(n):
    # CPython makes a frame object for each raising call and one for the
    # catching frame, which it keeps
    out, made = none_made(_catches, n)
    return out, made <= out.count(-1) + 1


def _interpreter_frames():
    try:
        import lejit._core
    except ImportError:
        return False
    return bool(dict(lejit._core._build['features']).get(
        'LE_HAS_INTERPRETER_FRAME'))


CASES = [
    ('exceptions', [(20,)],
     dict(CHECKED, xfail="looking up the innermost compiled frame makes the"
                         " root frame's frame object") if _interpreter_frames()
     else CHECKED),
]
