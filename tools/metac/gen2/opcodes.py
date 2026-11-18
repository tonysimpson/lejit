"""The opcode cases (doc/GENERATOR_DESIGN.md "The opcode entry point"):
each case of _PyEval_EvalFrameDefault's switch, walked as a function of
its own (CaseWalk), becomes a case of pycompiler.c's opcode switch
(pycompiler_gen.h).

The case's stack is Ni's: the C's pops, peeks and pushes move nothing
while the case is walked. Each input stays where it is (NTOP of its depth
at entry) until the case succeeds; the epilogue then pops what the C
touched, releases what it decref'd and pushes what it left. An error is
'break' with the inputs still on the stack, as the C's error leaves them
for the unwinding (both release each once); an input the C moved
elsewhere before an error would be released twice, so that is refused.

The frame's names are read through Ni's macros: oparg is the meta's, an
item of co_consts or co_names is Ni's constant of it (made where it is
used, like an immortal object), fastlocals[i] is GETLOCAL(i). The frame
itself, the thread state and the instruction pointer beyond the jumps are
the interpreter's: a case reading them is refused ('core').
"""
import json
import subprocess

from pycparser import c_ast

import cpython_src
import cslots
import front
from . import conventions, policy, scope, types
from .abstract import ROOT, Translator
from .bta import locals_bt
from .cbuild import Code
from .ctyping import INT, strip_casts, text
from .errors import Refuse, ResumeHere
from .liveness import Liveness, body
from .values import Val, Var
from .walk import Walk, once_do, singleton

CEVAL = scope.CEVAL
# the oparg values the stack effects are compared at (dis.stack_effect)
OPARGS = (0, 1, 2, 3, 4, 5, 7, 8, 255, 256, 257, 258, 513)
# names of the frame a case may not read: the interpreter's own state
CORE = {'f', 'tstate', 'why', 'retval', 'throwflag', 'exc_info', 'first_instr',
        'instr_ub', 'instr_lb', 'instr_prev', 'co_opcache', 'opcache'}
CODE_TUPLES = {'consts': 'GETCONST', 'names': 'GETNAMEV'}


class Stack:
    """The C's stack on one path, relative to the case's entry: sp the
    offset of its pointer, low the lowest it reached, slots the values
    written (offset -> Pushed), uses the references the C consumed of each
    input (depth -> [how]), names the variable each input was read into."""

    def __init__(self):
        self.sp, self.low = 0, 0
        self.slots, self.uses, self.names = {}, {}, {}

    def copy(self):
        s = Stack()
        s.sp, s.low = self.sp, self.low
        s.slots = dict(self.slots)
        s.uses = {d: list(u) for d, u in self.uses.items()}
        s.names = dict(self.names)
        return s

    def __eq__(self, other):
        return (self.sp, self.low, self.slots, self.uses) == \
            (other.sp, other.low, other.slots, other.uses)


class Pushed:
    """A value written to a slot: its C, and whether the slot owns a
    reference to it (else it is the input of that depth, moved)."""

    def __init__(self, c, owned, depth=None, var=None):
        self.c, self.owned, self.depth, self.var = c, owned, depth, var

    def __eq__(self, other):
        return (self.c, self.owned, self.depth) == (other.c, other.owned, other.depth)


def is_sp(e):
    return isinstance(e, c_ast.ID) and e.name == 'stack_pointer'


def const_int(e):
    e = strip_casts(e)
    if isinstance(e, c_ast.Constant) and e.type == 'int':
        return int(e.value.rstrip('uUlL'), 0)
    if isinstance(e, c_ast.UnaryOp) and e.op == '-':
        k = const_int(e.expr)
        return None if k is None else -k
    return None


def code_tuple(e):
    """('GETCONST' or 'GETNAMEV', index expression) for an item of the
    code's constants or names, as GETITEM expands: ((PyTupleObject *)
    consts)->ob_item[i]."""
    e = strip_casts(e)
    if isinstance(e, c_ast.ArrayRef) and isinstance(e.name, c_ast.StructRef) \
            and e.name.field.name == 'ob_item':
        b = strip_casts(e.name.name)
        while isinstance(b, c_ast.ExprList) and all(
                isinstance(strip_casts(x), c_ast.Constant) for x in b.exprs[:-1]):
            b = strip_casts(b.exprs[-1])    # (assert(...), x) under NDEBUG
        if isinstance(b, c_ast.ID) and b.name in CODE_TUPLES:
            return CODE_TUPLES[b.name], e.subscript
    return None


