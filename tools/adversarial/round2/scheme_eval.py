"""Scheme interpreter compiled to closures, with trampolined tail calls and escape-only call/cc by exceptions: an interpreter loop's megamorphic dispatch (Bolz et al., Tracing the Meta-Level, ICOOOLPS 2009)."""
import re

N = 22


class Sym(str):
    __slots__ = ()


SYMS = {}


def sym(s):
    r = SYMS.get(s)
    if r is None:
        r = SYMS[s] = Sym(s)
    return r


class Env:
    __slots__ = ('vars', 'outer')

    def __init__(self, names, values, outer, rest=None):
        self.vars = dict(zip(names, values))
        if rest is not None:
            self.vars[rest] = list(values[len(names):])
        self.outer = outer

    def find(self, name):
        e = self
        while e is not None:
            if name in e.vars:
                return e
            e = e.outer
        raise NameError('unbound ' + name)


class Lambda:
    __slots__ = ('params', 'rest', 'body', 'env', 'name')

    def __init__(self, params, rest, body, env, name):
        self.params, self.rest, self.body, self.env, self.name = params, rest, body, env, name


class TailCall:
    __slots__ = ('f', 'args')

    def __init__(self, f, args):
        self.f, self.args = f, args


class Escape(Exception):
    def __init__(self, k, value):
        self.k, self.value = k, value


class Continuation:
    def __call__(self, value=None):
        raise Escape(self, value)


def apply_proc(f, args):
    while True:
        if type(f) is Lambda:
            r = f.body(Env(f.params, args, f.env, f.rest))
            if type(r) is TailCall:
                f, args = r.f, r.args
                continue
            return r
        return f(*args)


def callcc(f):
    k = Continuation()
    try:
        return apply_proc(f, [k])
    except Escape as e:
        if e.k is k:
            return e.value
        raise


def tokenise(src):
    src = re.sub(r';[^\n]*', '', src)
    return re.findall(r'"(?:[^"\\]|\\.)*"|[()\']|[^\s()\']+', src)


def atom(t):
    if t[0] == '"':
        return t[1:-1]
    if t == '#t':
        return True
    if t == '#f':
        return False
    try:
        return int(t)
    except ValueError:
        pass
    try:
        return float(t)
    except ValueError:
        return sym(t)


def read_all(src):
    tokens = tokenise(src)
    pos = 0
    out = []

    def read():
        nonlocal pos
        t = tokens[pos]
        pos += 1
        if t == '(':
            lst = []
            while tokens[pos] != ')':
                lst.append(read())
            pos += 1
            return lst
        if t == "'":
            return [sym('quote'), read()]
        return atom(t)

    while pos < len(tokens):
        out.append(read())
    return out


def analyse(x, tail=False):
    if type(x) is Sym:
        name = x
        return lambda env: env.find(name).vars[name]
    if not isinstance(x, list):
        return lambda env: x
    if not x:
        return lambda env: []
    head = x[0]
    if head == 'quote':
        q = x[1]
        return lambda env: q
    if head == 'if':
        c = analyse(x[1])
        t = analyse(x[2], tail)
        e = analyse(x[3], tail) if len(x) > 3 else (lambda env: False)
        return lambda env: t(env) if c(env) is not False else e(env)
    if head == 'define':
        if isinstance(x[1], list):
            name = x[1][0]
            proc = analyse_lambda(x[1][1:], x[2:], name)
        else:
            name = x[1]
            proc = analyse(x[2])

        def define(env):
            env.vars[name] = proc(env)
        return define
    if head == 'set!':
        name = x[1]
        val = analyse(x[2])

        def setbang(env):
            env.find(name).vars[name] = val(env)
        return setbang
    if head == 'lambda':
        return analyse_lambda(x[1], x[2:], 'lambda')
    if head == 'begin':
        return analyse_body(x[1:], tail)
    if head == 'let':
        names = [b[0] for b in x[1]]
        inits = [analyse(b[1]) for b in x[1]]
        body = analyse_body(x[2:], tail)
        return lambda env: body(Env(names, [i(env) for i in inits], env))
    if head == 'cond':
        clauses = [(None if c[0] == 'else' else analyse(c[0]), analyse_body(c[1:], tail))
                   for c in x[1:]]

        def cond(env):
            for test, body in clauses:
                if test is None or test(env) is not False:
                    return body(env)
            return False
        return cond
    if head == 'and':
        parts = [analyse(p) for p in x[1:]]

        def and_(env):
            r = True
            for p in parts:
                r = p(env)
                if r is False:
                    return False
            return r
        return and_
    if head == 'or':
        parts = [analyse(p) for p in x[1:]]

        def or_(env):
            for p in parts:
                r = p(env)
                if r is not False:
                    return r
            return False
        return or_
    fn = analyse(head)
    args = [analyse(a) for a in x[1:]]
    if tail:
        return lambda env: TailCall(fn(env), [a(env) for a in args])
    return lambda env: apply_proc(fn(env), [a(env) for a in args])


def analyse_body(forms, tail):
    parts = [analyse(f) for f in forms[:-1]] + [analyse(forms[-1], tail)]
    if len(parts) == 1:
        return parts[0]

    def body(env):
        for p in parts[:-1]:
            p(env)
        return parts[-1](env)
    return body


def analyse_lambda(params, body, name):
    rest = None
    if type(params) is Sym:
        rest, params = params, []
    elif '.' in params:
        rest = params[-1]
        params = params[:-2]
    b = analyse_body(body, True)
    return lambda env: Lambda(params, rest, b, env, name)


