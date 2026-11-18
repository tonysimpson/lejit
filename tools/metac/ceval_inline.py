"""ceval.c's static helpers inlined into the opcode cases (DECISIONS D82,
doc/METACOMPILER.md "`ceval.c` reader").

A case that calls a static function of ceval.c ('res = cmp_outcome(oparg,
left, right);') gets the function's body in place of the call, rewritten
into the statements gen_ceval.py's back half reads:
  - parameters are the call's arguments; a parameter or local assigned
    again on a path gets a new name from there (each is assigned once),
    and every local is renamed after the helper (cmp_outcome_res);
  - a 'switch' on a value is an if-chain on it (the generated compiler
    tests oparg where ceval.c switched on it), each arm followed by the
    statements after the switch when it ends in 'break', so every arm is
    one path to a 'return';
  - 'return E;' is 'X = E;' followed by the rest of the case after the
    call; 'return NULL;' (the error value of a helper returning an
    object) is the case's own error exit, ERROR_IF(true, L), when the case
    tests 'X == NULL' with only releases of its inputs before;
  - an if whose arm returns takes the statements after it as its else;
  - an object-like macro of ceval.c whose body is a string is its string.
Where the helper's policy entry says type tests use only the types Ni knows
('promote', 'known_only'), an arm of the switch that tests the type of a
parameter is marked resumable: NI_RESUME(copy, result, args...) { arm },
and its run-time copy (copy_text) is written for the back half to call
where Ni knows too little.

Anything else is refused (Refused), and the case is not generated.
"""
import os
import re

import ceval_reader
import ctokens

# words that start a declaration of a helper's local
DECL_WORDS = {'int', 'long', 'Py_ssize_t', 'size_t', 'PyObject', 'PyTypeObject',
              'const', 'unsigned', 'char'}
# a call of one of these is a type test (Include's Py*_Check macros and
# PyExceptionClass_Check)
TYPE_TEST = re.compile(r'Py\w*_Check(Exact)?$|PyExceptionClass_Check$')
# what ERROR_IF's label is written as for a helper's error return
RESUME = 'NI_RESUME'


class Refused(Exception):
    pass


class Helper:
    def __init__(self, name, ret, params, body):
        self.name = name
        self.ret = ret            # return type text: 'PyObject *'
        self.params = params      # [(ctype, name)]
        self.body = body          # tokens inside the braces


def helpers(src):
    """({name: Helper} for the static functions of Python/ceval.c,
    {macro: string text} for its object-like macros that are strings)."""
    path = os.path.join(src, 'Python', 'ceval.c')
    with open(path, encoding='latin-1') as f:
        toks, macros = ctokens.tokenize(f.read(), 'ceval.c')
    out = {}
    start, i = 0, 0
    while i < len(toks):
        t = toks[i]
        if t.text in (';', '}'):
            start = i + 1
        elif t.text == '{':
            i = ctokens.match_close(toks, i) + 1
            start = i
            continue
        elif t.kind == 'id' and i + 1 < len(toks) and toks[i + 1].text == '(':
            close = ctokens.match_close(toks, i + 1)
            if close + 1 < len(toks) and toks[close + 1].text == '{':
                end = ctokens.match_close(toks, close + 1)
                head = [u.text for u in toks[start:i]]
                if 'static' in head:
                    ret = ' '.join(w for w in head if w not in ('static', 'inline'))
                    params = []
                    for p in ctokens.split_top(toks[i + 2:close], ','):
                        w = [u.text for u in p]
                        if w == ['void']:
                            continue
                        params.append((' '.join(w[:-1]), w[-1]))
                    out[t.text] = Helper(t.text, ret.replace(' *', ' *').strip(),
                                         params, toks[close + 2:end])
                i = end + 1
                start = i
                continue
            i = close + 1
            continue
        i += 1
    strings = {n: ' '.join(u.text for u in m.body) for n, m in macros.items()
               if m.params is None and m.body and all(u.kind == 'str' for u in m.body)}
    return out, strings