def local_item(e):
    """The index of fastlocals[i], or None."""
    e = strip_casts(e)
    if isinstance(e, c_ast.ArrayRef) and isinstance(e.name, c_ast.ID) \
            and e.name.name == 'fastlocals':
        return e.subscript
    return None


def predict(s):
    """PREDICT(op)'s expansion: a do-while(0) whose body jumps to a
    PRED_ label; without computed gotos it only saves a dispatch."""
    if not (isinstance(s, c_ast.DoWhile) and const_int(s.cond) == 0):
        return False
    return any(isinstance(n, c_ast.Goto) and n.name.startswith('PRED_')
               for n in walk_nodes(s.stmt))


def setlocal(s):
    """SETLOCAL(i, v)'s expansion, do { tmp = fastlocals[i];
    fastlocals[i] = v; Py_XDECREF(tmp); } while (0): (i, v), else None."""
    if not once_do(s):
        return None
    xs = body(s.stmt)
    if len(xs) != 3 or not isinstance(xs[0], c_ast.Decl) or xs[0].init is None \
            or local_item(xs[0].init) is None:
        return None
    st, dec = xs[1], xs[2]
    if not (isinstance(st, c_ast.Assignment) and st.op == '=' and local_item(st.lvalue) is not None):
        return None
    from .vocabulary import of_marker
    if not (isinstance(dec, c_ast.FuncCall) and isinstance(dec.name, c_ast.ID)
            and getattr(of_marker(dec.name.name), 'name', None) == 'Py_XDECREF'):
        return None
    a = strip_casts(dec.args.exprs[0])
    if not (isinstance(a, c_ast.ID) and a.name == xs[0].name):
        return None
    return local_item(st.lvalue), st.rvalue


def walk_nodes(n):
    yield n
    for _, c in n.children():
        yield from walk_nodes(c)


class Summary:
    """What the walk needs of a case: it has no result of its own."""
    kind = 'ct'
    pbt = ['meta']
    params = ['oparg']


