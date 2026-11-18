"""Shrinks a program on which Ni and CPython disagree, keeping the kind of
mismatch (run.py:compare) the same at every step:

    python3 tools/fuzz/minimise.py PROGRAM [-o OUT] [--timeout 30] [--same-line]

Passes, repeated until none changes anything: delete runs of statements
(and list or tuple items: CALLS entries, arguments) by halving chunk sizes;
replace a compound statement by its body; replace an expression by one of
its subexpressions or by 0; lower ROUNDS. --same-line also keeps the first
differing line pair the same (after the call's round, index and name).
Works on the AST, so it needs Python 3.9+ (ast.unparse); the programs it
writes are run under 3.7.
"""
import argparse, ast, concurrent.futures, copy, os, sys, warnings

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run  # noqa: E402


def diff_line(plain, ni):
    for a, b in zip(plain[1].splitlines(), ni[1].splitlines()):
        if a != b:
            return a.split(' ', 3)[-1], b.split(' ', 3)[-1]
    return None


class Shrinker:
    def __init__(self, path, out, timeout, same_line):
        self.out, self.timeout, self.same_line = out, timeout, same_line
        self.tmp = out + '.try.py'
        self.tests = 0
        self.pool = concurrent.futures.ThreadPoolExecutor(2)
        src = open(path).read()
        self.tree = ast.parse(src)
        # the driver (module-level loops and prints) stays as it is, so the
        # result still prints each call's result or exception
        self.frozen = {id(n) for top in self.tree.body if isinstance(top, (ast.For, ast.Expr))
                       for n in ast.walk(top)}
        self.kind, self.line = self.judge(src)
        if self.kind is None:
            sys.exit('no mismatch on %s' % path)
        print('mismatch: %s %s' % (self.kind, self.line or ''))
        self.best = src

    def judge(self, src):
        with open(self.tmp, 'w') as f:
            f.write(src)
        self.tests += 1
        p = self.pool.submit(run.run_one, self.tmp, 'plain', self.timeout)
        n = self.pool.submit(run.run_one, self.tmp, 'ni', self.timeout)
        p, n = p.result(), n.result()
        if p[0] != 0:
            return None, None
        return run.compare(p, n), diff_line(p, n)

    def ok(self):
        """does the tree, as it is now, still show the mismatch?"""
        try:
            src = ast.unparse(self.tree) + '\n'
            with warnings.catch_warnings():
                warnings.simplefilter('ignore')
                compile(src, '<min>', 'exec')
        except Exception:
            return False
        if src == self.best:
            return False
        kind, line = self.judge(src)
        if kind != self.kind or (self.same_line and line != self.line):
            return False
        self.best = src
        with open(self.out, 'w') as f:
            f.write(src)
        return True

    def lists(self):
        for node in ast.walk(self.tree):
            if id(node) in self.frozen:
                continue
            for field in ('body', 'orelse', 'finalbody', 'handlers', 'elts', 'args', 'keys'):
                l = getattr(node, field, None)
                if isinstance(l, list) and l and not isinstance(node, ast.arguments):
                    if field == 'keys' and isinstance(node, ast.Dict):
                        continue
                    yield node, field

    def drop_items(self):
        changed = False
        for node, field in list(self.lists()):
            l = getattr(node, field)
            chunk = max(1, len(l) // 2)
            while chunk >= 1:
                i = 0
                while i < len(l):
                    saved = l[i:i + chunk]
                    del l[i:i + chunk]
                    padded = field == 'body' and not l
                    if padded:
                        l.append(ast.Pass())
                    if self.ok():
                        changed = True
                        if padded:
                            break
                    else:
                        if padded:
                            l.pop()
                        l[i:i] = saved
                        i += chunk
                chunk //= 2
        return changed

    def flatten(self):
        changed = False
        for node, field in list(self.lists()):
            l = getattr(node, field)
            i = 0
            while i < len(l):
                s = l[i]
                inner = None
                if isinstance(s, (ast.If, ast.For, ast.While, ast.With)):
                    inner = s.body
                elif isinstance(s, ast.Try):
                    inner = s.body + s.finalbody
                elif isinstance(s, ast.ExceptHandler):
                    inner = None
                if inner and field != 'handlers':
                    l[i:i + 1] = inner
                    if self.ok():
                        changed = True
                        continue
                    l[i:i + len(inner)] = [s]
                i += 1
        return changed

    def simplify(self):
        changed = False
        for node in list(ast.walk(self.tree)):
            if id(node) in self.frozen:
                continue
            for field, value in list(ast.iter_fields(node)):
                if not isinstance(value, ast.expr) or isinstance(getattr(value, 'ctx', None), (ast.Store, ast.Del)):
                    continue
                if isinstance(value, ast.Constant):
                    continue
                cands = [c for c in ast.iter_child_nodes(value) if isinstance(c, ast.expr)
                         and not isinstance(getattr(c, 'ctx', None), (ast.Store, ast.Del))]
                cands.append(ast.Constant(0))
                for c in cands:
                    setattr(node, field, copy.deepcopy(c))
                    if self.ok():
                        changed = True
                        break
                    setattr(node, field, value)
        return changed

    def rounds(self):
        for node in self.tree.body:
            if (isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name)
                    and node.targets[0].id == 'ROUNDS' and isinstance(node.value, ast.Constant)):
                old = node.value.value
                for n in (1, 2, 3):
                    if n < old:
                        node.value.value = n
                        if self.ok():
                            return True
                        node.value.value = old
        return False

    def run(self):
        while True:
            changed = self.rounds()
            changed |= self.drop_items()
            changed |= self.flatten()
            changed |= self.simplify()
            print('  %d lines after %d tests' % (self.best.count('\n'), self.tests), flush=True)
            if not changed:
                break
        with open(self.out, 'w') as f:
            f.write(self.best)
        os.remove(self.tmp)
        self.pool.shutdown()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('program')
    ap.add_argument('-o', '--out')
    ap.add_argument('--timeout', type=int, default=30)
    ap.add_argument('--same-line', action='store_true')
    a = ap.parse_args()
    out = a.out or a.program[:-3] + '.min.py'
    Shrinker(a.program, out, a.timeout, a.same_line).run()
    print('wrote', out)


if __name__ == '__main__':
    main()