def add(*a):
    r = a[0]
    for x in a[1:]:
        r = r + x
    return r


def sub(a, *b):
    if not b:
        return -a
    for x in b:
        a = a - x
    return a


def mul(*a):
    r = 1
    for x in a:
        r = r * x
    return r


def num_to_string(x):
    return repr(x) if isinstance(x, float) else str(x)


def global_env(out):
    g = Env([], [], None)
    g.vars.update({
        '+': add, '-': sub, '*': mul, '/': lambda a, b: a / b if a % b else a // b,
        'quotient': lambda a, b: a // b, 'remainder': lambda a, b: a % b,
        '<': lambda a, b: a < b, '>': lambda a, b: a > b, '=': lambda a, b: a == b,
        '<=': lambda a, b: a <= b, 'not': lambda a: a is False,
        'eq?': lambda a, b: a is b or a == b, 'null?': lambda a: a == [],
        'pair?': lambda a: isinstance(a, list) and a != [],
        'car': lambda a: a[0], 'cdr': lambda a: a[1:], 'cons': lambda a, b: [a] + b,
        'list': lambda *a: list(a), 'length': len,
        'append': lambda a, b: a + b, 'apply': lambda f, a: apply_proc(f, a),
        'string-append': lambda *a: ''.join(a), 'number->string': num_to_string,
        'string<?': lambda a, b: a < b, 'string-length': len,
        'display': lambda x: out.append(x), 'call/cc': callcc,
        'number?': lambda x: isinstance(x, (int, float)) and not isinstance(x, bool),
        'string?': lambda x: isinstance(x, str) and type(x) is not Sym,
    })
    return g


PRELUDE = '''
(define (map f l) (if (null? l) '() (cons (f (car l)) (map f (cdr l)))))
(define (filter p l) (cond ((null? l) '()) ((p (car l)) (cons (car l) (filter p (cdr l)))) (else (filter p (cdr l)))))
(define (foldl f acc l) (if (null? l) acc (foldl f (f acc (car l)) (cdr l))))
(define (iota n) (define (go i acc) (if (< i 0) acc (go (- i 1) (cons i acc)))) (go (- n 1) '()))
(define (insert x l less) (cond ((null? l) (list x)) ((less x (car l)) (cons x l)) (else (cons (car l) (insert x (cdr l) less)))))
(define (isort l less) (foldl (lambda (acc x) (insert x acc less)) '() l))
(define (find-first p l) (call/cc (lambda (k) (map (lambda (x) (if (p x) (k x) #f)) l) #f)))
(define (make-counter start step) (lambda () (set! start (+ start step)) start))
(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
(define (tak x y z) (if (not (< y x)) z (tak (tak (- x 1) y z) (tak (- y 1) z x) (tak (- z 1) x y))))
(define (count-to n acc) (if (= n 0) acc (count-to (- n 1) (+ acc n))))
(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))
(define (assoc-inc key alist) (cond ((null? alist) (list (list key 1))) ((eq? (car (car alist)) key) (cons (list key (+ 1 (car (cdr (car alist))))) (cdr alist))) (else (cons (car alist) (assoc-inc key (cdr alist))))))
(define (tally l) (foldl (lambda (acc x) (assoc-inc x acc)) '() l))
(define (variadic . xs) (length xs))
'''

PROGRAMS = [
    '(fib {fib})',
    '(tak {tak})',
    '(count-to {count} {zero})',
    '(foldl + {zero} (map (lambda (x) (* x x)) (iota {iota})))',
    '(isort (map (lambda (x) {key}) (iota {sort})) {less})',
    '(find-first (lambda (x) (> x {limit})) (map (lambda (x) {key}) (iota {iota})))',
    '(let ((c (make-counter {cstart} {step}))) (c) (c) (display (c)) (c))',
    '(fact {fact})',
    '(tally (map (lambda (x) (remainder x 7)) (iota {iota})))',
    '(variadic 1 "two" 3.0 (quote four))',
    '(string-append "r" (number->string (fib {small})) ":" (number->string {zero}))',
]

VARIANTS = [
    dict(fib='{i}', tak='{t} 8 4', count='{c}', zero='0', iota='{s}', sort='{q}',
         key='(remainder (* x 37) 101)', less='<', limit='90', cstart='0', step='3', fact='12',
         small='10'),
    dict(fib='{i}.0', tak='{t}.5 8.0 4', count='{c}', zero='0.5', iota='{s}', sort='{q}',
         key='(string-append "k" (number->string (remainder (* x 37) 101)))',
         less='string<?', limit='"k9"', cstart='"c"', step='"x"', fact='{f}', small='10.0'),
]


def run_once(i, variant):
    out = []
    g = global_env(out)
    for form in read_all(PRELUDE):
        analyse(form)(g)
    params = {k: v.format(i=12 + i % 4, t=10 + i % 3, c=1500 + 100 * (i % 10), s=60 + i % 20,
                          q=40 + i % 20, f=40 + 7 * (i % 8))
              for k, v in VARIANTS[variant].items()}
    results = []
    for p in PROGRAMS:
        src = p.format(**params)
        r = None
        for form in read_all(src):
            r = analyse(form)(g)
        results.append(r)
    return results, out


def main(n, variant=0):
    return [run_once(i, variant) for i in range(n)]
