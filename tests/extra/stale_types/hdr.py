# Shared by the stale-type checks, which change a class, method or global
# after code using it was compiled: run.py runs each one plain and with
# LeJIT, and the outputs must be the same.
import os, re, sys
if len(sys.argv) > 1 and sys.argv[1] == 'lejit':
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
    import lejit
    lejit.start()
def hot(f, *a):
    for _ in range(200):
        try: f(*a)
        except Exception: pass
def show(label, f, *a):
    try: r = re.sub(' at 0x[0-9a-f]+', ' at 0x?', repr(f(*a)))
    except Exception as e: r = '%s: %s' % (type(e).__name__, e)
    print(label, r)
