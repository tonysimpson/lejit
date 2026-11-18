"""Generic matrix library (multiply, power, Bareiss determinant, Gaussian solve) whose elements are small ints in one variant and ints overflowing past 2**63, Fractions, Decimals, floats and complex in the other: representation changes on overflow and arithmetic polymorphic per element (Rigo, Representation-based Just-in-Time Specialization and the Psyco Prototype, PEPM 2004; RPython JIT int_add_ovf and guard_no_overflow)."""
from decimal import Decimal, getcontext
from fractions import Fraction
import zlib

N = 500


class Matrix:
    __slots__ = ('rows', 'n', 'm')

    def __init__(self, rows):
        self.rows = [list(r) for r in rows]
        self.n = len(rows)
        self.m = len(rows[0]) if rows else 0

    @classmethod
    def identity(cls, n, one=1, zero=0):
        return cls([[one if i == j else zero for j in range(n)] for i in range(n)])

    def __mul__(self, other):
        if not isinstance(other, Matrix):
            return Matrix([[x * other for x in r] for r in self.rows])
        cols = list(zip(*other.rows))
        out = []
        for r in self.rows:
            row = []
            for c in cols:
                acc = r[0] * c[0]
                for k in range(1, len(c)):
                    acc = acc + r[k] * c[k]
                row.append(acc)
            out.append(row)
        return Matrix(out)

    def __add__(self, other):
        return Matrix([[a + b for a, b in zip(r, s)] for r, s in zip(self.rows, other.rows)])

    def __pow__(self, e):
        result = Matrix.identity(self.n, self.rows[0][0] ** 0, self.rows[0][0] * 0)
        base = self
        while e:
            if e & 1:
                result = result * base
            e >>= 1
            if e:
                base = base * base
        return result

    def mod(self, p):
        return Matrix([[x % p for x in r] for r in self.rows])

    def trace(self):
        t = self.rows[0][0]
        for i in range(1, self.n):
            t = t + self.rows[i][i]
        return t


def bareiss_det(mat):
    a = [r[:] for r in mat.rows]
    n = len(a)
    sign = 1
    prev = 1
    for k in range(n - 1):
        if a[k][k] == 0:
            for i in range(k + 1, n):
                if a[i][k] != 0:
                    a[k], a[i] = a[i], a[k]
                    sign = -sign
                    break
            else:
                return 0
        for i in range(k + 1, n):
            for j in range(k + 1, n):
                a[i][j] = (a[i][j] * a[k][k] - a[i][k] * a[k][j]) // prev
        prev = a[k][k]
    return sign * a[n - 1][n - 1]


def solve(mat, rhs):
    n = mat.n
    a = [r[:] + [b] for r, b in zip(mat.rows, rhs)]
    for col in range(n):
        piv = max(range(col, n), key=lambda r: abs(a[r][col]))
        if a[piv][col] == 0:
            return None
        a[col], a[piv] = a[piv], a[col]
        for r in range(n):
            if r != col:
                f = a[r][col] / a[col][col]
                a[r] = [x - f * y for x, y in zip(a[r], a[col])]
    return [a[i][n] / a[i][i] for i in range(n)]


def show(x):
    if isinstance(x, float):
        return '%.6g' % x
    if isinstance(x, complex):
        return '(%.6g%+.6gj)' % (x.real, x.imag)
    if isinstance(x, int) and abs(x) > 10 ** 30:
        return 'int~%de%d' % (int(str(abs(x))[:6]), len(str(abs(x))))
    return str(x)


def element(i, j, k, kind):
    v = (i * 7 + j * 3 + k) % 11 - 5
    if kind == 'int':
        return v
    if kind == 'big':
        return v * (1 << 62) + i - j
    if kind == 'frac':
        return Fraction(v, i + j + 1)
    if kind == 'dec':
        return Decimal(v) / Decimal(j + 3)
    if kind == 'float':
        return v / (i + 1.5)
    return complex(v, i - j)


def suite(k, kind, size):
    m = Matrix([[element(i, j, k, kind) for j in range(size)] for i in range(size)])
    if kind in ('int', 'big'):
        diag = Matrix.identity(size, 2, 0)
    elif kind == 'dec':
        diag = Matrix.identity(size, Decimal(2), Decimal(0))
    else:
        diag = Matrix.identity(size, element(0, 0, 6, kind) or 1, element(0, 0, 5, kind) * 0)
    m = m + diag
    p = m ** (6 + k % 4)
    out = [show(p.trace()), show(p.rows[0][size - 1])]
    if kind in ('int', 'big'):
        out.append(show(bareiss_det(m)))
        fib = Matrix([[1, 1], [1, 0]]) ** (40 + (k % 40 if kind == 'int' else 30 * (k % 40)))
        out.append(show(fib.rows[0][1]))
        out.append(show(fib.mod(1000003).rows[0][1]))
    else:
        rhs = [element(i, 0, k + 1, kind) for i in range(size)]
        x = solve(m, rhs)
        out.append(None if x is None else [show(v) for v in x[:3]])
    return out


def digest(results, keep):
    crc = 0
    for r in results:
        crc = zlib.crc32(repr(r).encode(), crc)
    return len(results), crc, results[:keep], results[-keep:]


def main(n, variant=0):
    getcontext().prec = 28
    kinds = ['int'] if variant == 0 else ['big', 'frac', 'dec', 'float', 'complex', 'int']
    out = []
    for k in range(n * 3):
        kind = kinds[k % len(kinds)]
        out.append((kind, suite(k, kind, 7 + k % 3)))
    return digest(out, 6)
