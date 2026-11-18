"""Deep recursion past the inlining and recursion limits: a recursive-descent parser on deeply nested input, tree walks and mutual recursion. Variant 1 has other seeds, depths and operator order."""
import sys

sys.setrecursionlimit(20000)


def nested(depth, seed, ops):
    if depth == 0:
        return str(seed % 10)
    op = ops[seed % 3]
    return '(%d%s%s)' % (seed % 7, op, nested(depth - 1, seed * 31 % 1009, ops))


class Parser:
    def __init__(self, text):
        self.text = text
        self.pos = 0

    def expr(self):
        if self.text[self.pos] == '(':
            self.pos += 1
            left = self.expr()
            op = self.text[self.pos]
            self.pos += 1
            right = self.expr()
            self.pos += 1
            return (op, left, right)
        start = self.pos
        while self.text[self.pos].isdigit():
            self.pos += 1
        return int(self.text[start:self.pos])


def evaluate(tree):
    if isinstance(tree, int):
        return tree
    op, a, b = tree
    a = evaluate(a)
    b = evaluate(b)
    if op == '+':
        return (a + b) % 1000003
    if op == '-':
        return (a - b) % 1000003
    return a * b % 1000003


def depth(tree):
    if isinstance(tree, int):
        return 0
    return 1 + max(depth(tree[1]), depth(tree[2]))


def is_even(k):
    return True if k == 0 else is_odd(k - 1)


def is_odd(k):
    return False if k == 0 else is_even(k - 1)


def flatten(xs, out):
    for x in xs:
        if isinstance(x, list):
            flatten(x, out)
        else:
            out.append(x)
    return out


def deep_list(d):
    xs = [d]
    for k in range(d):
        xs = [k, xs, -k]
    return xs


SHAPES = [(600, 200, 1, '+-*', 900, 700, 500, 50), (500, 300, 7, '*+-', 1100, 600, 450, 70)]


def main(n, variant=0):
    d0, dk, seed, ops, even, odd, l0, lk = SHAPES[variant]
    total = 0
    for i in range(n):
        tree = Parser(nested(d0 + i % dk, i * seed + 1, ops)).expr()
        total += evaluate(tree) + depth(tree)
        total += is_even(even + i % 3) + is_odd(odd + i % 5)
        total += sum(flatten(deep_list(l0 + i % lk), []))
    return total


N = 30

if __name__ == '__main__':
    print(main(N))
