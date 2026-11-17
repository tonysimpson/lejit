# frame.clear() on the frame of running compiled code (traceback.clear_frames,
# which unittest's assertRaises calls) must raise RuntimeError, as ceval's
# f_executing makes it; it cleared the cells and arguments the compiled code
# borrows, and the next use crashed.
import traceback


def clear_in_except(x):
    y = [x]

    def g():
        return y
    out = []
    for i in range(3):
        try:
            raise ValueError(i)
        except ValueError as e:
            traceback.clear_frames(e.__traceback__)
            out.append(str(e))
            del e
    return out, g(), x


def clear_own_frame(x):
    y = [x]

    def g():
        return y
    try:
        raise ValueError
    except ValueError as e:
        try:
            e.__traceback__.tb_frame.clear()
        except RuntimeError as r:
            msg = str(r)
        del e
    return msg, g()


CASES = [
    ('clear_in_except', [(1,), (2,)], {}),
    ('clear_own_frame', [(1,)], {}),
]
