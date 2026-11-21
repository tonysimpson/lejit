#!/usr/bin/env python3
"""Show the machine code Ni generated for one Python function.

Runs a script under pointbreak with analyser's tracer, stops at
Py_FinalizeEx (so the process is still alive), and prints every code chunk
emitted for the named function, in emission order, each annotated with the
Python line, bytecode offset and the compiler call stack that wrote it.

Usage (from a venv with pointbreak, distorm3 and intervaltree):
    dump_function.py FUNC_NAME python3.7 script.py [args...]
Ni must be built with LE_TRACE=1, and the script must call lejit.start().
Promotion records and other data sit inline in the code and disassemble
as junk.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pointbreak
import analyser


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    func = sys.argv[1]
    cmd = ' '.join("'%s'" % a for a in sys.argv[2:])
    db = pointbreak.create_debugger(cmd)
    tracer = analyser.SimpleExecutionTracer(db)

    def stop(db):
        db.raise_event('STOP')
        return True
    db.add_breakpoint('Py_FinalizeEx', stop)
    while db.next_event().name != 'STOP':
        pass
    chunks = sorted((i for i in tracer.code_gen
                     if i.data.python_src and i.data.python_src[2] == func),
                    key=lambda i: i.data.sequence)
    print('%d chunks for %s' % (len(chunks), func))
    for i in chunks:
        _, line, _, pos = i.data.python_src
        print('--- line %s bytecode %s  [%#x-%#x]' % (line, pos, i.begin, i.end))
        tracer.fancy_disassemble(i.begin, i.end)
    db.sigkill()


if __name__ == '__main__':
    main()
