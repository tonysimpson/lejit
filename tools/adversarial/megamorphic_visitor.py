"""Megamorphic dispatch in an AST visitor: getattr-built method names over twenty-six node types. Variant 1 evaluates other programs, with the variables' types swapped."""


class Node:
    __slots__ = ()


class Leaf(Node):
    __slots__ = ('value',)

    def __init__(self, value):
        self.value = value


class Unary(Node):
    __slots__ = ('arg',)

    def __init__(self, arg):
        self.arg = arg


class Binary(Node):
    __slots__ = ('left', 'right')

    def __init__(self, left, right):
        self.left = left
        self.right = right


class Num(Leaf): __slots__ = ()
class Str(Leaf): __slots__ = ()
class Var(Leaf): __slots__ = ()
class Neg(Unary): __slots__ = ()
class Not(Unary): __slots__ = ()
class Len(Unary): __slots__ = ()
class Abs(Unary): __slots__ = ()
class ToStr(Unary): __slots__ = ()
class Add(Binary): __slots__ = ()
class Sub(Binary): __slots__ = ()
class Mul(Binary): __slots__ = ()
class Div(Binary): __slots__ = ()
class Mod(Binary): __slots__ = ()
class Pow(Binary): __slots__ = ()
class Eq(Binary): __slots__ = ()
class Lt(Binary): __slots__ = ()
class Gt(Binary): __slots__ = ()
class And(Binary): __slots__ = ()
class Or(Binary): __slots__ = ()
class Max(Binary): __slots__ = ()
class Min(Binary): __slots__ = ()
class Concat(Binary): __slots__ = ()
class Index(Binary): __slots__ = ()
class Repeat(Binary): __slots__ = ()


class If(Node):
    __slots__ = ('cond', 'then', 'other')

    def __init__(self, cond, then, other):
        self.cond = cond
        self.then = then
        self.other = other


class Let(Node):
    __slots__ = ('name', 'value', 'body')

    def __init__(self, name, value, body):
        self.name = name
        self.value = value
        self.body = body


class Evaluator:
    def __init__(self, env):
        self.env = dict(env)
        self.steps = 0

    def visit(self, node):
        self.steps += 1
        return getattr(self, 'visit_' + node.__class__.__name__)(node)

    def visit_Num(self, n): return n.value
    def visit_Str(self, n): return n.value
    def visit_Var(self, n): return self.env.get(n.value, 0)
    def visit_Neg(self, n): return -num(self.visit(n.arg))
    def visit_Not(self, n): return not self.visit(n.arg)
    def visit_Len(self, n): return len(str(self.visit(n.arg)))
    def visit_Abs(self, n): return abs(num(self.visit(n.arg)))
    def visit_ToStr(self, n): return str(self.visit(n.arg))
    def visit_Add(self, n): return num(self.visit(n.left)) + num(self.visit(n.right))
    def visit_Sub(self, n): return num(self.visit(n.left)) - num(self.visit(n.right))
    def visit_Mul(self, n): return num(self.visit(n.left)) * num(self.visit(n.right)) % 100003
    def visit_Div(self, n): return num(self.visit(n.left)) // (num(self.visit(n.right)) or 1)
    def visit_Mod(self, n): return num(self.visit(n.left)) % (num(self.visit(n.right)) or 7)
    def visit_Pow(self, n): return pow(num(self.visit(n.left)), abs(num(self.visit(n.right))) % 4, 1009)
    def visit_Eq(self, n): return self.visit(n.left) == self.visit(n.right)
    def visit_Lt(self, n): return num(self.visit(n.left)) < num(self.visit(n.right))
    def visit_Gt(self, n): return num(self.visit(n.left)) > num(self.visit(n.right))
    def visit_And(self, n): return self.visit(n.left) and self.visit(n.right)
    def visit_Or(self, n): return self.visit(n.left) or self.visit(n.right)
    def visit_Max(self, n): return max(num(self.visit(n.left)), num(self.visit(n.right)))
    def visit_Min(self, n): return min(num(self.visit(n.left)), num(self.visit(n.right)))
    def visit_Concat(self, n): return (str(self.visit(n.left)) + str(self.visit(n.right)))[:20]
    def visit_Index(self, n):
        s = str(self.visit(n.left))
        return s[num(self.visit(n.right)) % len(s)] if s else ''
    def visit_Repeat(self, n): return str(self.visit(n.left))[:4] * (num(self.visit(n.right)) % 3)
    def visit_If(self, n): return self.visit(n.then) if self.visit(n.cond) else self.visit(n.other)

    def visit_Let(self, n):
        old = self.env.get(n.name)
        self.env[n.name] = self.visit(n.value)
        try:
            return self.visit(n.body)
        finally:
            self.env[n.name] = old


def num(v):
    if isinstance(v, (int, bool)):
        return int(v)
    return len(v) if isinstance(v, str) else 0


UNARY = [Neg, Not, Len, Abs, ToStr]
BINARY = [Add, Sub, Mul, Div, Mod, Pow, Eq, Lt, Gt, And, Or, Max, Min, Concat, Index, Repeat]


class Gen:
    def __init__(self, seed):
        self.seed = seed

    def next(self, k):
        self.seed = (self.seed * 1103515245 + 12345) & 0x7fffffff
        return self.seed % k

    def tree(self, depth):
        if depth == 0 or self.next(5) == 0:
            c = self.next(3)
            if c == 0:
                return Num(self.next(50))
            return Str('abcdefg'[:self.next(7) + 1]) if c == 1 else Var('xys'[self.next(3)])
        c = self.next(10)
        if c < 2:
            return UNARY[self.next(len(UNARY))](self.tree(depth - 1))
        if c == 2:
            return If(self.tree(depth - 1), self.tree(depth - 1), self.tree(depth - 1))
        if c == 3:
            return Let('xys'[self.next(3)], self.tree(depth - 1), self.tree(depth - 1))
        return BINARY[self.next(len(BINARY))](self.tree(depth - 1), self.tree(depth - 1))


PROGRAMS = [[Gen(i).tree(7) for i in range(60)], [Gen(i).tree(7) for i in range(1000, 1060)]]
ENVS = [{'x': 3, 'y': 7, 's': 'hello'}, {'x': 'xyzzy', 'y': True, 's': 11}]


def main(n, variant=0):
    ev = Evaluator(ENVS[variant])
    acc = 0
    for i in range(n):
        for p in PROGRAMS[variant]:
            r = ev.visit(p)
            acc = (acc * 7 + num(r) + (len(r) if isinstance(r, str) else 0)) % 1000000007
    return acc, ev.steps


N = 40

if __name__ == '__main__':
    print(main(N))
