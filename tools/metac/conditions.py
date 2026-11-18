"""Rules of the general translator for bit-fields, run-time choices and two
policy items (D89; METACOMPILER.md "str and descriptors"). Mixed into
gen_abstract.Walk, which calls each hook with a line; each answers None
when the expression is not one it knows.

  - bit_test (rule bit_field): a one-bit field 'X->F.B' of an object, where
    policy_table.LAYOUTS lists F's bits and TYPES a descriptor for F, is
    F read through Ni's descriptor and tested; the bit at the top of the
    bytes Ni reads (LAYOUTS' read width) is 'F >= mask', unsigned, one
    compare. A branch on it needs a 'likely' item, written as the C wrote
    the test (the macro's name: 'PyUnicode_IS_READY').
  - choice (rule runtime_choice): 'c ? A : B' with c a run-time test
    takes the arm c's 'likely' item favours; on the other arm CPython's
    algorithm resumes from the statement (PyUnicode_READY's
    _PyUnicode_Ready runs only on a legacy string).
  - ('shortcut', 'const_fold'): when every object parameter is a
    constant, the meta calls CPython's function at compile time; an error
    is left to run time (D37: strings are immutable).
  - ('promote', 'known_only'): a parameter's type is the one Ni knows
    (Psyco_KnownType); where it knows none, CPython's function runs.
  - ('shortcut', 'skip_check', C): a test the caller has made already
    (pobject.c:generic_getattr calls a descriptor's __get__ only on an
    instance of its type) holds.
"""
import policy_table

GA = None       # gen_abstract, set by it