class CaseWalk(Walk):
    def __init__(self, tr, fn, sm, name):
        super().__init__(tr, fn, sm, [])
        self.case = name
        self.effects = []         # the stack effect of each path that succeeds

    def fail_line(self, p):
        if self.loops or self.switch_depth:
            raise Refuse('an error inside a meta loop or switch')
        st = p.stack
        moved = sorted(d for d, u in st.uses.items() if any(h != 'released' for h in u))
        if moved:
            raise Refuse('an error after the input at depth %d was moved' % moved[0])
        return 'break;'

    def held_outside(self, v):
        # an input is the compiler's stack's, which Ni's metas promote
        return super().held_outside(v) or getattr(v, 'input', None) is not None

    def constant_text(self, key):
        if isinstance(key, tuple):
            return 'vinfo_new(CompileTime_New((long)%s(%s)))' % key
        return super().constant_text(key)

    def release_all(self, p, out, keep=None):
        super().release_all(p, out, keep)
        for o in sorted(p.stack.slots):
            x = p.stack.slots[o]
            if x.owned and p.vars[x.var].own != 'none':
                # (none: the error test of the variable pushed held)
                out.line('vinfo_decref(%s, po);' % x.c)

    # ---- the case ----

    def run(self):
        from .values import Path
        p = Path()
        p.stack = Stack()
        v = Var('oparg', INT, 'meta', 'oparg', isparam=True)
        p.vars['oparg'] = v
        out = Code()
        self.out = out
        self.body_start = 0
        self.block(body(self.fn.body), p, out, [])
        if not p.ended:
            raise Refuse('the case falls through to the next')

    def block(self, stmts, p, out, frames, start=0):
        i = start
        while i < len(stmts) and not p.ended:
            s = stmts[i]
            self.statement(s, p, out, frames + [(stmts, i)])
            if not p.ended:
                self.release_dead(p, out, self.live.after[id(s)])
            i += 1

    def statement(self, s, p, out, frames):
        outer = self.stmt_ncalls, self.stmt_frames, self.stmt_called
        self.stmt_ncalls, self.stmt_frames = self.rec.ncalls, frames
        self.stmt_called = p.called
        try:
            self.stmt(s, p, out, frames)
        except ResumeHere as e:
            raise Refuse(str(e))
        finally:
            self.stmt_ncalls, self.stmt_frames, self.stmt_called = outer

    def stmt(self, s, p, out, frames):
        if isinstance(s, c_ast.Continue):
            return self.success(p, out)
        if predict(s):
            return
        sl = setlocal(s)
        if sl is not None:
            return self.store_local(p, out, sl[0], sl[1], frames)
        if isinstance(s, (c_ast.For, c_ast.While)) \
                or (isinstance(s, c_ast.DoWhile) and not once_do(s)):
            raise Refuse('a loop')
        super().stmt(s, p, out, frames)

    def goto(self, s, p, out, frames):
        if s.name == 'error':
            self.release_all(p, out)
            out.line(self.fail_line(p))
            p.ended = True
            return
        if s.name == 'fast_next_opcode':
            return self.success(p, out)
        raise Refuse('goto %s' % s.name)

    def success(self, p, out):
        """The epilogue: pop the inputs the C touched, release those it
        decref'd, push what it left; then on to the next instruction."""
        super().release_all(p, out)
        st = p.stack
        touched = max([-st.low] + [-o for o in st.slots if o < 0] + [0])
        for o, x in st.slots.items():
            if o >= st.sp and x.owned:
                raise Refuse('a value written above the stack pointer')
        contents = []
        for o in range(-touched, st.sp):
            if o in st.slots:
                contents.append(st.slots[o])
            elif o < 0:
                contents.append(Pushed(None, False, -o))
            else:
                raise Refuse('a slot pushed with no value')
        names = {}
        for d in range(1, touched + 1):
            if d not in st.names:
                st.names[d] = self.new_tmp('vinfo_t *')
            names[d] = st.names[d]
        for d in st.uses:
            if not isinstance(d, int) or d > touched:
                raise Refuse('the input at depth %s consumed but not popped' % d)
        for x in contents:
            if x.depth is not None:
                st.uses.setdefault(x.depth, []).append('pushed')
        released = []
        for d in range(1, touched + 1):
            u = st.uses.get(d, [])
            if len(u) != 1:
                raise Refuse('the input at depth %d consumed %d times' % (d, len(u)))
            if u[0] == 'released':
                released.append(d)
        for d in range(1, touched + 1):
            out.line('POP(%s);' % names[d])
        for d in released:
            out.line('vinfo_decref(%s, po);' % names[d])
        for x in contents:
            out.line('PUSH(%s);' % (x.c if x.depth is None else names[x.depth]))
        out.line('goto fine;')
        self.effects.append(st.sp)
        p.ended = True

    # ---- the stack ----

    def slot_of(self, e, p, out, frames):
        """The offset from the entry of the slot e names, if it is one:
        stack_pointer[i] or *(stack_pointer++) (which moves the pointer)."""
        st = p.stack
        x = strip_casts(e)
        if isinstance(x, c_ast.ArrayRef) and is_sp(x.name):
            k = const_int(x.subscript)
            if k is None:
                raise Refuse('a stack slot at a run-time index %s' % text_of(x.subscript))
            return st.sp + k
        if isinstance(x, c_ast.UnaryOp) and x.op == '*' and isinstance(x.expr, c_ast.UnaryOp) \
                and is_sp(x.expr.expr):
            op = x.expr.op
            if op == 'p++':
                st.sp += 1
                return st.sp - 1
            if op == '--':
                st.sp -= 1
                st.low = min(st.low, st.sp)
                return st.sp
        return None

    def read_slot(self, e, p, out, frames):
        st = p.stack
        x = strip_casts(e)
        if isinstance(x, c_ast.ArrayRef) and is_sp(x.name) and const_int(x.subscript) is None:
            # PEEK(n) at an oparg: an input, read where it is
            i = strip_casts(x.subscript)
            if not (isinstance(i, c_ast.UnaryOp) and i.op == '-'):
                raise Refuse('a stack slot at %s' % text_of(x.subscript))
            n = self.ev(i.expr, p, out, frames)
            if n.bt != 'meta':
                raise Refuse('a stack slot at a run-time depth')
            depth = '%s + %d' % (n.c, -st.sp) if st.sp else n.c
            v = Val('dynamic', 'NTOP(%s)' % depth, types_obj(self.u))
            v.input = depth
            return v
        o = self.slot_of(e, p, out, frames)
        if o is None:
            return None
        if o in st.slots:
            x = st.slots[o]
            if x.depth is not None:
                v = Val('dynamic', st.names.get(x.depth, 'NTOP(%d)' % x.depth), types_obj(self.u))
                v.input = x.depth
                return v
            raise Refuse('a pushed value read back from the stack')
        if o >= 0:
            raise Refuse('a read above the stack')
        v = Val('dynamic', 'NTOP(%d)' % -o, types_obj(self.u))
        v.input = -o
        return v

    def write_slot(self, o, val, p, out):
        st = p.stack
        if o in st.slots and st.slots[o].owned:
            raise Refuse('a stack slot written twice')
        if val.bt != 'dynamic':
            raise Refuse('a %s value pushed' % val.bt)
        v = val.var
        if v is None:
            raise Refuse('a value pushed that no variable holds: %s' % val.c)
        if v.owned():
            if v.own != 'new':
                raise Refuse('%s pushed where it may be NULL' % v.name)
            st.slots[o] = Pushed(v.cname, True, var=v.name)
            v.own = 'borrowed'
        elif getattr(v, 'input', None) is not None and isinstance(v.input, int):
            st.slots[o] = Pushed(None, False, v.input)
        else:
            raise Refuse('%s pushed, a borrowed reference' % v.name)

    def store_local(self, p, out, idx, e, frames):
        i = self.ev(idx, p, out, frames)
        val = self.ev(e, p, out, frames)
        if i.bt != 'meta' or val.bt != 'dynamic' or val.var is None:
            raise Refuse('SETLOCAL of %s' % text_of(e))
        v = val.var
        if v.owned():
            if v.own != 'new':
                raise Refuse('%s stored where it may be NULL' % v.name)
            v.own = 'borrowed'
        elif isinstance(getattr(v, 'input', None), int):
            p.stack.uses.setdefault(v.input, []).append('stored')
        else:
            raise Refuse('%s stored, a borrowed reference' % v.name)
        out.line('SETLOCAL(%s, %s);' % (i.c, v.cname))

    # ---- expressions and statements of the frame ----

    def ev(self, e, p, out, frames, dest=None):
        x = strip_casts(e)
        if isinstance(x, (c_ast.ArrayRef, c_ast.UnaryOp)):
            v = self.read_slot(x, p, out, frames)
            if v is not None and dest is None and v.var is None:
                # an input as an operand: a variable of its own
                t = self.new_tmp('vinfo_t *')
                tv = Var(t, v.ctype, 'dynamic', t)
                tv.own, tv.input = 'borrowed', v.input
                p.vars[t] = tv
                out.line('%s = %s;' % (t, v.c))
                if isinstance(v.input, int):
                    p.stack.names.setdefault(v.input, t)
                r = Val('dynamic', t, v.ctype, var=tv)
                r.input = v.input
                return r
            if v is not None:
                return v
            ct = code_tuple(x)
            if ct is not None:
                i = self.ev(ct[1], p, out, frames)
                if i.bt != 'meta':
                    raise Refuse('an item of the code at a run-time index')
                return self.const_var(p, out, (ct[0], i.c))
            li = local_item(x)
            if li is not None:
                raise Refuse('a read of fastlocals')
        s = singleton(x)
        if s is not None:
            return self.const_var(p, out, s)
        if isinstance(x, c_ast.ID) and x.name not in p.vars:
            if x.name in CORE or x.name == 'stack_pointer':
                raise Refuse('core: reads the frame\'s %s' % x.name)
        return super().ev(e, p, out, frames, dest)

    def const_var(self, p, out, key):
        """Ni's constant of key (a singleton, or an item of the code) as a
        value: a variable of its own, released when dead."""
        t = self.new_tmp('vinfo_t *')
        v = Var(t, types_obj(self.u), 'dynamic', t)
        v.singleton = key
        p.vars[t] = v
        return self.ev_id(t, p, out)

    def held_call(self, val, out):
        if val.conv.kind == 'ok':
            # a meta's bool, kept for the C's test of its status
            self.status_held = True
            return Val('dynamic', val.c, INT, conv=val.conv)
        return super().held_call(val, out)

    def ev_id(self, name, p, out):
        v = p.vars.get(name)
        if v is not None and getattr(v, 'status', False):
            return Val('call', v.cname, INT, conv=v.conv)
        return super().ev_id(name, p, out)

    def assign(self, p, out, name, e, frames):
        x = strip_casts(e)
        ct = code_tuple(x)
        v = p.vars.get(name)
        if ct is not None and v is not None and v.bt == 'dynamic':
            i = self.ev(ct[1], p, out, frames)
            if i.bt != 'meta':
                raise Refuse('an item of the code at a run-time index')
            if v.owned():
                self.release(v, out)
            v.own, v.singleton, v.constant, v.known_null, v.conv = \
                'none', (ct[0], i.c), None, False, None
            v.input = None
            return
        val = self.ev(e, p, out, frames, dest=name) if self.stack_read(x) else None
        if val is not None:
            if v is None or v.bt != 'dynamic':
                raise Refuse('a stack value in %s' % name)
            if v.owned():
                self.release(v, out)
            out.line('%s = %s;' % (v.cname, val.c))
            v.own, v.singleton, v.constant, v.known_null, v.conv = \
                'borrowed', None, None, False, None
            v.input = val.input
            if isinstance(val.input, int):
                p.stack.names.setdefault(val.input, v.cname)
            return
        if v is not None:
            v.input = None
            v.status = False
        self.status_held = False
        super().assign(p, out, name, e, frames)
        if self.status_held:
            v.status, v.own = True, 'none'
            self.rec.decls[v.cname] = 'bool %s;' % v.cname

    def stack_read(self, x):
        if isinstance(x, c_ast.ArrayRef) and is_sp(x.name):
            return True
        return isinstance(x, c_ast.UnaryOp) and x.op == '*' \
            and isinstance(x.expr, c_ast.UnaryOp) and is_sp(x.expr.expr)

    def expr_stmt(self, e, p, out, frames):
        if isinstance(e, c_ast.Assignment):
            lv = strip_casts(e.lvalue)
            if is_sp(lv):
                k = const_int(e.rvalue)
                if e.op not in ('+=', '-=') or k is None:
                    raise Refuse('the stack pointer moved by %s' % text_of(e.rvalue))
                p.stack.sp += k if e.op == '+=' else -k
                p.stack.low = min(p.stack.low, p.stack.sp)
                return
            if e.op == '=' and self.stack_read(lv):
                val = self.ev(e.rvalue, p, out, frames)
                o = self.slot_of(lv, p, out, frames)
                self.write_slot(o, val, p, out)
                return
            if local_item(lv) is not None:
                raise Refuse('a store into fastlocals')
        if isinstance(e, c_ast.FuncCall) and isinstance(e.name, c_ast.ID) and e.args:
            from .vocabulary import of_marker
            entry = of_marker(e.name.name)
            a = strip_casts(e.args.exprs[0])
            v = p.vars.get(a.name) if isinstance(a, c_ast.ID) else None
            if entry is not None and entry.name in ('Py_DECREF', 'Py_XDECREF') \
                    and v is not None and v.own == 'borrowed' \
                    and getattr(v, 'input', None) is not None:
                if not isinstance(v.input, int):
                    raise Refuse('a decref of the input at depth %s' % v.input)
                p.stack.uses.setdefault(v.input, []).append('released')
                v.own = 'none'
                return
        super().expr_stmt(e, p, out, frames)


