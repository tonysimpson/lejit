"""Generate the compile-time float slots from CPython's Objects/floatobject.c.

Second meta-compiler slice (doc/METACOMPILER.md). Every slot in
float_as_number is tried. A slot is translated if its C body is:

    double x, y, ...;
    CONVERT_TO_DOUBLE(param, x); ...        (or reads of param->ob_fval)
    <code on doubles only>
    return PyFloat_FromDouble(EXPR);        (nb_bool: return EXPR;)

The backend has no floating-point instructions, so the double-only code is
copied, nearly verbatim, into a C helper that the meta calls (CfPure, so it
folds when the operands are constants; CfPyErrCheck if it can raise). The
result is a virtual float. CONVERT_TO_DOUBLE and the ob_fval field are the
representation, hand-written in pfloatobject.c/.h: psyco_convert_to_double
and PsycoFloat_AS_DOUBLE. Anything else is skipped with the reason.

Run by generate.py, which writes pfloatobject_gen.h.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ctokens  # noqa: E402
import cslots  # noqa: E402
from cslots import Skip, text, is_seq, statements  # noqa: E402


# names the copied double-only code may use, besides its double locals
LIBM = {'fmod', 'copysign', 'fabs', 'floor'}
ERRORS = {'PyErr_SetString'}
KEYWORDS = {'if', 'else'}
# fields of a float operand, and how a meta reads them (pfloatobject.h)
FIELDS = {'ob_fval': 'PsycoFloat_AS_DOUBLE'}
# macros with no effect in this build (pyfpe.h, without WANT_SIGFPE_HANDLER)
NO_OPS = {'PyFPE_START_PROTECT', 'PyFPE_END_PROTECT'}


def strip_no_ops(toks):
    out, i = [], 0
    while i < len(toks):
        if toks[i].text in NO_OPS and toks[i + 1].text == '(':
            i = ctokens.match_close(toks, i + 1) + 1
            if i < len(toks) and toks[i].text == ';':
                i += 1
        else:
            out.append(toks[i])
            i += 1
    return out


def translate(cname, slot, params_toks, body, macros):
    params = [[t.text for t in p if t.kind == 'id'][-1]
              for p in ctokens.split_top(params_toks, ',')]
    stmts = statements(strip_no_ops(body))
    doubles = []                     # declared double locals, in order
    i = 0
    while (i < len(stmts) and stmts[i][0].text == 'double'
           and '=' not in [t.text for t in stmts[i]]):
        doubles += [t.text for t in stmts[i][1:-1] if t.kind == 'id']
        i += 1
    converts = []                    # (param, double local)
    while i < len(stmts) and stmts[i][0].text == 'CONVERT_TO_DOUBLE':
        w = [t.text for t in stmts[i]]
        if (len(w) != 7 or w[2] not in params or w[4] not in doubles
                or w[1::2] != ['(', ',', ')']):
            raise Skip('CONVERT_TO_DOUBLE %r' % text(stmts[i]))
        converts.append((w[2], w[4]))
        i += 1
    if i >= len(stmts) or stmts[-1][0].text != 'return':
        raise Skip('does not end with a return')
    middle, ret = stmts[i:-1], stmts[-1]
    if slot == 'nb_bool':
        result, rtype, value = ret[1:-1], 'int', None
    else:
        if (len(ret) < 5 or ret[1].text != 'PyFloat_FromDouble'
                or ctokens.match_close(ret, 2) != len(ret) - 2):
            raise Skip('returns %r, not PyFloat_FromDouble(...)'
                       % text(ret[1:-1]))
        result, rtype, value = ret[3:-2], 'double', 'float'
    converted = [x for _, x in converts]
    fields = []                      # (param, field) read in the code
    can_raise = False
    code = []
    for st in middle + [result]:
        out, k = [], 0
        while k < len(st):
            t = st[k]
            nxt = [x.text for x in st[k + 1:k + 3]]
            if t.text in params and nxt[:1] == ['->']:
                if len(nxt) < 2 or nxt[1] not in FIELDS:
                    raise Skip('reads %s->%s' % (t.text, nxt[1:2]))
                if (t.text, nxt[1]) not in fields:
                    fields.append((t.text, nxt[1]))
                out.append(t._replace(text='%s_%s' % (t.text, nxt[1])))
                k += 3
                continue
            if t.text == 'return':
                if nxt[:2] != ['NULL', ';']:
                    raise Skip('returns early with %r' % text(st[k:k + 3]))
                out.append(t)
                out.append(t._replace(text='-1', sp=True))
                k += 2
                continue
            if t.kind == 'id':
                if t.text in ERRORS:
                    can_raise = True
                elif not (t.text in doubles or t.text in LIBM
                          or t.text in KEYWORDS
                          or t.text.startswith('PyExc_')):
                    raise Skip('uses %r, not only doubles' % t.text)
            out.append(t)
            k += 1
        code.append(out)
    if not converts and not fields:
        raise Skip('no float operand')
    if can_raise and not any(t.text == 'return' for st in code for t in st):
        raise Skip('sets an error without returning')
    args = converted + ['%s_%s' % f for f in fields]
    locals_ = [d for d in doubles if d not in converted]
    return emit(cname, slot, params, converts, fields, args, locals_,
                code[:-1], code[-1], rtype, value, can_raise)


def c_lines(toks, indent):
    """Lay out copied statements one per line."""
    lines, cur, depth = [], '', indent
    for k, t in enumerate(toks):
        if t.text == '}':
            if cur.strip():
                lines.append(cur)
            depth -= 1
            cur = '  ' * depth + '}'
            nxt = toks[k + 1].text if k + 1 < len(toks) else None
            if nxt != 'else':
                lines.append(cur)
                cur = ''
            continue
        if not cur:
            cur = '  ' * depth
        elif cur.strip():
            cur += ' ' if t.sp or t.text in ('{', 'else') else ''
        cur += t.text
        if t.text == '{':
            lines.append(cur)
            cur = ''
            depth += 1
        elif t.text == ';' and _paren_balanced(cur):
            lines.append(cur)
            cur = ''
    if cur.strip():
        lines.append(cur)
    return lines


def _paren_balanced(s):
    return s.count('(') == s.count(')')


def emit(cname, slot, params, converts, fields, args, locals_, middle,
         result, rtype, value, can_raise):
    helper = 'cimpl_' + cname
    out = ['/* Objects/floatobject.c:%s: %s */' % (cname, slot)]
    out.append('static %s %s(%s) {' % (rtype, helper, ', '.join(
        'double ' + a for a in args)))
    if locals_:
        out.append('  double %s;' % ', '.join(locals_))
    for st in middle:
        out += c_lines(st, 1)
    out.append('  return %s;' % text(result))
    out.append('}')
    out.append('')
    py = ['p' + x for x in params]
    out.append('static vinfo_t *p%s(PsycoObject *po, %s) {'
               % (cname, ', '.join('vinfo_t *' + x for x in py)))
    out.append('  vinfo_t %s;' % ', '.join(
        '*' + v for v in [x for _, x in converts] + ['%s_%s' % f for f in fields]
        + ['x']))
    if converts:
        out.append('  int r;')
    held = []
    for p, x in converts:
        out.append('  /* CONVERT_TO_DOUBLE(%s, %s) */' % (p, x))
        out.append('  r = psyco_convert_to_double(po, p%s, &%s);' % (p, x))
        tail = 'return r == false ? NULL : psyco_vi_NotImplemented();'
        if held:
            out.append('  if (r != true) {')
            for h in reversed(held):
                out.append('    vinfo_decref(%s, po);' % h)
            out.append('    ' + tail)
            out.append('  }')
        else:
            out.append('  if (r != true)')
            out.append('    ' + tail)
        held.append(x)
    for p, f in fields:
        v = '%s_%s' % (p, f)
        out.append('  /* %s->%s */' % (p, f))
        out.append('  %s = %s(po, p%s); /* borrowed */' % (v, FIELDS[f], p))
        out.append('  if (%s == NULL) {' % v if held else '  if (%s == NULL)' % v)
        for h in reversed(held):
            out.append('    vinfo_decref(%s, po);' % h)
        out.append('    return NULL;')
        if held:
            out.append('  }')
    flags = ('CfPyErrCheck' if can_raise else 'CfPure') + \
        (' | CfReturnTypeDouble' if rtype == 'double' else ' | CfReturnTypeInt')
    if can_raise:
        out.append('  /* not CfPure: it can raise */')
    out.append('  x = psyco_generic_call(po, %s, %s, "%s", %s);'
               % (helper, flags, 'd' * len(args), ', '.join(args)))
    for h in reversed(held):
        out.append('  vinfo_decref(%s, po);' % h)
    if value == 'float':
        out.append('  return x != NULL ? PsycoFloat_FROM_DOUBLE(x) : NULL;')
    else:
        out.append('  return x;')
    out.append('}')
    return '\n'.join(out)


def generate(src, cfg):
    with open(cfg) as f:
        if 'WANT_SIGFPE_HANDLER' in ctokens.tokenize(f.read())[1]:
            raise cslots.GenError('%s defines WANT_SIGFPE_HANDLER: the PyFPE_*'
                             ' macros are not no-ops' % cfg)
    head = ['#ifdef WANT_SIGFPE_HANDLER',
            '#error "generated assuming the PyFPE_* macros are no-ops"',
            '#endif']
    return cslots.generate(
        src, 'floatobject.c', 'float_as_number', 'gen_float.py',
        'pfloatobject.c', 'psyco_convert_to_double', 'pfloat_generated_init',
        translate, head)


def build(target):
    out, done, skipped = generate(target.src, target.pyconfig)
    return out, cslots.report(done, skipped)