class ConditionRules:

    # -- bit-fields --

    def bit_test(self, e, st, out, frames):
        if e[0] != 'dot' or e[1][0] != 'arrow':
            return None
        base, field, bit = e[1][1], e[1][2], e[2]
        b = self.ev(base, st, out, frames)
        if b.bt != 'rt' or b.var is None or not self.s.is_obj(b.ctype):
            return None
        struct = b.ctype.replace('const ', '').rstrip(' *').replace('struct ', '').strip()
        lay = policy_table.LAYOUTS.get(struct, {})
        bits = lay.get('bits', {}).get(field)
        if not bits:
            return None
        read = dict((n, r) for n, _, r in lay.get('fields', ())).get(field)
        desc = GA.field_descriptor(b.ctype, field)
        if desc is None or read is None:
            raise GA.ResumeHere('the bit-field %s.%s has no descriptor' % (field, bit))
        pos = 0
        for spec in bits:
            name, width = spec.split(':')
            if name == bit:
                break
            pos += int(width)
        else:
            raise GA.ResumeHere('no bit %s in %s.%s' % (bit, struct, field))
        nbits = int(read.split('/')[0][1:])
        if int(width) != 1 or pos + 1 != nbits:
            raise GA.ResumeHere('the bit-field %s.%s is not the top bit Ni reads'
                                % (field, bit))
        how, text = self.bit_likely(e)
        fv = self.new_tmp('vinfo_t *')
        out('%s = psyco_get_field(po, %s, %s);' % (fv, b.c, desc))
        self.error_exit(st, out, '%s == NULL' % fv)
        cc = self.new_tmp('condition_code_t')
        out('%s = integer_cmp_i(po, %s, NI_BIT_%s__%s__%s, Py_GE | COMPARE_UNSIGNED);'
            % (cc, fv, struct, field, bit))
        out('vinfo_decref(%s, po);' % fv)
        self.error_exit(st, out, '%s == CC_ERROR' % cc)
        self.used_likely.add(text)
        t = self.new_tmp('int')
        out('%s = runtime_condition_%s(po, %s);' % (t, 't' if how == 'true' else 'f', cc))
        self.choice_likely()[t] = how == 'true'
        return t

    def bit_likely(self, e):
        """(how, text) of the 'likely' item for a bit test: its text is a
        header macro of one parameter whose expansion is e."""
        x = e[1][1]
        while x[0] == 'cast':
            x = x[2]
        for text, how in self.likely.items():
            m = self.s.macros.get(text)
            if m is None or m.params is None or len(m.params) != 1:
                continue
            try:
                if strip_casts(self.s.expand_call(text, [x])) == strip_casts(e):
                    return how, text
            except GA.Resume:
                continue
        raise GA.Refuse("no 'likely' item in the policy table for the run-time"
                        " test of the bit-field %s" % GA.cparse.c_expr(e))

    def likely_key(self, e):
        """The 'likely' item whose text, read as C, is the expression e."""
        want = GA.cparse.c_expr(e)
        for text in self.likely:
            try:
                x = GA.cparse.parse_expr(GA.ctokens.tokenize(text)[0], self.s.decls.types)
            except GA.cparse.ParseError:
                continue
            if GA.cparse.c_expr(x) == want:
                return text
        return None

    def choice_likely(self):
        if not hasattr(self, '_choice_likely'):
            self._choice_likely = {}
        return self._choice_likely

    # -- c ? A : B on a run-time test --

    def choice(self, e, c, st, out, frames):
        likely = self.choice_likely().get(c) if isinstance(c, str) else None
        if likely is None:
            return None
        out('if (%s%s) {' % ('!' if likely else '', c))
        sub = GA.Out(out.depth + 1)
        self.resume_inside(st.copy(), sub, frames,
                           'the unlikely arm of %s' % GA.cparse.c_expr(e)[:60])
        out.extend(sub)
        out('}')
        return self.ev(e[2] if likely else e[3], st, out, frames)

    # -- policy items --

    def const_fold(self, st, out):
        """('shortcut', 'const_fold'): CPython's function at compile time
        when every object parameter is a constant."""
        f = self.f
        objs = [p for (t, p), b in zip(f.params, self.sm.pbt)
                if b == 'rt' and self.s.is_obj(t)]
        if not objs or len(objs) != sum(1 for b in self.sm.pbt if b == 'rt'):
            raise GA.Refuse("'const_fold' on a function with non-object run-time"
                            " parameters")
        if f.static and f.name not in self.tr.slot_roots:
            raise GA.Refuse("'const_fold' on a static function that is not a slot")
        fn = GA.slot_ref(self.tr.slot_types[f.name], *self.tr.slot_roots[f.name]) \
            if f.static else f.name
        args = ', '.join('(PyObject *)CompileTime_Get(%s->source)->value' % p
                         if b == 'rt' else p
                         for (_, p), b in zip(f.params, self.sm.pbt))
        out('if (%s) {' % ' && '.join('is_compiletime(%s->source)' % p for p in objs))
        if self.s.is_obj(f.ret):
            out('  PyObject *r = %s(%s);' % (fn, args))
            out('  if (r != NULL)')
            out('    return vinfo_new(CompileTime_NewSk(sk_new((long)r, SkFlagPyObj)));')
        elif f.ret in GA.INT_TYPES:
            out('  %s r = %s(%s);' % (f.ret, fn, args))
            out('  if (r >= 0)')
            out('    return vinfo_new(CompileTime_New(r));')
        else:
            raise GA.Refuse("'const_fold' on a function returning %s" % f.ret)
        out('  PyErr_Clear(); /* raised again at run time */')
        out('}')

    def assumed(self, e):
        """('shortcut', 'skip_check', C): the caller has made the test C
        (casts aside), so it holds."""
        for item in self.tr.policy_items(self.f.name):
            if tuple(item[:2]) != ('shortcut', 'skip_check') or len(item) < 3:
                continue
            try:
                x = GA.cparse.parse_expr(GA.ctokens.tokenize(item[2])[0], self.s.decls.types)
            except GA.cparse.ParseError:
                continue
            if strip_casts(x) == strip_casts(e):
                return True
        return None

    def known_only(self):
        return ('promote', 'known_only') in [tuple(i[:2]) for i in
                                             self.tr.policy_items(self.f.name)]


def strip_casts(e):
    """e with casts and the release form's assert(...) left out, for
    comparing a macro's expansion with the body's."""
    if isinstance(e, tuple) and e:
        if e[0] == 'cast':
            return strip_casts(e[2])
        return tuple(strip_casts(x) if isinstance(x, (tuple, list)) else x for x in e)
    if isinstance(e, list):
        return [strip_casts(x) for x in e]
    return e
