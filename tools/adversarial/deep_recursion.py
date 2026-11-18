"""Deep recursion past the inlining and recursion limits: a recursive-descent parser on deeply nested input, tree walks and mutual recursion."""
import sys

sys.setrecursionlimit(20000)


def nested(depth, seed):
    if depth == 0:
        return str(seed % 10)
    op = '+-*'[seed % 3]
    return '(%d%s%s)' % (seed % 7, op, nested(depth - 1, seed * 31 % 1009))


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


def main(n):
    total = 0
    for i in range(n):
        tree = Parser(nested(600 + i % 200, i + 1)).expr()
        total += evaluate(tree) + depth(tree)
        total += is_even(900 + i % 3) + is_odd(700 + i % 5)
        total += sum(flatten(deep_list(500 + i % 50), []))
    return total


N = 30

if __name__ == '__main__':
    print(main(N))
