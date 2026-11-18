"""Raw addresses: a run-time 'char *' into an object, moved by an offset
the compiler knows (the design's "Abstract values"; METACOMPILER.md
"Descriptors").

PyMember_GetOne(addr, l) is given an object as 'const char *', moves it by
a field's offset (l->offset, a C value of the compiler) and reads the
bytes there. In the meta the address is the object's vinfo and the offset
(Var.offset, C text):
  - 'p += E', E known now, adds E to the offset and emits nothing;
  - '*(T *)p' is psyco_get_field_offset(po, p, FMUT(DEF_ARRAY(T, 0)),
    offset), UNSIGNED_ARRAY for an unsigned T: read each time it is
    named, since the bytes are the object's and may change (FMUT);
  - p as a value is integer_add_i(po, p, offset, false).
An object read so is a vinfo the meta holds, but no reference to the
object (Var.noref): Py_INCREF of it is need_reference, its compare with
NULL a run-time test of the word. A float is no word Ni reads: the
function resumes there.
"""
from pycparser import c_ast

from .ctyping import strip_casts, text
from .errors import ResumeHere
from .values import Conv, Val

# the integer types read through an address: their width in bits, and
# whether they are signed
INT_READS = {
    'char': (8, True), 'signed char': (8, True), 'unsigned char': (8, False),
    'short': (16, True), 'unsigned short': (16, False),
    'int': (32, True), 'unsigned int': (32, False),
    'long': (64, True), 'unsigned long': (64, False),
    'Py_ssize_t': (64, True), 'long long': (64, True),
    'unsigned long long': (64, False),
}

# the header's assertion where a 'char' is read as signed
CHAR_SIGNED = '_Static_assert((char)-1 < 0, "a char member reads signed");'


def var(w, p, e):
    """The address variable e names (through casts), or None."""
    e = strip_casts(e)
    if isinstance(e, c_ast.ID) and e.name in p.vars:
        v = p.vars[e.name]
        if v.bt == 'dynamic' and v.offset is not None:
            return v
    return None


def step(w, p, out, name, op, e, frames):
    """'p += E' (or -=) on a run-time 'char *' parameter, E known now:
    True if it was one."""
    v = p.vars.get(name)
    if op not in ('+=', '-=') or v is None or v.bt != 'dynamic' or not v.isparam \
            or not w.u.is_char_ptr(v.ctype):
        return False
    val = w.ev(e, p, out, frames)
    if val.bt != 'meta':
        raise ResumeHere('%s moved by a run-time amount' % name)
    c = val.c if val.c.startswith('(') and val.c.endswith(')') else '(%s)' % val.c
    v.offset = '%s %s %s' % (v.offset or '0', op[0], c)
    return True


def load(w, e, p, out, frames):
    """'*(T *)p' of an address: the bytes there as a vinfo; else None."""
    if not (isinstance(e, c_ast.UnaryOp) and e.op == '*' and isinstance(e.expr, c_ast.Cast)):
        return None
    v = var(w, p, e.expr.expr)
    if v is None:
        return None
    pt = w.u.pointee(e.expr.to_type.type)
    if pt is None:
        return None
    t = text(pt).replace('const ', '').strip()
    if t in INT_READS:
        arr = 'DEF_ARRAY' if INT_READS[t][1] else 'UNSIGNED_ARRAY'
        if t == 'char':
            w.tr.header_asserts.add(CHAR_SIGNED)
    elif w.u.pointee(pt) is not None or t == 'double':
        arr = 'DEF_ARRAY'
    else:
        raise ResumeHere('a read of a %s through %s' % (t, v.name))
    r = w.new_tmp('vinfo_t *')
    out.line('%s = psyco_get_field_offset(po, %s, FMUT(%s(%s, 0)), %s);'
             % (r, v.cname, arr, t, v.offset))
    w.error_exit(p, out, '%s == NULL' % r)
    # tested above: a scalar read may be a call's argument (held_scalar)
    val = Val('dynamic', r, pt, owned=True, conv=Conv(never=True))
    val.noref = w.u.is_object(pt)
    if t in INT_READS:
        bits, signed = INT_READS[t]
        val.rng = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1)
    return val


def value(w, v, out):
    """The address variable v as a value: the object's plus the offset."""
    r = w.new_tmp('vinfo_t *')
    out.line('%s = integer_add_i(po, %s, %s, false);' % (r, v.cname, v.offset))
    return Val('dynamic', r, v.ctype, owned=True, conv=Conv(never=True))
