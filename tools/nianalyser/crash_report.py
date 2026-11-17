#!/usr/bin/env python3
"""Explain a crash in Ni-generated code.

Runs a script under pointbreak with nianalyser's tracer and, when the
process stops on a fatal signal (SIGSEGV, SIGBUS, SIGILL, SIGFPE, or
SIGABRT from a Psyco assertion), reports:
  - the faulting address, and the emitted chunk that contains it (Python
    file, line, bytecode offset, compiler call stack), or that it is outside
    all emitted code (a bad jump or call);
  - every word on the top of the stack that points into emitted code
    (return addresses and saved code pointers), with the same annotation,
    the most recent first;
  - the registers, and a disassembly of the chunk around the fault.

Usage (from the venv of doc/BUILD.md, Debug; Ni built with NI_TRACE=1):
    crash_report.py [--stack N] [--step-from K] python3.7 script.py [args...]

Each report says how many times compiled code was entered
(psyco_processor_run). With --step-from K the run single-steps from the
K-th entry to the crash and prints the last instructions executed, so a bad
jump shows where it came from (slow: use the last entry, or close to it).
"""
import collections
import os, signal, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pointbreak
import nianalyser
from pointbreak import proc_maps

FATAL = {signal.SIGSEGV, signal.SIGBUS, signal.SIGILL, signal.SIGFPE,
         signal.SIGABRT}


def mapping(db, address):
    """the /proc/pid/maps region holding address, as 'name+offset (perms)'
    (pointbreak has no public API for this: doc/pointbreak-journal.md)"""
    for m in proc_maps._maps(db._pid):
        if m.lower <= address < m.upper:
            perms = ('r' if m.r else '-') + ('w' if m.w else '-') + \
                    ('x' if m.x else '-')
            name = os.path.basename(m.pathname) if m.pathname else 'anon'
            return '%s+%#x (%s)' % (name, address - m.lower + m.offset, perms)
    return 'unmapped'


def where(tracer, address):
    """'file:line:func@offset  [compiler stack]' for emitted code, else None"""
    hits = list(tracer.code_gen[address])
    if not hits:
        return None
    data = hits[0].data
    src = data.python_src
    if src:
        filename, line, func, pos = src
        loc = '%s:%s %s @%s' % (os.path.basename(filename), line, func, pos)
    else:
        loc = '(no Python location)'
    stack = data.where
    if 'psyco_pycompiler_mainloop' in stack:
        stack = stack[stack.index('psyco_pycompiler_mainloop'):]
    return '%s  chunk %#x-%#x #%d\n        emitted by %s' % (
        loc, hits[0].begin, hits[0].end, data.sequence,
        ' < '.join(stack[:-1][-8:]))


def main():
    args = sys.argv[1:]
    depth = 64
    step_from = None
    while args[:1] in (['--stack'], ['--step-from']):
        if args[0] == '--stack':
            depth = int(args[1])
        else:
            step_from = int(args[1])
        args = args[2:]
    if not args:
        sys.exit(__doc__)
    db = pointbreak.create_debugger(' '.join("'%s'" % a for a in args))
    tracer = nianalyser.SimpleExecutionTracer(db)
    entries = [0]
    last = collections.deque(maxlen=4000)

    def entered(db):
        entries[0] += 1
        if entries[0] == step_from:
            db.raise_event('STEP')
        return True
    db.add_breakpoint('psyco_processor_run', entered)
    stepping = False
    while True:
        if stepping:
            evs = db.single_step()
            last.append(db.registers.rip)
            fatal = [e for e in evs if getattr(e, 'signal', None) in FATAL]
            if fatal:
                sig = fatal[0].signal
                break
            if [e for e in evs if e.name == pointbreak.EVENT_NAME_EXITED]:
                print('exited while stepping')
                return 0
            continue
        ev = db.next_event()
        if ev.name == 'STEP':
            stepping = True
            continue
        if ev.name == pointbreak.EVENT_NAME_EXITED:
            print('exited normally, status %s' % getattr(ev, 'status', '?'))
            return 0
        sig = getattr(ev, 'signal', None)
        if sig in FATAL:
            break
        if ev.name not in ('STOPPED', 'NI_CODE_GEN') and sig is None:
            continue
    regs = db.registers
    rip, rsp = regs.rip, regs.rsp
    print('compiled code entered %d times (psyco_processor_run)' % entries[0])
    if last:
        print('\nemitted chunks executed, most recent last (up to 12):')
        seen = []
        for a in last:
            h = list(tracer.code_gen[a])
            if h and (not seen or seen[-1] != h[0]):
                seen.append(h[0])
        for h in seen[-12:]:
            print('-- %s' % where(tracer, h.begin).split('\n')[0])
            tracer.fancy_disassemble(h.begin, h.end)
        print('\nlast instructions executed:')
        for a in list(last)[-25:-1]:
            w = where(tracer, a)
            print('  %#x %s' % (a, w.split('\n')[0] if w else
                                '%s %s' % (db.address_to_description(a),
                                           mapping(db, a))))
    print('signal %s at %#x (%s, %s)' % (signal.Signals(sig).name, rip,
                                         db.address_to_description(rip),
                                         mapping(db, rip)))
    w = where(tracer, rip)
    print('  fault is %s' % (w or 'OUTSIDE all emitted code: a bad jump, '
                                  'call or return'))
    print('  (%d chunks of emitted code recorded)' % len(tracer.code_gen))
    if not tracer.code_gen:
        print('  no chunks: is Ni built with NI_TRACE=1?')
    print('\ntop of stack (negative offsets: just popped; [rsp-0x8] == rip '
          'means a ret popped a bad address):')
    for k in range(-4, 6):
        word = db.read_fmt(rsp + 8 * k, '<Q')[0]
        print('  [rsp%+#x] %#018x %s  %s' % (8 * k, word,
                                             db.address_to_description(word),
                                             mapping(db, word)))
    print('\nstack words pointing into emitted code (most recent first):')
    for k in range(depth):
        try:
            word = db.read_fmt(rsp + 8 * k, '<Q')[0]
        except Exception:
            break
        w = where(tracer, word)
        if w:
            print('  [rsp+%#x] %#x  %s' % (8 * k, word, w))
    print('\nregisters:')
    for r in ('rax', 'rbx', 'rcx', 'rdx', 'rsi', 'rdi', 'rbp', 'rsp', 'r8',
              'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15'):
        v = getattr(regs, r)
        w = where(tracer, v)
        print('  %-4s %#018x %s' % (r, v, ('-> ' + w.split('\n')[0]) if w
                                    else db.address_to_description(v)))
    hits = list(tracer.code_gen[rip])
    if not hits:
        # the last emitted code that ran: where a bad jump came from
        for a in reversed(last):
            hits = list(tracer.code_gen[a])
            if hits:
                break
    if hits:
        h = hits[0]
        print('\nchunk %#x-%#x (%s):' % (h.begin, h.end,
                                         where(tracer, h.begin).split('\n')[1].strip()))
        tracer.fancy_disassemble(h.begin, h.end)
    db.sigkill()
    return 1


if __name__ == '__main__':
    sys.exit(main())
