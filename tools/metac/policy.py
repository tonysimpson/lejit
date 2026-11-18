#!/usr/bin/env python3
"""The policy table's vocabulary and its checks (GENERATION step 3, D71).

The table (policy_table.py) is the generator's judgement, as data: what to
promote, guard, make virtual, which shortcuts and likelihoods to use, what
the result type is, what Ni assumes about effects. It is keyed by CPython
definition names (csource.py: 'Objects/abstract.c:PyNumber_Add'), and each
keyed item carries the shape (token hash) of the definition it was written
for. check() runs on every build (generate.py) and fails on:
  - a key that names nothing in the source being built for;
  - a shape that differs: the C changed, so the entry must be read again
    against it, and its shape updated (several shapes may be listed, one
    for each source the entry has been checked against);
  - a kind or value outside VOCABULARY, or a field the section doesn't have;
  - a Ni name ('ni/...:name') that doesn't exist in Ni's source;
  - data that copies Ni's code (limits, the D53 list, the type whitelists)
    and no longer says what the code does: until a generator emits that
    code from the table, the two must agree.

  policy.py --src DIR           run the checks, print the problems
  policy.py --src DIR --shape KEY...   print each key's current shape
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import csource  # noqa: E402
import ctokens  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))

# kind -> {value: meaning}. A policy item is (kind, value) or (kind, value,
# note); the note is free text, the C it is about.
VOCABULARY = {
    'promote': {
        'type': "the operand's type is promoted before it is used (P01)",
        'verify_type': 'tested against one type; promoted only if unknown (P02)',
        'static_type': 'only a static type is promoted; a heap type stays run-time (F25)',
        'value': 'a C value that chooses the path is promoted (P03)',
        'constant': 'the value found becomes a compile-time constant, behind a guard',
        'known_only': 'uses types already known; never promotes',
    },
    'mega': {
        'type': 'type promotion with a megamorphic cut-off (P04, need_type_mega)',
        'value': 'value promotion with a megamorphic cut-off (P04)',
        'residual': 'a forgotten or megamorphic value takes the residual call (D43)',
    },
    'guard': {
        'version_tag': 'type version-tag guard, shared per (type, name, kind) (D30, D60)',
        'store_guard': 'store guard shared per (type, name, kind) (D39)',
        'dict_version': "dict ma_version_tag guard (PEP 509, D25)",
        'first_seen': 'the field first seen, held, compared at run time (D48, F52, F57)',
        'memory': 'a cached field compared with memory at run time (F48)',
        'identity': 'compared with an expected object at run time',
        'constant_class': "a class-assignable constant's type behind a check (D62)",
        'no_fold_heap': 'a heap type is never folded at compile time',
        'generic_for_heap': 'a heap-type case takes the generic path',
    },
    'virtual': {
        'result': 'the result is a virtual object (note: its type) (P18)',
        'argument': 'a virtual argument is unpacked by this consumer (P18a)',
        'fast_path': 'a virtual operand is read or written in its vinfo array (P19)',
        'concat': 'virtual operands give a virtual result',
        'fuse': 'a generator expression is fused with its consumer (D46)',
        'args_tuple': 'call arguments as a virtual tuple',
    },
    'shortcut': {
        'one_digit': 'one-digit ints as machine words',
        'const_fold': 'constant inputs computed at compile time (P14)',
        'type_whitelist': 'specialised only for the types named (P26)',
        'inline_index': 'an int index converted inline',
        'inline_read': 'a field read inline instead of a call',
        'no_tuple_call': 'arguments passed without a tuple (F13)',
        'known_length': 'specialised only for a tuple of known length (P22)',
        'identity': 'an operand returned as the result (P21)',
        'power_of_two': 'a constant power-of-two divisor as shift and mask',
        'small_presized': 'a small presized dict made as an empty one',
        'word_compare': 'a compare of words instead of a call',
        'skip_check': 'a check CPython makes is known to pass and is left out',
        'specialise_only': 'specialised only in the case named, else the generic call',
        'compile_time_unpack': 'a virtual sequence unpacked at compile time',
        'direct_meta': "a known type's meta called instead of the generic path",
        'if_known': 'an integer parameter used as its value when that is a'
                    ' constant, else CPython\'s function (D80)',
        'zero_or_one': 'the int result is 0 or 1 when it is not an error',
        'bool_word': "an argument known to be 0 or 1 is the bool's word"
                     " (note: Ni's function of that word)",
    },
    'likely': {
        'true': 'the condition in the note is expected true (runtime_condition_t)',
        'false': 'the condition in the note is expected false (runtime_condition_f)',
    },
    'restype': None,    # a type object of the source, checked by name
    'ref': {
        'need_reference': 'a borrowed value loaded becomes a reference compiled code owns',
        'held': 'held by the compiled code for good',
        'no_steal': 'the helper does not consume its arguments',
        'incref': 'the helper increfs what the C steals',
        'keep_alive': 'a reference kept for the duration of a call',
    },
    'forget': {
        'before_branch': 'known types forgotten before a branch to a merge point (D59)',
        'direct_call': 'before a direct call (D59)',
        'handler': 'after unwinding to a handler (D59)',
        'store': "an inline store whose old value's dealloc may run Python (F55)",
    },
    'limit': {
        'nested_weight': 'nested weight of virtual operands limited (psyco_forking)',
        'max_args': 'at most this many arguments passed one by one',
        'depth': 'nesting depth after which the real call is made',
        'max_frees': 'free-variable count after which the real call is made',
    },
    'other': {
        'compile_time_hash': 'a string hash computed at compile time',
        'ends_block': 'the block ends after the guard (split_after_global)',
        'untraced_only': 'done only while no trace function is set (D46)',
        'trace_fallback': 'while tracing, the real call (C23, D46)',
        'mutable_fields': 'every field read is mutable',
        'not_translation': "Ni's function of this name does another job (not generated)",
        'unpack_arguments': 'binds its outputs to the items of an argument array'
                            ' whose count is within its bounds, else fails'
                            ' (gen_builtins.py, D86)',
        'frame_binding': 'the argument binding, between making the frame and'
                         ' running it, is generated as a binder the compiler'
                         ' runs (gen_call.py, D85)',
    },
}

# extra kind: a shortcut with no C behind it in this source (the generator
# must emit it from the table, or same code shows a loss)
VOCABULARY['extra'] = VOCABULARY['shortcut']

EFFECTS = {'pure', 'no_python', 'blocks'}             # EXTERNAL values
ASSUME = {'no_python', 'no_block', 'pure', 'raises'}  # ASSUMPTIONS values
SEED_REASONS = {'decref', 'gc', 'blocks', 'sets_error', 'alloc', 'fatal'}


def table():
    import policy_table
    return policy_table


class Checker:
    def __init__(self, src, ni_root=ROOT):
        self.src = csource.Tree(src)
        self.ni = csource.Tree(ni_root)
        self.ni_root = ni_root
        self.problems = []

    def bad(self, where, what):
        self.problems.append('%s: %s' % (where, what))

    def shape(self, where, key, entry):
        d = self.src.find(key)
        if d is None:
            self.bad(where, 'not in the source')
            return None
        want = entry.get('shape')
        shapes = (want,) if isinstance(want, str) else tuple(want or ())
        if d.shape() not in shapes:
            self.bad(where, 'shape %s, the entry was written for %s: read the'
                     ' entry again against the C and update its shape'
                     % (d.shape(), ', '.join(shapes) or 'none'))
        return d

    def fields(self, where, entry, allowed):
        for f in entry:
            if f not in allowed:
                self.bad(where, 'unknown field %r' % f)

    def ni_name(self, where, name):
        if name.startswith('tools/'):
            path, _, n = name.partition(':')
            full = os.path.join(self.ni_root, path)
            text = open(full).read() if os.path.isfile(full) else ''
            if not re.search(r'^(def %s\b|%s = )' % (n, n), text, re.M):
                self.bad(where, '%s not found' % name)
        elif self.ni.find(name) is None:
            self.bad(where, '%s not in Ni' % name)

    def type_object(self, where, name):
        """A PyTypeObject variable of the source, by name."""
        if not hasattr(self, '_types'):
            self._types = set()
            for d in ('Objects', 'Python'):
                for fn in sorted(os.listdir(os.path.join(self.src.root, d))):
                    if fn.endswith('.c'):
                        toks = self.src.file('%s/%s' % (d, fn))[0]
                        for i, t in enumerate(toks[:-2]):
                            if t.text == 'PyTypeObject' and toks[i + 2].text == '=':
                                self._types.add(toks[i + 1].text)
        if name not in self._types:
            self.bad(where, 'no type object %s in the source' % name)

    # ---- sections ----

    def entries(self, t):
        for key, e in t.ENTRIES.items():
            self.fields(key, e, ('shape', 'ni', 'policy'))
            self.shape(key, key, e)
            for n in e.get('ni', ()):
                self.ni_name(key, n)
            for item in e.get('policy', ()):
                self.item(key, item)
                # a generated function's virtual result comes from TYPES'
                # constructors (D80): an item saying so would be a second
                # copy that could disagree
                if item[:2] == ('virtual', 'result') and any(
                        n.startswith('tools/') for n in e.get('ni', ())):
                    self.bad(key, "a generated function with a 'virtual result'"
                             " item: the constructor rule reads TYPES")

    def item(self, where, item):
        if not isinstance(item, tuple) or len(item) not in (2, 3):
            self.bad(where, 'item %r is not (kind, value[, note])' % (item,))
            return
        kind, value = item[0], item[1]
        if kind not in VOCABULARY:
            self.bad(where, 'unknown kind %r' % kind)
        elif kind == 'restype':
            self.type_object(where, value)
        elif value not in VOCABULARY[kind]:
            self.bad(where, 'unknown %s value %r' % (kind, value))

    def rules(self, t):
        for name, r in t.RULES.items():
            self.fields('RULES ' + name, r, ('what', 'decisions', 'applied_by', 'replaces'))
            for n in r.get('applied_by', ()):
                self.ni_name('RULES ' + name, n)

    def limits(self, t):
        for name, (path, value) in t.LIMITS.items():
            f = self.ni.file(path)
            d = f and f[1].get(name)
            d = d if d and d.kind == 'macro' else None
            body = d and [x.text for x in d.toks[1:]]
            if body != [str(value)]:
                self.bad('LIMITS ' + name, '%s defines it as %s, the table says %s'
                         % (path, ' '.join(body) if body else 'nothing', value))

    def light_opcodes(self, t):
        ops = self.src.file('Include/opcode.h')[1]
        for op in t.LIGHT_OPCODES:
            if op not in ops:
                self.bad('LIGHT_OPCODES', '%s is not an opcode of the source' % op)
        d = self.ni.find('ni/mergepoints.c:IS_LIGHT_INSTR')
        code = {x.text for x in d.toks[1:] if x.kind == 'id'} - {'op'} if d else set()
        if code != set(t.LIGHT_OPCODES):
            self.bad('LIGHT_OPCODES', 'mergepoints.c:IS_LIGHT_INSTR lists %s'
                     % ' '.join(sorted(code ^ set(t.LIGHT_OPCODES))) + ' differently')

    def types(self, t):
        for key, e in t.TYPES.items():
            where = 'TYPES ' + key
            self.fields(where, e, ('shape', 'type', 'mutable', 'immutable',
                                   'virtual', 'slot_result', 'ni', 'known_length',
                                   'descriptors', 'word', 'double'))
            self.descriptors(where, e)
            kl = e.get('known_length')
            if kl is not None:
                self.fields(where + ' known_length', kl, ('size', 'items', 'load', 'item'))
                for f in (kl.get('size'), kl.get('items')):
                    if f not in e.get('immutable', ()):
                        self.bad(where, 'known_length reads %s, which is not immutable' % f)
                for n in (kl.get('load'), kl.get('item')):
                    self.ni_name(where, n)
            d = self.shape(where, key, e)
            if 'type' in e:
                self.type_object(where, e['type'])
            if 'slot_result' in e:
                self.type_object(where, e['slot_result'])
            names = {x.text for x in d.toks} if d else set()
            if {'PyObject_VAR_HEAD'} & names:
                names |= {'ob_size', 'ob_type', 'ob_refcnt'}
            elif {'PyObject_HEAD'} & names:
                names |= {'ob_type', 'ob_refcnt'}
            for f in list(e.get('mutable', ())) + list(e.get('immutable', ())):
                if d and f not in names:
                    self.bad(where, 'no field %s' % f)
            for n in e.get('ni', ()):
                self.ni_name(where, n)
            if 'virtual' in e:
                self.virtual(where, e['virtual'], bool(e.get('mutable')))
            if 'word' in e:
                self.word(where, e['word'])
            if 'double' in e:
                self.double(where, e['double'], names if d else None)

    def word(self, where, w):
        """A word representation (D83): its Ni functions are declared and
        its constructors are in the source."""
        where += ' word'
        self.fields(where, w, ('test', 'value', 'value_ctype', 'bound', 'load',
                               'sign', 'sign_load', 'constructors', 'new'))
        for k in ('test', 'value', 'value_ctype', 'bound', 'load', 'constructors', 'new'):
            if k not in w:
                self.bad(where, 'no %r' % k)
        for k in ('load', 'sign_load', 'new'):
            if k in w:
                path, _, name = w[k].partition(':')
                f = self.ni.file(path)
                if f is None or not any(t.text == name for t in f[0]):
                    self.bad(where, '%s does not declare %s' % (path, name))
        for c in w.get('constructors', ()):
            if self.src.find(c) is None:
                self.bad(where, 'no constructor %s in the source' % c)

    def double(self, where, w, fields):
        """A scalar representation (D87): its field is the struct's, its Ni
        functions are declared, its constructors are in the source."""
        where += ' double'
        self.fields(where, w, ('field', 'value_ctype', 'load', 'constructors', 'new'))
        for k in ('field', 'value_ctype', 'load', 'constructors', 'new'):
            if k not in w:
                self.bad(where, 'no %r' % k)
        if fields is not None and w.get('field') not in fields:
            self.bad(where, 'no field %s' % w.get('field'))
        for k in ('load', 'new'):
            if k in w:
                path, _, name = w[k].partition(':')
                f = self.ni.file(path)
                if f is None or not any(t.text == name for t in f[0]):
                    self.bad(where, '%s does not declare %s' % (path, name))
        for c in w.get('constructors', ()):
            if self.src.find(c) is None:
                self.bad(where, 'no constructor %s in the source' % c)

    def descriptors(self, where, e):
        """Each descriptor is a #define of Ni's for a field the entry lists,
        with FMUT in its definition exactly when the field is mutable."""
        for field, name in e.get('descriptors', {}).items():
            path, _, n = name.partition(':')
            full = os.path.join(self.ni_root, path)
            text = open(full).read() if os.path.isfile(full) else ''
            m = re.search(r'^#define %s\s+(.*)$' % n, text, re.M)
            if m is None:
                self.bad(where, '%s does not define %s' % (path, n))
                continue
            mutable = field in e.get('mutable', ())
            if not mutable and field not in e.get('immutable', ()):
                self.bad(where, 'a descriptor for %s, not a listed field' % field)
            if ('FMUT' in m.group(1)) != mutable:
                self.bad(where, '%s: FMUT and the field being %s disagree'
                         % (name, 'mutable' if mutable else 'immutable'))

    def virtual(self, where, v, mutable=False):
        """The weights as INIT_SVIRTUAL(_NOCALL) in Ni registers them; for
        a type gen_virtual.py generates (a constructor, no mutable field),
        the constructor exists and Ni declares the virtual one."""
        self.fields(where + ' virtual', v, ('ni', 'weight', 'weight_at_calls',
                                             'constructor', 'new', 'max_items',
                                             'forced'))
        if 'constructor' in v:
            if self.src.find(v['constructor']) is None:
                self.bad(where, 'no constructor %s in the source' % v['constructor'])
            # declared in Ni's header, defined by gen_virtual.py (or by Ni
            # for a mutable type)
            path, _, name = v.get('new', ':').partition(':')
            f = self.ni.file(path)
            if f is None or not any(t.text == name for t in f[0]):
                self.bad(where, '%s does not declare %s' % (path, name))
        if v.get('forced') not in (None, 'drop_fields'):
            self.bad(where, "'forced' is %r, not 'drop_fields'" % v['forced'])
        if 'max_items' in v and v['max_items'] not in table().LIMITS:
            self.bad(where, 'max_items %s is not in LIMITS' % v['max_items'])
        if 'constructor' in v and not mutable:
            # gen_virtual.py registers the weights itself
            path, _, sv = v['ni'].partition(':')
            if self.ni.file(path) is None:
                self.bad(where, 'no file %s' % path)
            return
        path, _, sv = v['ni'].partition(':')
        f = self.ni.file(path)
        toks = f[0] if f else []
        for i, x in enumerate(toks):
            if (x.text in ('INIT_SVIRTUAL', 'INIT_SVIRTUAL_NOCALL')
                    and toks[i + 2].text == sv):
                args = [' '.join(y.text for y in a) for a in
                        ctokens.split_top(toks[i + 2:ctokens.match_close(toks, i + 1)], ',')]
                if x.text == 'INIT_SVIRTUAL_NOCALL':
                    got = (args[2], 'NW_FORCE')
                else:
                    got = (args[4], args[5])
                want = (str(v['weight']), str(v['weight_at_calls']))
                if got != want:
                    self.bad(where, '%s registers weights %s, the table says %s'
                             % (v['ni'], got, want))
                return
        self.bad(where, 'no INIT_SVIRTUAL for %s' % v['ni'])

    def cannot_block(self, t):
        listed = set()
        for key, e in t.CANNOT_BLOCK.items():
            where = 'CANNOT_BLOCK ' + key
            self.fields(where, e, ('shape', 'ni'))
            self.shape(where, key, e)
            listed.add(e['ni'])
            if e['ni'][0] in ('method', 'builtin'):
                self.method_table(where, key, e['ni'][-1])
        code = self.cannot_block_code()
        for x in sorted(code - listed, key=str):
            self.bad('CANNOT_BLOCK', 'psyfunc.c:cannot_block_init lists %s, the table does not' % (x,))
        for x in sorted(listed - code, key=str):
            self.bad('CANNOT_BLOCK', 'the table lists %s, psyfunc.c:cannot_block_init does not' % (x,))

    def method_table(self, where, key, pyname):
        """The method's PyMethodDef entry, in the key's file or its clinic
        header, names the key's function."""
        path, _, fn = key.partition(':')
        base = os.path.basename(path)
        for p in (path, os.path.join(os.path.dirname(path), 'clinic', base + '.h')):
            f = self.src.file(p)
            toks = f[0] if f else []
            for d in (f[1].values() if f else ()):
                if d.kind == 'macro':
                    toks = toks + d.toks
            for i, x in enumerate(toks):
                if x.text == '"%s"' % pyname and toks[i - 1].text == '{':
                    # the entry's function: the last name before the next
                    # ',' (after casts: '(PyCFunction)(void(*)(void))f' in
                    # 3.8's clinic headers)
                    end = next((j for j in range(i + 2, len(toks))
                                if toks[j].text in (',', '}')), len(toks))
                    names = [y.text for y in toks[i + 2:end] if y.kind == 'id']
                    if names and names[-1] == fn:
                        return
        self.bad(where, 'no method table entry "%s" for %s' % (pyname, fn))

    def cannot_block_code(self):
        d = self.ni.find('ni/psyfunc.c:cannot_block_init')
        out = set()
        if d is None:
            return out
        toks = d.body
        array = []          # the one array of method names, added in a loop
        for i, x in enumerate(toks):
            if x.text == '{' and toks[i - 1].text == '=':
                array = [y.text.strip('"') for y in
                         toks[i:ctokens.match_close(toks, i)] if y.kind == 'str']
        for i, x in enumerate(toks):
            # add_cannot_block(F); add_method(&T, "m"); add_builtin(b, "f")
            if x.text == 'add_cannot_block':
                close = ctokens.match_close(toks, i + 1)
                out.add(('c', ''.join(y.text for y in toks[i + 2:close])))
            elif x.text == 'add_method' and toks[i + 5].kind == 'str':
                out.add(('method', toks[i + 3].text, toks[i + 5].text.strip('"')))
            elif x.text == 'add_method':
                out |= {('method', toks[i + 3].text, m) for m in array}
            elif x.text == 'add_builtin':
                out.add(('builtin', toks[i + 4].text.strip('"')))
        return out

    def type_list(self, name, keys, copies):
        """A list of type objects that Ni's code also lists. Each copy is a
        list of 'ni/...:function' whose 'X == &T' tests together must name
        the same types."""
        aliases = {'NiCompatStr_Type': 'PyUnicode_Type'}
        for k in keys:
            self.type_object(name, k)
        for copy in copies:
            got = set()
            for n in copy:
                d = self.ni.find(n)
                if d is None:
                    self.bad(name, '%s not in Ni' % n)
                    continue
                w = [x.text for x in d.toks]
                for i in range(len(w) - 2):
                    if w[i] == '==' and w[i + 1] == '&':
                        got.add(aliases.get(w[i + 2], w[i + 2]))
                    elif w[i:i + 4] == ['==', 'Py_None', '->', 'ob_type']:
                        got.add('_PyNone_Type')
            if got != set(keys):
                self.bad(name, '%s test %s' % (' and '.join(copy), ' '.join(
                    sorted(got ^ set(keys)))) + ' differently from the table')

    def effects(self, t):
        for key, e in t.SEEDS.items():
            where = 'SEEDS ' + key
            self.fields(where, e, ('shape', 'reason'))
            self.shape(where, key, e)
            if e.get('reason') not in SEED_REASONS:
                self.bad(where, 'unknown reason %r' % e.get('reason'))
        for name, eff in t.EXTERNAL.items():
            if eff not in EFFECTS:
                self.bad('EXTERNAL ' + name, 'unknown effect %r' % eff)
        for key, e in t.ASSUMPTIONS.items():
            where = 'ASSUMPTIONS ' + key
            self.fields(where, e, ('shape', 'assume', 'why'))
            self.shape(where, key, e)
            for a in e.get('assume', ()):
                if a not in ASSUME:
                    self.bad(where, 'unknown assumption %r' % a)

    def slot_args(self, t):
        """SLOT_ARGS copies how Ni calls a slot's meta: each function it
        names must pass that format to Psyco_META*."""
        for slot, spec in getattr(t, 'SLOT_ARGS', {}).items():
            where = 'SLOT_ARGS ' + slot
            self.fields(where, spec, ('format', 'applied_by'))
            if set(spec['format']) - set('vl'):
                self.bad(where, 'format %r is not made of v and l' % spec['format'])
            for n in spec.get('applied_by', ()):
                self.ni_name(where, n)
                d = self.ni.find(n)
                if d is not None and '"%s"' % spec['format'] not in [x.text for x in d.toks]:
                    self.bad(where, '%s does not call with "%s"' % (n, spec['format']))

    def builtins(self, t):
        """TRANSLATED_FUNCTIONS names files of the source; METH_ARGS'
        keys are ml_flags, its parts a closed set, and each function it
        names is Ni's caller of the metas it describes."""
        for path, names in getattr(t, 'TRANSLATED_FUNCTIONS', {}).items():
            where = 'TRANSLATED_FUNCTIONS ' + path
            if self.src.file(path) is None:
                self.bad(where, 'not in the source')
            if len(set(names)) != len(names):
                self.bad(where, 'a name listed twice')
        for key, spec in getattr(t, 'METH_ARGS', {}).items():
            where = 'METH_ARGS ' + key
            self.fields(where, spec, ('c', 'fallback', 'applied_by'))
            for flag in key.split('|'):
                if not re.match(r'^METH_[A-Z]+$', flag.strip()):
                    self.bad(where, '%r is not an ml_flags name' % flag)
            for part in spec.get('c', ()):
                if part not in ('self', 'arg', 'array', 'count'):
                    self.bad(where, 'unknown part %r' % part)
            for n in spec.get('applied_by', ()):
                self.ni_name(where, n)
                d = self.ni.find(n)
                if d is not None and key.split('|')[0].strip() not in [x.text for x in d.toks]:
                    self.bad(where, '%s does not test %s' % (n, key))

    def run(self, t):
        self.entries(t)
        self.rules(t)
        self.limits(t)
        self.light_opcodes(t)
        self.types(t)
        self.cannot_block(t)
        for path, specs in t.TRANSLATED_SLOTS.items():
            where = 'TRANSLATED_SLOTS ' + path
            if self.src.file(path) is None:
                self.bad(where, 'not in the source')
            # one spec, or one per type the file defines
            for spec in specs if isinstance(specs, list) else [specs]:
                self.fields(where, spec, ('type', 'slots'))
                self.type_object(where, spec['type'])
        for r in t.SCALAR_CODE['raises']:
            if self.src.find(r) is None:
                self.bad('SCALAR_CODE', 'no function %s in the source' % r)
        self.slot_args(t)
        self.builtins(t)
        self.type_list('NO_DEL_TYPES', t.NO_DEL_TYPES,
                       [['ni/Objects/pobject.c:collect_undeletable_vars'],
                        ['ni/x64/ipyencoding.h:decref_may_run_python']])
        self.type_list('SAFE_DESCRIPTORS', t.SAFE_DESCRIPTORS,
                       [['ni/Objects/pobject.c:c_descriptor',
                         'ni/Objects/pobject.c:safe_descriptor']])
        self.effects(t)
        return self.problems


def check(src, ni_root=ROOT):
    """Problems with the table against the source tree src: [] if none."""
    return Checker(src, ni_root).run(table())


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', required=True)
    ap.add_argument('--shape', nargs='*')
    args = ap.parse_args(argv)
    if args.shape is not None:
        tree = csource.Tree(args.src)
        for k in args.shape:
            d = tree.find(k)
            print('%s %s' % (d.shape() if d else 'missing', k))
        return 0
    problems = check(args.src)
    for p in problems:
        print(p)
    t = table()
    print('%d entries (%d with policy items), %d rules, %d problems'
          % (len(t.ENTRIES), sum(1 for e in t.ENTRIES.values() if e.get('policy')),
             len(t.RULES), len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
