"""Pratt-parser calculator REPL whose errors are raised many frames deep and reported from their tracebacks (function names, line offsets, chained causes): exceptions crossing optimised frames that must be rebuilt (Hoelzle, Chambers and Ungar, Debugging Optimized Code with Dynamic Deoptimization, PLDI 1992; Antonov, Optimization Killers, V8 Crankshaft)."""
import re
import zlib

N = 250

TOKEN = re.compile(r'\s*(?:(\d+\.\d*|\d+j|\d+)|([A-Za-z_]\w*)|(\*\*|==|<=|>=|[-+*/%()<>,=?:]))')


class CalcError(Exception):
    pass


class ParseError(CalcError):
    pass


class EvalError(CalcError):
    pass


def tokenise(src):
    pos = 0
    out = []
    src = src.rstrip()
    while pos < len(src):
        m = TOKEN.match(src, pos)
        if not m or m.end() == pos:
            raise ParseError('bad character %r at %d' % (src[pos], pos))
        num, name, op = m.groups()
        if num:
            out.append(('num', complex(num) if num.endswith('j') else
                        float(num) if '.' in num else int(num)))
        elif name:
            out.append(('name', name))
        else:
            out.append(('op', op))
        pos = m.end()
    out.append(('end', None))
    return out


class Parser:
    BINARY = {'==': 1, '<': 1, '>': 1, '<=': 1, '>=': 1, '+': 2, '-': 2, '*': 3, '/': 3,
              '%': 3, '**': 5}

    def __init__(self, tokens):
        self.tokens = tokens
        self.i = 0

    def peek(self):
        return self.tokens[self.i]

    def advance(self):
        t = self.tokens[self.i]
        self.i += 1
        return t

    def expect(self, op):
        t = self.advance()
        if t != ('op', op):
            raise ParseError('expected %r, got %r' % (op, t[1]))

    def expression(self, rbp=0):
        left = self.nud(self.advance())
        while True:
            kind, val = self.peek()
            if kind == 'op' and val == '?' and rbp < 1:
                self.advance()
                a = self.expression()
                self.expect(':')
                b = self.expression(0)
                left = ('if', left, a, b)
                continue
            if kind != 'op' or val not in self.BINARY or self.BINARY[val] <= rbp:
                return left
            self.advance()
            bp = self.BINARY[val] - (1 if val == '**' else 0)
            left = ('bin', val, left, self.expression(bp))

    def nud(self, tok):
        kind, val = tok
        if kind == 'num':
            return ('num', val)
        if kind == 'name':
            if self.peek() == ('op', '('):
                self.advance()
                args = []
                if self.peek() != ('op', ')'):
                    args.append(self.expression())
                    while self.peek() == ('op', ','):
                        self.advance()
                        args.append(self.expression())
                self.expect(')')
                return ('call', val, args)
            return ('var', val)
        if tok == ('op', '-'):
            return ('neg', self.expression(4))
        if tok == ('op', '('):
            e = self.expression()
            self.expect(')')
            return e
        raise ParseError('unexpected %r' % (val,))


def parse_statement(src):
    toks = tokenise(src)
    if len(toks) > 3 and toks[0] == ('name', 'def'):
        p = Parser(toks[2:])
        p.expect('(')
        params = []
        while p.peek() != ('op', ')'):
            params.append(p.advance()[1])
            if p.peek() == ('op', ','):
                p.advance()
        p.expect(')')
        p.expect('=')
        body = p.expression()
        return ('def', toks[1][1], params, body)
    p = Parser(toks)
    e = p.expression()
    if p.peek()[0] != 'end':
        raise ParseError('trailing %r' % (p.peek()[1],))
    return ('expr', e)


OPS = {'+': lambda a, b: a + b, '-': lambda a, b: a - b, '*': lambda a, b: a * b,
       '/': lambda a, b: a / b, '%': lambda a, b: a % b, '**': lambda a, b: a ** b,
       '==': lambda a, b: a == b, '<': lambda a, b: a < b, '>': lambda a, b: a > b,
       '<=': lambda a, b: a <= b, '>=': lambda a, b: a >= b}


