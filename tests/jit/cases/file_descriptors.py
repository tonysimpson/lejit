# LeJIT opens no file descriptors while the program runs: a program that
# counts or reuses its own sees the same ones as plainly. A collection
# while compiled code runs has the owner scan unwind the C stack
# (frames.c:runs_unwind), and libunwind opens a pipe the first time it
# unwinds; the first call is the one that would see it.
import gc
import os


def open_fds():
    return len(os.listdir('/proc/self/fd'))


def collect_in_compiled_code(depth):
    # a class of its own gives the scan something short-lived to look at
    class Short:
        pass
    Short()
    if depth:
        return collect_in_compiled_code(depth - 1)
    before = open_fds()
    gc.collect()
    return open_fds() - before


CASES = [
    ('collect_in_compiled_code', [(3,), (3,), (0,)], {}),
]
