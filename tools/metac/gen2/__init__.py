import os
import sys

_HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
for _p in (_HERE, os.path.join(_HERE, 'third_party', 'pycparser')):
    if _p not in sys.path:
        sys.path.insert(0, _p)