def untstate(toks, public):
    """'_PyX(tstate, a...)' is 'PyX(a...)' where PyX is public: from 3.8
    ceval.c hands the running thread's state to the internal form of a
    function whose public form finds it itself (PyErr_SetString calls
    _PyErr_SetString(_PyThreadState_GET(), ...))."""
    out, k = [], 0
    while k < len(toks):
        t = toks[k]
        if t.kind == 'id' and t.text.startswith('_Py') and t.text[1:] in public \
                and k + 3 < len(toks) and toks[k + 1].text == '(' \
                and toks[k + 2].text == 'tstate' and toks[k + 3].text == ',':
            out += [t._replace(text=t.text[1:]), toks[k + 1]]
            k += 4
            continue
        out.append(t)
        k += 1
    return out


def _texts(toks):
    return [t.text for t in toks]


def _is_release(w, inputs):
    return len(w) == 4 and w[0] in ('Py_DECREF', 'Py_XDECREF') and w[1] == '(' \
        and w[2] in inputs and w[3] == ')'


class _St:
    """A path's names: helper name -> its text now, and the names that
    have a value."""

    def __init__(self, env, valued, scope):
        self.env, self.valued, self.scope = env, valued, scope

    def copy(self, scope=None):
        return _St(dict(self.env), set(self.valued),
                   set(self.scope) if scope is None else scope)


def _block_text(stmts):
    return '{ ' + ' '.join(s if s.endswith('}') else s + ' ;' for s in stmts) + ' }'