def types_obj(unit):
    return c_ast.PtrDecl([], c_ast.TypeDecl(None, [], None, c_ast.IdentifierType(['PyObject'])))


def text_of(e):
    from . import cprint
    return cprint.expr(e)


# ---- the cases of the source ----------------------------------------------

def opmap(python):
    r = subprocess.run([python, '-I', '-c', 'import dis, json; print(json.dumps(dis.opmap))'],
                       check=True, stdout=subprocess.PIPE, universal_newlines=True, timeout=60)
    return json.loads(r.stdout)


def stack_effects(names, python):
    """{name: [dis.stack_effect at each of OPARGS] or None}."""
    code = ('import dis, json, opcode, sys\n'
            'out = {}\n'
            'for n in sys.argv[2:]:\n'
            '    op = opcode.opmap[n]\n'
            '    try:\n'
            '        out[n] = [dis.stack_effect(op, a if op >= opcode.HAVE_ARGUMENT else None)\n'
            '                  for a in json.loads(sys.argv[1])]\n'
            '    except ValueError:\n'
            '        out[n] = None  # unknown to compile.c (NOP)\n'
            'print(json.dumps(out))\n')
    r = subprocess.run([python, '-I', '-c', code, json.dumps(OPARGS)] + names, check=True,
                       stdout=subprocess.PIPE, universal_newlines=True, timeout=60)
    return json.loads(r.stdout)


