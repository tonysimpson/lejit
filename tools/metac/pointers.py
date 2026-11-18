"""Rules of the general translator for a run-time 'char *' into an object
(rule raw_pointer, D89; METACOMPILER.md "str and descriptors"). Mixed into
gen_abstract.Walk; each hook answers None when it doesn't apply.

PyMember_GetOne(addr, l) is given an object as 'const char *' and moves it
by a field's offset, a C value of the compiler (l->offset); then it reads
'*(T *)addr', or passes '(char *)addr' on. In the meta the pointer is the
object's vinfo plus an offset known now:
  - 'p += E' with E known now adds E to the offset, and emits nothing;
  - '*(T *)p' is psyco_get_field_offset(po, base, FMUT(DEF_ARRAY(T, 0)),
    offset) (UNSIGNED_ARRAY for an unsigned T): read again each time,
    since the bytes are the object's and may change (FMUT);
  - 'p' itself as a value is integer_add_i(po, base, offset, false);
  - a 'PyObject *' read so holds no reference: Py_INCREF of it is
    need_reference (policy ('ref', 'need_reference')), and it may be NULL,
    so comparing it with NULL is a run-time test.
"""
GA = None       # gen_abstract, set by it

# Ni's constant of each singleton, a new reference (as gen_builtins.py's)
SINGLETONS = {'Py_None': 'psyco_vi_None', 'Py_True': 'psyco_vi_Py_True',
              'Py_False': 'psyco_vi_Py_False'}

RAW_TYPES = {'char *', 'const char *'}
# integer C types read through a pointer, with their width in bits and
# whether they are signed
INT_READS = {
    'char': (8, True), 'signed char': (8, True), 'unsigned char': (8, False),
    'short': (16, True), 'unsigned short': (16, False),
    'int': (32, True), 'unsigned int': (32, False),
    'long': (64, True), 'unsigned long': (64, False),
    'Py_ssize_t': (64, True), 'long long': (64, True),
    'unsigned long long': (64, False),
}


class PointerRules:

    def raw_var(self, st, e):
        while e[0] == 'cast':
            e = e[2]
        if e[0] != 'id':
            return None
        v = st.vars.get(e[1])
        if v is None or v.bt != 'rt' or v.ctype not in RAW_TYPES:
            return None
        return v

    def raw_step(self, st, out, name, op, e, frames):
        """'p += E' (or -=) with E known now."""
        v = st.vars.get(name)
        if op not in ('+=', '-=') or v is None or v.bt != 'rt' or v.ctype not in RAW_TYPES:
            return False
        val = self.ev(e, st, out, frames)
        if val.bt != 'ct':
            raise GA.ResumeHere('%s moved by a run-time amount' % name)
        v.raw_off = '%s %s %s' % (getattr(v, 'raw_off', None) or '0', op[0],
                                  GA.paren(val.c))
        return True

    def raw_load(self, e, st, out, owned=False, scalars=()):
        """'*(T *)p': the bytes at the pointer, as a vinfo."""
        if e[0] != 'un' or e[1] != '*' or e[2][0] != 'cast':
            return None
        v = self.raw_var(st, e[2][2])
        if v is None:
            return None
        ctype = GA.ctype_of(GA.ctokens.tokenize(e[2][1])[0])
        if not ctype.endswith('*'):
            return None
        t = ctype[:-1].strip()
        if t in INT_READS:
            bits, signed = INT_READS[t]
            arr = 'DEF_ARRAY' if signed else 'UNSIGNED_ARRAY'
            if t == 'char':
                self.tr.header_asserts.add(
                    '_Static_assert((char)-1 < 0, "a char member reads signed");')
        elif t.endswith('*') or t in scalars:
            # a pointer, or a C scalar (scalars.py: a word holding its bits,
            # only where the scalar rules read it)
            arr, bits, signed = 'DEF_ARRAY', None, None
        else:
            raise GA.ResumeHere('a read of a %s through %s' % (t, v.name))
        r = self.new_tmp('vinfo_t *')
        out('%s = psyco_get_field_offset(po, %s, FMUT(%s(%s, 0)), %s);'
            % (r, v.cname, arr, t, getattr(v, 'raw_off', None) or '0'))
        self.error_exit(st, out, '%s == NULL' % r)
        rng = None
        if bits is not None:
            lo = -(1 << (bits - 1)) if signed else 0
            hi = (1 << (bits - 1)) - 1 if signed else (1 << bits) - 1
            rng = (lo, hi, max(32, bits))
        if owned:
            val = GA.Val('rt', r, t, owned=True, conv={'checkable': False})
        else:
            val = self.statement_value(st, r, t)
            val.var.rng, val.var.noref = rng, self.s.is_obj(t)
        val.rng = rng
        val.noref = self.s.is_obj(t)
        return val

    def raw_value(self, e, st, out):
        """The pointer itself as a value: the object's address plus the
        offset."""
        v = self.raw_var(st, e)
        if v is None or e[0] == 'cast' and not e[1].replace(' ', '').endswith('*'):
            return None
        off = getattr(v, 'raw_off', None)
        if off is None:
            return None
        r = self.new_tmp('vinfo_t *')
        out('%s = integer_add_i(po, %s, %s, false);' % (r, v.cname, off))
        return self.statement_value(st, r, 'char *')

    def statement_value(self, st, r, ctype):
        """A vinfo the meta made for the statement being translated, held
        by a variable of its own and released after it (release_dead)."""
        self.tmp_reads = getattr(self, 'tmp_reads', 0) + 1
        var = GA.Var('$m%d' % self.tmp_reads, ctype, 'rt', r)
        var.owned = 'yes'
        st.vars[var.name] = var
        return GA.Val('rt', r, ctype, var=var, conv={'checkable': False})

    def singleton(self, e, st, out):
        """'x = Py_None' (Py_True, Py_False): Ni's constant, a new
        reference."""
        if e[0] != 'id' or e[1] not in SINGLETONS or e[1] in st.vars:
            return None
        r = self.new_tmp('vinfo_t *')
        out('%s = %s();' % (r, SINGLETONS[e[1]]))
        val = GA.Val('rt', r, 'PyObject *', owned=True)
        val.singleton = True
        return val

    def noref_incref(self, st, out, e):
        """Py_INCREF of a value read from memory: need_reference."""
        v = self.var_of(st, e)
        if not getattr(v, 'noref', False):
            return False
        if ('ref', 'need_reference') not in [tuple(i[:2]) for i in
                                            self.tr.policy_items(self.f.name)]:
            raise GA.Refuse("an incref of %s, read from memory, needs the policy's"
                            " ('ref', 'need_reference')" % v.name)
        out('need_reference(po, %s);' % v.cname)
        v.noref = False
        return True