class Inliner:
    """One call 'dest = helper(args);' and the statements after it."""

    def __init__(self, helper, args, dest, rest, inputs, strings, known_only,
                 public=()):
        if len(args) != len(helper.params):
            raise Refused('%s takes %d arguments' % (helper.name, len(helper.params)))
        if not helper.ret.endswith('*'):
            raise Refused('%s returns %s, not an object' % (helper.name, helper.ret))
        self.h = helper
        self.body = untstate(helper.body, public)
        self.dest = dest
        self.rest = [' '.join(_texts(s)) for s in rest]
        self.inputs = inputs
        self.strings = strings
        self.known_only = known_only
        self.types = {}            # helper name -> declared type
        self.used = {}             # new name -> count
        self.copies = []           # (name, C text) of run-time copies
        self.label = None          # the case's error label
        env = {}
        for (ctype, p), a in zip(helper.params, args):
            env[p] = a if re.fullmatch(r'\w+', a) else '( %s )' % a
            self.types[p] = ctype
        self.params = {p for _, p in helper.params}
        self.st0 = _St(env, set(env), set(env))
        # the error exit of the case: ERROR_IF(dest == NULL, L) after only
        # releases of inputs
        for s in rest:
            w = _texts(s)
            if _is_release(w, inputs):
                continue
            m = re.fullmatch(r'ERROR_IF \( %s == NULL , (\w+) \)' % re.escape(dest),
                             ' '.join(w))
            if m:
                self.label = m.group(1)
            break

    def run(self):
        out, end = self.flatten(ceval_reader.parse(self.body), self.st0, 'path')
        if end != 'return' and end != 'error':
            raise Refused('%s can end without a return' % self.h.name)
        return [ctokens.tokenize(s)[0] for s in out]

    # ---- names ----

    def fresh(self, name):
        base = '%s_%s' % (self.h.name, name)
        k = self.used.get(base, 0)
        self.used[base] = k + 1
        return base if k == 0 else '%s%d' % (base, k)

    def sub(self, toks, st):
        out = []
        for k, t in enumerate(toks):
            prev = toks[k - 1].text if k else ''
            if t.kind == 'id' and prev not in ('.', '->'):
                if t.text in st.env:
                    out.append(st.env[t.text])
                    continue
                if t.text in self.strings:
                    out.append(self.strings[t.text])
                    continue
            out.append(t.text)
        return ' '.join(out)

    # ---- statements ----

    def flatten(self, nodes, st, mode):
        """(statement texts, end) for the nodes on one path: end None (the
        path goes on after them), 'return' or 'error'. mode: 'path' (the
        rest of a path), 'branch' (an if's arm that may join again) or
        'loop' (a loop's body)."""
        out = []
        nodes = list(nodes)
        while nodes:
            n = nodes.pop(0)
            if n.kind == 'block':
                nodes[:0] = n.nodes
                continue
            if n.kind == 'switch':
                if mode == 'loop':
                    raise Refused('a switch in a loop')
                return out + [self.switch(n, nodes, st, mode)], self.ends
            if n.kind == 'if':
                stmts, end, ended = self.if_(n, nodes, st, mode)
                out += stmts
                if ended:
                    return out, end
                continue
            if n.kind in ('for', 'while'):
                body, end = self.flatten(ceval_reader._block(n.body), st.copy(scope=set()),
                                         'loop')
                if end == 'return':
                    raise Refused('a return in a loop')
                head = self.sub(n.toks[:ctokens.match_close(n.toks, 1) + 1], st)
                out.append('%s %s' % (head, _block_text(body)))
                continue
            if n.kind != 'stmt':
                raise Refused('%s in %s' % (n.kind, self.h.name))
            w = _texts(n.w)
            if not w:
                continue
            if w[0] == 'return':
                stmts, end = self.ret(n.w[1:], st, mode)
                return out + stmts, end
            if w == ['break']:
                raise Refused('a break outside a switch arm\'s end')
            if w[0] in DECL_WORDS and len(w) > 1 and w[1] not in ('=', '('):
                out += self.decl(n.w, st)
                continue
            if len(w) > 2 and w[1] == '=' and n.w[0].kind == 'id' and w[0] in st.env \
                    and (w[0] in self.params or w[0] in self.types):
                out.append(self.assign(w[0], n.w[2:], st, mode))
                continue
            out.append(self.sub(n.w, st))
        return out, None

    def decl(self, w, st):
        base, k = [], 0
        while k < len(w) and w[k].text in DECL_WORDS:
            base.append(w[k].text)
            k += 1
        out = []
        for d in ctokens.split_top(w[k:], ','):
            stars = 0
            while d and d[0].text == '*':
                stars += 1
                d = d[1:]
            if not d or d[0].kind != 'id' or (len(d) > 1 and d[1].text != '='):
                raise Refused('a declaration %s' % ' '.join(_texts(w)))
            name = d[0].text
            ctype = ' '.join(base) + (' ' + '*' * stars if stars else '')
            new = self.fresh(name)
            init = self.sub(d[2:], st) if len(d) > 2 else None
            st.env[name] = new
            self.types[name] = ctype
            st.scope.add(name)
            if init is not None:
                st.valued.add(name)
                out.append('%s %s = %s' % (ctype, new, init))
            else:
                st.valued.discard(name)
                out.append('%s %s' % (ctype, new))
        return out

    def assign(self, name, rhs, st, mode):
        """'name = E': a new name for it where it already has a value."""
        e = self.sub(rhs, st)
        if name not in st.scope:
            raise Refused('%s assigns %s, declared outside the branch' % (self.h.name, name))
        if name in st.valued and mode != 'loop':
            new = self.fresh(name)
            st.env[name] = new
            return '%s %s = %s' % (self.types[name], new, e)
        if name in st.valued:
            raise Refused('%s assigns %s again in a loop' % (self.h.name, name))
        st.valued.add(name)
        return '%s = %s' % (st.env[name], e)

    def ret(self, e, st, mode):
        text = self.sub(e, st)
        if text == 'NULL':
            if self.label is None:
                raise Refused('%s returns NULL but the case does not test %s == NULL'
                              % (self.h.name, self.dest))
            return ['ERROR_IF ( true , %s )' % self.label], 'error'
        if mode == 'loop':
            raise Refused('a return of a value in a loop')
        return ['%s = %s' % (self.dest, text)] + self.rest, 'return'

    def if_(self, n, nodes, st, mode):
        cond = self.sub(n.cond, st)
        then, t_end = self.flatten(ceval_reader._block(n.then),
                                   st.copy(scope=set()), 'branch' if mode != 'loop' else 'loop')
        other, o_end = ([], None)
        if n.other is not None:
            other, o_end = self.flatten(ceval_reader._block(n.other),
                                        st.copy(scope=set()),
                                        'branch' if mode != 'loop' else 'loop')
        if 'return' in (t_end, o_end):
            # the statements after the if go on the arm that doesn't end
            if mode == 'loop':
                raise Refused('a return of a value in a loop')
            rest, r_end = self.flatten(nodes, st, mode)
            del nodes[:]
            if t_end is None:
                then = then + rest
                t_end = r_end
            if o_end is None:
                other = other + rest
                o_end = r_end
            ends = {t_end, o_end}
            end = 'return' if 'return' in ends else 'error' if ends == {'error'} else None
            return ['if ( %s ) %s else %s' % (cond, _block_text(then),
                                              _block_text(other))], end, True
        if t_end == 'error' and o_end == 'error':
            return ['if ( %s ) %s else %s' % (cond, _block_text(then),
                                              _block_text(other))], 'error', True
        if t_end == 'error' and not other and then == ['ERROR_IF ( true , %s )' % self.label]:
            stmt = 'ERROR_IF ( %s , %s )' % (cond, self.label)
        elif other:
            stmt = 'if ( %s ) %s else %s' % (cond, _block_text(then), _block_text(other))
        else:
            stmt = 'if ( %s ) %s' % (cond, _block_text(then))
        return [stmt], None, False

    def switch(self, n, after, st, mode):
        """An if-chain on the switch's value, each arm with the statements
        after the switch when it ends in break."""
        value = self.sub(n.cond, st)
        if not re.fullmatch(r'\w+', value):
            value = '( %s )' % value
        body = n.body
        if body.kind != 'block':
            raise Refused('a switch without a block')
        arms = self.arms(body.toks[1:-1])
        tail = list(after)
        del after[:]
        chain, ends = [], set()
        default = None
        for labels, nodes, breaks in arms:
            arm = list(nodes) + (tail if breaks else [])
            region = self.known_only and breaks and self.tests_types(nodes)
            if region:
                text, end = self.resumable(labels, nodes, tail, st)
            else:
                stmts, end = self.flatten(arm, st.copy(), 'path')
                text = _block_text(stmts)
            ends.add(end)
            if 'default' in labels:
                default = text
            else:
                chain.append((' || '.join('%s == %s' % (value, x) for x in labels), text))
        if default is None:
            stmts, end = self.flatten(tail, st.copy(), 'path')
            default = _block_text(stmts)
            ends.add(end)
        self.ends = 'return' if 'return' in ends else 'error' if ends == {'error'} else None
        if not chain:
            return default
        text = ' else '.join('if ( %s ) %s' % c for c in chain)
        return '%s else %s' % (text, default)

    def arms(self, toks):
        """[(labels, nodes, ends_in_break)] of a switch's block."""
        out, labels, cur, depth, k = [], [], [], 0, 0
        segs = []
        while k < len(toks):
            t = toks[k]
            if t.text in '([{':
                depth += 1
            elif t.text in ')]}':
                depth -= 1
            if depth == 0 and t.text in ('case', 'default'):
                segs.append((labels, cur))
                j = k + 1
                while toks[j].text != ':':
                    j += 1
                label = 'default' if t.text == 'default' else ' '.join(_texts(toks[k + 1:j]))
                labels, cur = [label], []
                k = j + 1
                continue
            cur.append(t)
            k += 1
        segs.append((labels, cur))
        if segs[0][1]:
            raise Refused('statements before the first case')
        pending = []
        for labels, cur in segs[1:]:
            if not cur:
                pending += labels        # case A: case B: ...
                continue
            nodes = ceval_reader.parse(cur)
            breaks = bool(nodes) and nodes[-1].kind == 'stmt' and _texts(nodes[-1].w) == ['break']
            if breaks:
                nodes = nodes[:-1]
            out.append((pending + labels, nodes, breaks))
            pending = []
        if pending:
            raise Refused('a case with no statements at the end of the switch')
        return out

    # ---- resumable arms ----

    def tests_types(self, nodes):
        """Does the arm test the type of a parameter (Py*_Check)?"""
        for n in ceval_reader.flat(nodes):
            w = n.toks
            for k in range(len(w) - 2):
                if w[k].kind == 'id' and TYPE_TEST.match(w[k].text) and w[k + 1].text == '(' \
                        and any(u.text in self.params for u in
                                w[k + 2:ctokens.match_close(w, k + 1)]):
                    return True
        return False

    def resumable(self, labels, nodes, tail, st):
        """NI_RESUME(copy, result, args) { arm } then the tail: the arm as
        the back half translates it, and its run-time copy, which gives the
        one local the tail reads (as a truth value) or -1 for an error."""
        st = st.copy()
        stmts, end = self.flatten(nodes, st, 'path')
        if end is not None:
            raise Refused('a resumable arm that does not end in break')
        outs = [n for n in self.types if n not in self.params and n in st.env
                and any(t.text == n for t in self._tail_toks(tail))]
        if len(outs) != 1:
            raise Refused('a resumable arm leaving %d values to the tail' % len(outs))
        res = outs[0]
        tw = self._tail_toks(tail)
        for k, t in enumerate(tw):
            if t.text == res and not ((k + 1 < len(tw) and tw[k + 1].text == '?')
                                      or (k and tw[k - 1].text == '!')):
                raise Refused('the tail reads %s other than as a truth value' % res)
        # the copy: the parameters the arm reads, the helper's locals it
        # reads with their initial values, the arm with 'return NULL' -1
        args = [p for _, p in self.h.params
                if any(t.text == p for n in nodes for t in n.toks)]
        if any(t.text == 'tstate' for n in nodes for t in n.toks):
            raise Refused('a resumable arm reading the thread state')
        name = 'cimpl_%s_%s' % (self.h.name, labels[0])
        if not re.fullmatch(r'\w+', name):
            raise Refused('a case label %s' % labels[0])
        ptypes = []
        for p in args:
            if self.types[p].replace(' ', '') != 'PyObject*':
                raise Refused('a resumable arm reading %s %s' % (self.types[p], p))
            ptypes.append('PyObject *%s' % p)
        body = []
        for n in ceval_reader.parse(self.body):
            w = _texts(n.toks)
            if n.kind == 'stmt' and w[:1] and w[0] in DECL_WORDS and res in w:
                body.append(' '.join(w))
        for n in nodes:
            body.append(self._copy_text(n.toks))
        body.append('return %s ? 1 : 0;' % res)
        self.copies.append((name, 'static int %s(%s) {\n  %s\n}' % (
            name, ', '.join(ptypes), '\n  '.join(body))))
        out = 'if ( NI_RESUME ( %s , %s%s ) ) %s' % (
            name, st.env[res], ''.join(' , ' + st.env[p] for p in args), _block_text(stmts))
        rest, end = self.flatten(tail, st, 'path')
        return _block_text([out] + rest), end

    def _tail_toks(self, tail):
        return [t for n in tail for t in n.toks]

    def _copy_text(self, toks):
        w = []
        for k, t in enumerate(toks):
            if t.text == 'NULL' and k and toks[k - 1].text == 'return':
                w.append('-1')
            elif t.text == 'return' and not (k + 1 < len(toks) and toks[k + 1].text == 'NULL'):
                raise Refused('a resumable arm returning a value')
            elif t.kind == 'id' and t.text in self.strings:
                w.append(self.strings[t.text])
            else:
                w.append(t.text)
        return ' '.join(w)
