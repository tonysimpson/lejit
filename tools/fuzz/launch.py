# Runs one fuzz program under python3.7, plain or with Ni enabled for the
# whole process:  launch.py ROOT plain|ni PROGRAM
# With Ni it reports on stderr how many of the program's functions compiled.
import runpy, sys, types
root, mode, prog = sys.argv[1:4]
if mode == 'ni':
    sys.path.insert(0, root)
    import ni
g = runpy.run_path(prog, run_name='__main__')
if mode == 'ni':
    fs = [v for v in g.values() if isinstance(v, types.FunctionType)]
    n = sum(ni._code_info(f.__code__) == 'compiled' for f in fs)
    sys.stderr.write('ni-compiled %d/%d\n' % (n, len(fs)))