def cases(ast, ops, function=scope.CEVAL_FUNCTION):
    """[(name, [statements])] of the eval loop's switch, in source order;
    the cases sharing one body each get it."""
    fn = next(d for d in ast.ext if isinstance(d, c_ast.FuncDef)
              and d.decl.name == function)
    sw = next(n for n in walk_nodes(fn.body) if isinstance(n, c_ast.Switch)
              and isinstance(n.cond, c_ast.ID) and n.cond.name == 'opcode')
    names = {v: k for k, v in ops.items()}
    out = []
    pending = []
    for c in sw.stmt.block_items:
        while isinstance(c, c_ast.Case):
            k = const_int(c.expr)
            if k not in names:
                raise Refuse('case %s is no opcode' % c.expr.value)
            pending.append(names[k])
            stmts = c.stmts or []
            if len(stmts) == 1 and isinstance(stmts[0], c_ast.Case):
                c = stmts[0]
                continue
            if stmts:
                out += [(n, stmts) for n in pending]
                pending = []
            break
    return out


def case_function(name, stmts):
    """The case's statements as a function of oparg, for the analyses."""
    param = c_ast.Decl('oparg', [], [], [], [], c_ast.TypeDecl('oparg', [], None,
                       c_ast.IdentifierType(['int'])), None, None)
    decl = c_ast.Decl('case_' + name, [], [], [], [], c_ast.FuncDecl(
        c_ast.ParamList([param]), c_ast.TypeDecl('case_' + name, [], None,
                                                 c_ast.IdentifierType(['void']))), None, None)
    return c_ast.FuncDef(decl, None, c_ast.Compound(list(stmts)))


