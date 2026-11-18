import sys, time
sys.path.insert(0, sys.argv[1])
import pycparser
from pycparser import c_parser
for f in sys.argv[2:]:
    t=time.time()
    try:
        ast = c_parser.CParser().parse(open(f).read(), f)
        print(f, 'OK', len(ast.ext), '%.1fs'%(time.time()-t))
    except Exception as e:
        print(f, 'FAIL', str(e)[:200])
