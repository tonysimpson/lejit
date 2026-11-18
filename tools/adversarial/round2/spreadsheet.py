"""Spreadsheet whose formulas are compiled to Python lambdas with eval() and recalculated in dependency order, with cells and ranges that hold ints, then floats, strings, None and error values as edits arrive: storage-strategy and elements-kind transitions in collections (Bolz, Diekmann and Tratt, Storage Strategies for Collections in Dynamically Typed Languages, OOPSLA 2013; Bynens, Elements Kinds in V8, 2017)."""
import ast
import re

N = 110


class Err:
    __slots__ = ('code',)

    def __init__(self, code):
        self.code = code

    def __repr__(self):
        return '#' + self.code

    def __eq__(self, other):
        return isinstance(other, Err) and other.code == self.code

    def __hash__(self):
        return hash(self.code)


REF = re.compile(r'\b([A-H])([1-9][0-9]?)\b(?::([A-H])([1-9][0-9]?))?')


def col_index(c):
    return ord(c) - 65


def cells_in(a, r1, b, r2):
    return ['%s%d' % (chr(65 + c), r) for c in range(col_index(a), col_index(b) + 1)
            for r in range(int(r1), int(r2) + 1)]


def f_sum(values):
    total = 0
    for v in values:
        if isinstance(v, Err):
            return v
        if isinstance(v, (int, float)) and not isinstance(v, bool):
            total = total + v
    return total


def f_avg(values):
    nums = [v for v in values if isinstance(v, (int, float)) and not isinstance(v, bool)]
    if any(isinstance(v, Err) for v in values):
        return Err('VALUE')
    return f_sum(nums) / len(nums) if nums else Err('DIV/0')


def f_max(values):
    nums = [v for v in values if isinstance(v, (int, float))]
    return max(nums) if nums else 0


def f_concat(*parts):
    return ''.join('' if p is None else str(p) for p in parts)


def f_if(c, a, b):
    return a if c else b


def safe(op):
    def run(a, b):
        if isinstance(a, Err):
            return a
        if isinstance(b, Err):
            return b
        try:
            return op(0 if a is None else a, 0 if b is None else b)
        except ZeroDivisionError:
            return Err('DIV/0')
        except TypeError:
            return Err('VALUE')
    return run


ENV = {'SUM': f_sum, 'AVG': f_avg, 'MAX': f_max, 'CONCAT': f_concat, 'IF': f_if,
       '_add': safe(lambda a, b: a + b), '_sub': safe(lambda a, b: a - b),
       '_mul': safe(lambda a, b: a * b), '_div': safe(lambda a, b: a / b),
       '_gt': safe(lambda a, b: a > b)}


def translate(formula):
    deps = []

    def ref(m):
        if m.group(3):
            names = cells_in(m.group(1), m.group(2), m.group(3), m.group(4))
            deps.extend(names)
            return '[' + ','.join('v(%r)' % x for x in names) + ']'
        deps.append(m.group(1) + m.group(2))
        return 'v(%r)' % (m.group(1) + m.group(2))

    expr = REF.sub(ref, formula)
    tree = Rewrite().visit(ast.parse(expr, mode='eval'))
    wrapper = ast.parse('lambda v: None', mode='eval')
    wrapper.body.body = tree.body
    return ast.fix_missing_locations(wrapper), deps


OPNAMES = {ast.Add: '_add', ast.Sub: '_sub', ast.Mult: '_mul', ast.Div: '_div', ast.Gt: '_gt'}


class Rewrite(ast.NodeTransformer):
    def visit_BinOp(self, node):
        self.generic_visit(node)
        return ast.Call(func=ast.Name(id=OPNAMES[type(node.op)], ctx=ast.Load()),
                        args=[node.left, node.right], keywords=[])

    def visit_Compare(self, node):
        self.generic_visit(node)
        return ast.Call(func=ast.Name(id=OPNAMES[type(node.ops[0])], ctx=ast.Load()),
                        args=[node.left, node.comparators[0]], keywords=[])


class Sheet:
    def __init__(self):
        self.raw = {}
        self.values = {}
        self.formulas = {}
        self.deps = {}
        self.compiled = 0

    def set(self, name, content):
        self.raw[name] = content
        if isinstance(content, str) and content.startswith('='):
            tree, deps = translate(content[1:])
            self.formulas[name] = eval(compile(tree, '<cell %s>' % name, 'eval'), ENV)
            self.deps[name] = deps
            self.compiled += 1
        else:
            self.formulas.pop(name, None)
            self.deps.pop(name, None)
            self.values[name] = content
        self.recalc()

    def recalc(self):
        order = []
        state = {}

        def visit(n):
            s = state.get(n)
            if s == 1:
                return False
            if s == 2:
                return True
            state[n] = 1
            ok = all(visit(d) for d in self.deps.get(n, ()))
            state[n] = 2
            order.append((n, ok))
            return ok

        for name in self.formulas:
            visit(name)
        get = self.values.get
        for name, ok in order:
            f = self.formulas.get(name)
            if f is not None:
                self.values[name] = f(get) if ok else Err('CYCLE')

    def column(self, c):
        return [self.values.get('%s%d' % (c, r)) for r in range(1, 21)]


def session(k, variant):
    sh = Sheet()
    for r in range(1, 21):
        sh.set('A%d' % r, r * (k + 1))
        sh.set('B%d' % r, (r * 7 + k) % 13)
    sh.set('C1', '=SUM(A1:A20)')
    sh.set('C2', '=AVG(B1:B20)')
    sh.set('C3', '=C1/C2+MAX(A1:B20)')
    sh.set('C4', '=IF(C3>100,C1-C3,C3*2)')
    sh.set('C5', '=CONCAT(A1,"-",B2,"-",C4)')
    for r in range(6, 21):
        sh.set('C%d' % r, '=A%d*B%d+C%d' % (r, r, r - 1) if r > 6 else '=A6*B6')
    snaps = [sh.column('C')[:6]]
    edits = 12 + k % 5
    for e in range(edits):
        r = (e * 7 + k) % 20 + 1
        if variant == 0:
            sh.set('B%d' % r, (e * 11 + k) % 17)
        else:
            choice = e % 6
            sh.set('B%d' % r, [(e * 11) % 17, e / 4, 'text%d' % e, None, 0, '=B%d/0' % ((r % 20) + 1)][choice])
            if e == edits - 2:
                sh.set('A1', '=C20')
            if e == edits - 1:
                sh.set('A1', k + 1)
        snaps.append((sh.values.get('C1'), sh.values.get('C2'), sh.values.get('C4'),
                      sh.values.get('C20')))
    return snaps, sh.column('C'), sh.compiled


def main(n, variant=0):
    return [session(k, variant) for k in range(n)]