def translate(tr, name, fn):
    """The case's C for pycompiler.c's switch; Refuse if it has none."""
    for n in walk_nodes(fn.body):
        if isinstance(n, c_ast.UnaryOp) and n.op == '&' and is_sp(strip_casts(n.expr)):
            raise Refuse('core: hands the stack pointer to a call')
    tr.liveness[fn.decl.name] = Liveness(fn.body)
    tr.local_bts[fn.decl.name] = locals_bt(fn, tr.unit, {'oparg': 'meta'})
    tr.loop_hoists[fn.decl.name] = {}
    w = CaseWalk(tr, fn, Summary(), name)
    w.run()
    lines = ['      case %s: {' % name]
    used = '\n'.join(w.out.render(0))
    import re
    words = set(re.findall(r'\b\w+\b', used))
    lines += ['        ' + d for n, d in sorted(w.rec.decls.items()) if n in words]
    lines += ['        ' + ln for ln in w.out.render(0)]
    lines.append('      }')
    return '\n'.join(lines), w.effects


def check_effect(name, effects, dis_effects):
    """A problem, or None: the most a path of the case grows the stack
    is what dis.stack_effect gives at every oparg."""
    want = dis_effects.get(name)
    if want is None or not effects:
        return None
    got = max(effects)
    if any(w != got for w in want):
        return '%s: the stack grows %d, dis.stack_effect says %s (opargs %s)' % (
            name, got, want, list(OPARGS))
    return None