def evaluate(node, env, funcs, depth=0):
    kind = node[0]
    if kind == 'num':
        return node[1]
    if kind == 'var':
        try:
            return env[node[1]]
        except KeyError:
            raise EvalError('undefined %s' % node[1]) from None
    if kind == 'neg':
        return -evaluate(node[1], env, funcs, depth)
    if kind == 'if':
        c = evaluate(node[1], env, funcs, depth)
        return evaluate(node[2] if c else node[3], env, funcs, depth)
    if kind == 'bin':
        a = evaluate(node[2], env, funcs, depth)
        b = evaluate(node[3], env, funcs, depth)
        try:
            return OPS[node[1]](a, b)
        except (ZeroDivisionError, TypeError, OverflowError) as e:
            raise EvalError('%s in %s' % (type(e).__name__, node[1])) from e
    if kind == 'call':
        return call_function(node[1], [evaluate(a, env, funcs, depth) for a in node[2]],
                             funcs, depth)
    raise EvalError('bad node %r' % (kind,))


def call_function(name, args, funcs, depth):
    if depth > 60:
        raise EvalError('recursion too deep in %s' % name)
    if name not in funcs:
        raise EvalError('no function %s' % name)
    params, body = funcs[name]
    if len(params) != len(args):
        raise EvalError('%s takes %d arguments' % (name, len(params)))
    return evaluate(body, dict(zip(params, args)), funcs, depth + 1)


def describe(exc):
    frames = []
    tb = exc.__traceback__
    while tb is not None:
        code = tb.tb_frame.f_code
        frames.append('%s+%d' % (code.co_name, tb.tb_lineno - code.co_firstlineno))
        tb = tb.tb_next
    chain = []
    e = exc
    while e is not None:
        chain.append(type(e).__name__)
        e = e.__cause__ or (None if e.__suppress_context__ else e.__context__)
    return '%s: %s | %d frames, last %s | chain %s' % (
        type(exc).__name__, exc, len(frames), ','.join(frames[-3:]), '<-'.join(chain))


def repl(lines):
    env = {'pi': 3, 'e': 2}
    funcs = {}
    out = []
    for line in lines:
        try:
            stmt = parse_statement(line)
            if stmt[0] == 'def':
                funcs[stmt[1]] = (stmt[2], stmt[3])
                out.append('defined %s/%d' % (stmt[1], len(stmt[2])))
            else:
                r = evaluate(stmt[1], env, funcs)
                env['_'] = r
                out.append(repr(r))
        except CalcError as e:
            out.append(describe(e))
    return out


DEFS = [
    'def sq(x) = x * x',
    'def fib(n) = n < 2 ? n : fib(n - 1) + fib(n - 2)',
    'def fact(n) = n <= 1 ? 1 : n * fact(n - 1)',
    'def hyp(a, b) = sq(a) + sq(b)',
    'def inv(x) = 1 / x',
    'def down(n) = n == 0 ? inv(n) : down(n - 1)',
]


def script(i, variant):
    lines = list(DEFS)
    for k in range(40):
        v = (i * 40 + k) % 97
        if variant == 0:
            lines.append(['hyp(%d, %d)' % (v, v + 1), 'fib(%d)' % (v % 12),
                          '(%d + 3) * %d %% 7' % (v, v), 'fact(%d)' % (v % 15)][k % 4])
            if k % 13 == 12:
                lines.append('inv(0)')
        else:
            lines.append(['hyp(%d.5, %dj)' % (v, v), 'down(%d)' % (v % 20),
                          'fib(%d.0)' % (v % 10), 'fact(%d)' % (v % 70),
                          'sq(%d' % v, 'undefined_%d + 1' % v, '2 ** %d ** 3' % (v % 6),
                          'down(%d)' % (v + 40), '%d $ 2' % v, 'hyp(1)',
                          '(%d.0 ** 400)' % (v + 2)][k % 11])
    return lines


def digest(results, keep):
    crc = 0
    for r in results:
        crc = zlib.crc32(repr(r).encode(), crc)
    return len(results), crc, results[:keep], results[-keep:]


def main(n, variant=0):
    return digest([repl(script(i, variant)) for i in range(n)], 2)
