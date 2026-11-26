# A frame object made for a compiled call while it runs, and held past the
# call's end, has the locals the call ended with, as CPython's has: a
# local set to None after a callee caught an exception no longer keeps
# its old value alive. In the frame LeCode_Run runs (entered from C) and in
# a compiled callee, found as a traceback frame's f_back and by
# sys._getframe; and asyncio's failed task, which its handlers' frames
# no longer keep from being freed.
import asyncio
import gc
import sys
import weakref


class Held:
    pass


def fail(box):
    try:
        raise ValueError
    except ValueError as e:
        box.append(e)


def caught_below(self, box):
    try:
        fail(box)
    finally:
        self = None
    return 1


def ended(back):
    return back.f_code.co_name, repr(back.f_locals.get('self', 'unbound'))


def entered_from_c(n):
    out = []
    for i in range(n):
        box, h = [], Held()
        w = weakref.ref(h)
        list(map(caught_below, [h], [box]))   # through C: a frame of its own
        del h
        back = box.pop().__traceback__.tb_frame.f_back
        out.append((w() is None, ended(back)))
    return out


class Handle:
    def __init__(self, box):
        self.box = box

    def run(self, read):
        try:
            list(map(fail_and_read, [self.box], [read]))
        except ValueError:
            pass
        self = None
        return 1


def fail_and_read(box, read):
    fail(box)
    if read:
        sys._getframe(1)            # the callee's frame, asked for


def compiled_callee(n, read):
    out = []
    for i in range(n):
        box = []
        h = Handle(box)
        h.run(read)
        w = weakref.ref(h)
        del h
        back = box.pop().__traceback__.tb_frame.f_back
        out.append((w() is None, ended(back) if back is not None else None))
    return out


def task_freed(n, task_type):
    cls = getattr(asyncio.tasks, task_type)
    out = []

    async def failing():
        1 / 0

    async def main():
        seen = []
        loop = asyncio.get_running_loop()
        loop.set_exception_handler(lambda loop, context: seen.append(
            type(context['exception']).__name__))
        cls(failing())
        await asyncio.sleep(0)
        return seen

    gc.disable()
    try:
        for i in range(n):
            out.append(asyncio.run(main()))
    finally:
        gc.enable()
    return out


CASES = [
    ('entered_from_c', [(3,)], {}),
    ('compiled_callee', [(3, False), (3, True)], {}),
    ('task_freed', [(2, '_PyTask'), (2, '_CTask')], {}),
]