def generate(target, only=None):
    """{name: case text}, {name: why refused}, [problems] for the
    target's cases (those in `only`, if given)."""
    ft = front.target(target.python, target.src)
    ast = front.parse(ft, CEVAL.cfile)
    ni = conventions.ni_decls(ROOT, scope.NI_HEADERS)
    tr = Translator(CEVAL, ast, ni, set(), policy.load(ft, CEVAL), None)
    ops = opmap(target.python)
    all_cases = cases(ast, ops)
    dis_effects = stack_effects(sorted({n for n, _ in all_cases}), target.python)
    done, refused, problems = {}, {}, []
    for name, stmts in all_cases:
        if only is not None and name not in only:
            continue
        fn = case_function(name, stmts)
        try:
            done[name], effects = translate(tr, name, fn)
        except Refuse as e:
            refused[name] = str(e)
            continue
        pr = check_effect(name, effects, dis_effects)
        if pr:
            problems.append(pr)
    return done, refused, problems, [n for n, _ in all_cases]


# The cases moved onto this translator so far; gen_ceval.py writes the rest
# into the same header until each has moved (Migration step 3).
MOVED = ['NOP', 'LOAD_CONST', 'STORE_FAST', 'POP_TOP', 'ROT_TWO', 'ROT_THREE', 'DUP_TOP',
         'DUP_TOP_TWO', 'UNARY_POSITIVE', 'UNARY_NEGATIVE', 'UNARY_INVERT',
         'BINARY_POWER', 'BINARY_MULTIPLY', 'BINARY_MATRIX_MULTIPLY', 'BINARY_TRUE_DIVIDE',
         'BINARY_FLOOR_DIVIDE', 'BINARY_SUBTRACT', 'BINARY_SUBSCR', 'BINARY_LSHIFT',
         'BINARY_RSHIFT', 'BINARY_AND', 'BINARY_XOR', 'BINARY_OR', 'LIST_APPEND',
         'INPLACE_POWER', 'INPLACE_MULTIPLY', 'INPLACE_MATRIX_MULTIPLY',
         'INPLACE_TRUE_DIVIDE', 'INPLACE_FLOOR_DIVIDE', 'INPLACE_MODULO',
         'INPLACE_SUBTRACT', 'INPLACE_LSHIFT', 'INPLACE_RSHIFT', 'INPLACE_AND',
         'INPLACE_XOR', 'INPLACE_OR', 'STORE_SUBSCR', 'DELETE_SUBSCR', 'STORE_ATTR',
         'DELETE_ATTR', 'LOAD_ATTR', 'GET_ITER']


def build(target):
    import re
    import gen_ceval
    try:
        done, refused, problems, _ = generate(target, set(MOVED))
    except (front.FrontError, policy.PolicyError) as e:
        raise cslots.GenError(str(e))
    if problems:
        raise cslots.GenError('the cases do not match dis.stack_effect:\n'
                              + '\n'.join('  ' + p for p in problems))
    text, report = gen_ceval.build(target)
    for name in MOVED:
        if name not in done:
            raise cslots.GenError('%s: %s' % (name, refused.get(name, 'not a case')))
        old = re.compile(r'^      case %s: \{\n.*?^      \}\n' % name, re.M | re.S)
        if not old.search(text):
            raise cslots.GenError('%s: gen_ceval.py does not generate it' % name)
        text = old.sub(lambda m: done[name] + '\n', text, count=1)
    text = text.replace('/* GENERATED by tools/metac/gen_ceval.py from',
                        '/* GENERATED by tools/metac/gen2/opcodes.py and gen_ceval.py from', 1)
    return text, report
