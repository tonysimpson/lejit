"""The policy table: the generator's judgement, as data (GENERATION step 3,
DECISIONS D71). Vocabulary and checks: policy.py; doc/METACOMPILER.md
"Policy table".

Keys name definitions in CPython's source (csource.py): 'path:name', path
relative to the source tree. 'shape' is the token hash of the definition
the entry was written for (a tuple when it has been checked against more
than one source); 'ni' names the hand-written code that carries the item
today. Nothing in the build reads the entries yet except policy.py's checks;
the generators of GENERATION steps 4 to 8 will.

What is a general rule is in RULES, not repeated per function: the D54 slot
guard, the restart rule, error branches unlikely, effects, forgetting,
megamorphic sites, the eval-breaker test. INVENTORY's DRIFT items are not
here: they are differences a generated function loses.
"""

# ---- general rules ----------------------------------------------------------

RULES = {
    'slot_guard': {
        'what': "Every slot read on a type that may be a heap type goes through"
                " Psyco_TypeSlot: a static type's slot is read at compile time, a"
                " heap type's first value is held and compared at run time. Where"
                " it changed, the code resumes CPython's algorithm at that point:"
                " the whole function if no slot has run yet, else a run-time C copy"
                " of the rest (gen_abstract.py's cimpl_ copies). PyType_IsSubtype on a heap type holds tp_mro"
                " the same way.",
        'decisions': ['D48', 'D54', 'P29', 'F56'],
        'replaces': "INVENTORY's GUARD items on slot reads: 22 in translated and"
                    " mixed rows, and the slot macros of the core rows",
        'applied_by': ['ni/Objects/pobject.h:Psyco_TypeSlot',
                       'ni/Objects/pobject.c:Psyco_HeapSlot',
                       'ni/Objects/pobject.c:Psyco_TypeIsSubtype',
                       'tools/metac/gen_abstract.py:generate'],
    },
    'restart': {
        'what': "Every promotion an instruction or meta needs comes before its"
                " first residual call. Only values in the frame can be promoted,"
                " so an intermediate of unknown type goes to the residual CPython"
                " function (F24); a value that may be a heap object is promoted as"
                " 'static_type' (F25).",
        'decisions': ['PATTERNS rule 1', 'F24', 'F25'],
        'replaces': '2 PROMOTE and OTHER items (F24)',
        'applied_by': ['tools/metac/gen_ceval.py:translate'],
    },
    'error_unlikely': {
        'what': "A run-time branch that leads only to raising an exception is"
                " unlikely; entries give the likelihood of other branches only.",
        'decisions': ['P05'],
        'replaces': '7 LIKELY items',
    },
    'effects': {
        'what': "A residual call's flags (return type, error convention, CfNewRef,"
                " CfPure, CfNoPython, CfNoBlock) come from the effect rules:"
                " effects.py's call graph over the source, with SEEDS, EXTERNAL"
                " and ASSUMPTIONS below. They are not written per call site.",
        'decisions': ['D71', 'F34', 'D53'],
        'replaces': "INVENTORY's EFFECT items (48 in translated and mixed rows,"
                    " 72 in all)",
        'applied_by': ['tools/metac/effects.py:analyse'],
    },
    'forget': {
        'what': "Known heap types and ModuleType subtypes are forgotten after"
                " anything that may run Python: a run-time call that is not CfPure"
                " or CfNoPython (rule effects), a direct compiled call, an inline"
                " store, a decref of a value whose type is not in NO_DEL_TYPES; at"
                " the end of the instruction and before a branch to a merge point,"
                " a direct call's frame, an inlined frame and a handler. A"
                " forgotten type is learnt again by a guard (D63); a"
                " class-assignable constant's type is behind a check (D62).",
        'decisions': ['D59', 'D62', 'D63', 'F55', 'F96'],
        'replaces': "INVENTORY's FORGET items in core rows; the sites of the"
                    " others are 'forget' items",
        'applied_by': ['ni/Objects/pobject.c:Psyco_ForgetClasses',
                       'ni/codegen.c:psyco_generic_call'],
    },
    'megamorphic': {
        'what': "A promotion site keeps up to LIMITS MEGAMORPHIC_MAX values;"
                " past that the value is megamorphic and takes the residual call"
                " ('mega' items say which sites cut off).",
        'decisions': ['P04', 'D42', 'D43', 'D66'],
        'applied_by': ['ni/Python/pycompiler.c:psyco_pyobj_atcompiletime_mega',
                       'ni/Objects/pobject.c:need_type_mega'],
    },
    'eval_breaker': {
        'what': "Compiled code tests eval_breaker at every backward-jump target"
                " and at the end of a CALL_* instruction that emitted a run-time"
                " call that may block: not CfNoBlock and not in CANNOT_BLOCK.",
        'decisions': ['D45', 'D53'],
        'applied_by': ['ni/Python/pycompiler.c:breaker_after_call',
                       'ni/psyfunc.c:psyco_cannot_block'],
    },
    'type_test_generic_path': {
        'what': "In a ceval case, 'if (T) A else B' where T only tests types keeps"
                " B, the generic path: Psyco's counterpart of B dispatches on the"
                " type itself, and A is CPython's fast path for exact built-in"
                " types.",
        'decisions': ['D69'],
        'applied_by': ['tools/metac/ceval_reader.py:_is_type_test'],
    },
    'singleton_test_generic_path': {
        'what': "In a ceval case, 'if (x == K) { A; DISPATCH(); }' for K Py_True,"
                " Py_False or Py_None is dropped when the rest of the case calls a"
                " function on x whose body answers K first ('if (v == K) return"
                " r;', PyObject_IsTrue) and, with that r, does A (checked"
                " statement by statement): the fast path gives the generic path's"
                " result. Otherwise the case is refused.",
        'decisions': ['D79'],
        'applied_by': ['tools/metac/ceval_reader.py:_answer'],
    },
    'bool_select': {
        'what': "A run-time branch whose arms only make one output Py_True and"
                " Py_False is that object chosen by the condition: a virtual bool"
                " of the condition code (PATTERNS P23), never a branch.",
        'decisions': ['D79'],
        'applied_by': ['tools/metac/gen_ceval.py:translate'],
    },
    'gc_ignored': {
        'what': "What a garbage collection may run (finalizers, weakref"
                " callbacks), reachable from any allocation, is not counted as"
                " running Python or blocking. Ni's flags have always assumed it.",
        'decisions': ['D71'],
        'applied_by': ['tools/metac/effects.py:analyse'],
    },
    'allocator_pure': {
        'what': "An object constructor called with constant arguments may be"
                " called at compile time (CfPure): every run then gets one shared"
                " object, which only identity can tell apart.",
        'decisions': ['D71', 'D64 (C17)'],
    },
    'loops': {
        'what': "A C loop with a compile-time bound (a 'for' counting a variable by"
                " one to a bound the body doesn't change, the bound known when the"
                " meta runs) is a loop of the meta: unrolled at compile time, its"
                " body emitted once per turn. A bound that is the length of an"
                " object whose type has a 'known_length' in TYPES is asked of Ni's"
                " representation (PsycoTuple_Load: a virtual tuple, or a constant"
                " one within CT_TUPLE_LOAD_SIZE_LIMIT); unknown, the meta resumes"
                " CPython's function. Any other loop is CPython's: the meta resumes"
                " at it, or, where every path from a point with nothing called"
                " yet runs such a loop or fails, calls the function from the start."
                " Promotions a loop's body needs after its first call are made at"
                " the loop's entry when nothing was called before it (rule 1).",
        'decisions': ['D78', 'D74', 'PATTERNS rule 1'],
        'applied_by': ['tools/metac/gen_abstract.py:generate'],
    },
    'virtual_types': {
        'what': "What is virtual, its weights and its fields' mutability are per"
                " type (TYPES), not per function; 'virtual' items say where a"
                " function makes or consumes one.",
        'decisions': ['P15', 'P18'],
    },
}

# The slots whose functions the general translator generates, per C file
# besides abstract.c (rule loops, D78): the header registers each meta
# (gen_<file>_init). Ni's choice of what to translate, as Ni's declarations
# are for abstract.c.
TRANSLATED_SLOTS = {
    'Objects/tupleobject.c': {'type': 'PyTuple_Type', 'slots': ['sq_contains']},
}

# ---- limits: numbers in Ni's code, checked to be the same -------------------

LIMITS = {
    'MEGAMORPHIC_MAX': ('ni/codegen.h', 5),
    'MP_BLOCK_CAP': ('ni/vcompiler.c', 6),
    'MAX_MP_DISTANCE': ('ni/mergepoints.c', 170),
    'INLINE_MAXIMUM_WEIGHT': ('ni/mergepoints.c', 6),
    'VLIST_LENGTH_MAX': ('ni/Objects/plistobject.h', 3),
    'CT_TUPLE_LOAD_SIZE_LIMIT': ('ni/Objects/ptupleobject.c', 15),
    'FAST_MAX': ('ni/Objects/pmethodobject.c', 5),
    'MAX_CALL_FREES': ('ni/psyfunc.c', 64),
    'STORE_CACHE_MAX_REFRESHES': ('ni/Objects/pobject.c', 64),
}

# opcodes that don't count towards INLINE_MAXIMUM_WEIGHT (upstream's rule)
LIGHT_OPCODES = [
    'NOP', 'POP_TOP', 'ROT_TWO', 'ROT_THREE', 'DUP_TOP', 'DUP_TOP_TWO',
    'LOAD_CONST', 'LOAD_FAST', 'STORE_FAST', 'DELETE_FAST', 'UNPACK_SEQUENCE',
    'BUILD_TUPLE', 'BUILD_LIST', 'BUILD_MAP', 'RETURN_VALUE', 'JUMP_FORWARD',
    'JUMP_ABSOLUTE', 'POP_JUMP_IF_FALSE', 'POP_JUMP_IF_TRUE',
    'JUMP_IF_FALSE_OR_POP', 'JUMP_IF_TRUE_OR_POP', 'EXTENDED_ARG',
]

# ---- representation per type (GENERATION step 6) ----------------------------
# Fields Ni treats as mutable or not (P15: FMUT, FIX_size/VAR_size) and the
# virtual form: its weight normally and at calls (NW_FORCE: forced at every
# call), as ni (its source_virtual_t) registers it.

TYPES = {
    'Include/tupleobject.h:PyTupleObject': {
        'shape': 'ecfd13dd0064', 'type': 'PyTuple_Type', 'immutable': ['ob_size', 'ob_item'],
        # a loop over ob_item to ob_size unrolls when Ni knows the length
        # (rule loops): PsycoTuple_Load gives it or -1, and fills the items
        'known_length': {'size': 'ob_size', 'items': 'ob_item',
                         'load': 'ni/Objects/ptupleobject.c:PsycoTuple_Load',
                         'item': 'ni/Objects/ptupleobject.h:PsycoTuple_GET_ITEM'},
        'virtual': {'ni': 'ni/Objects/ptupleobject.c:psyco_computed_tuple',
                    'weight': 'NW_TUPLES_NORMAL', 'weight_at_calls': 'NW_TUPLES_FUNCALL',
                    # D80: the C constructor a virtual stands for, and Ni's
                    # virtual constructor; gen_virtual.py writes both, the
                    # force function and the field descriptors
                    'constructor': 'Objects/tupleobject.c:PyTuple_New',
                    'new': 'ni/Objects/ptupleobject.h:PsycoTuple_New',
                    # a tuple whose size a meta computes is virtual up to the
                    # size Ni loads from a constant one (gen_abstract.py)
                    'max_items': 'CT_TUPLE_LOAD_SIZE_LIMIT'}},
    'Include/listobject.h:PyListObject': {
        'shape': '1e17e786b756', 'type': 'PyList_Type', 'mutable': ['ob_size', 'ob_item'],
        # mutable: only its construction is the rule's (BUILD_LIST, D73),
        # its force function and fields are hand-written
        'virtual': {'ni': 'ni/Objects/plistobject.c:psyco_computed_vlist',
                    'weight': 'NW_VLISTS', 'weight_at_calls': 'NW_FORCE',
                    'constructor': 'Objects/listobject.c:PyList_New',
                    'new': 'ni/Objects/plistobject.h:PsycoList_New'}},
    'Include/longintrepr.h:_longobject': {
        'shape': '8ccf94d3f22e', 'type': 'PyLong_Type', 'immutable': ['ob_size', 'ob_digit'],
        # the residual call of an int slot on two ints gives an int
        # (plong_fallback1/2, divmod_fallback)
        'slot_result': 'PyLong_Type',
        'virtual': {'ni': 'ni/Objects/plongobject.c:psyco_computed_long',
                    'weight': 0, 'weight_at_calls': 0}},
    'Include/floatobject.h:PyFloatObject': {
        'shape': '1458f11765b5', 'type': 'PyFloat_Type', 'immutable': ['ob_fval'],
        'virtual': {'ni': 'ni/Objects/pfloatobject.c:psyco_computed_float',
                    'weight': 0, 'weight_at_calls': 0}},
    'Objects/iterobject.c:seqiterobject': {
        # it_seq: cached, and compared with memory for a run-time iterator (F48)
        'shape': '24ea9b846c56', 'type': 'PySeqIter_Type', 'mutable': ['it_index'],
        'immutable': ['it_seq'],
        'virtual': {'ni': 'ni/Objects/piterobject.c:psyco_computed_seqiter',
                    'weight': 1, 'weight_at_calls': 'NW_FORCE'}},
    'Objects/rangeobject.c:rangeobject': {
        'shape': '1534db80b98a', 'type': 'PyRange_Type',
        'immutable': ['start', 'stop', 'step', 'length'],
        'virtual': {'ni': 'ni/Objects/prangeobject.c:psyco_computed_range',
                    'weight': 4, 'weight_at_calls': 'NW_FORCE'}},
    'Objects/rangeobject.c:rangeiterobject': {
        'shape': 'd9be9f22f87f', 'type': 'PyRangeIter_Type', 'mutable': ['index'],
        'immutable': ['start', 'step', 'len'],
        'virtual': {'ni': 'ni/Objects/prangeobject.c:psyco_computed_rangeiter',
                    'weight': 1, 'weight_at_calls': 'NW_FORCE'}},
    'Include/sliceobject.h:PySliceObject': {
        'shape': '9fa2823d7f91', 'type': 'PySlice_Type', 'immutable': ['start', 'stop', 'step'],
        'virtual': {'ni': 'ni/Objects/psliceobject.c:psyco_computed_slice',
                    'weight': 4, 'weight_at_calls': 'NW_FORCE',
                    'constructor': 'Objects/sliceobject.c:PySlice_New',
                    'new': 'ni/Objects/psliceobject.h:PsycoSlice_New',
                    # once real, its bounds are dropped from the vinfo, not
                    # kept as known fields (D56)
                    'forced': 'drop_fields'}},
    'Include/funcobject.h:PyFunctionObject': {
        # func_code, func_defaults: held as first seen and guarded (F52, F57)
        'shape': 'b4e965472ecd', 'type': 'PyFunction_Type',
        'mutable': ['func_code', 'func_defaults'],
        'virtual': {'ni': 'ni/Objects/pfuncobject.c:psyco_computed_function',
                    'weight': 1, 'weight_at_calls': 'NW_FORCE'}},
    'Include/methodobject.h:PyCFunctionObject': {
        'shape': 'a11b06e946e1', 'type': 'PyCFunction_Type', 'immutable': ['m_ml', 'm_self'],
        'virtual': {'ni': 'ni/Objects/pmethodobject.c:psyco_computed_cfunction',
                    'weight': 1, 'weight_at_calls': 1}},
    'Include/classobject.h:PyMethodObject': {
        'shape': '87c9708df70e', 'type': 'PyMethod_Type', 'immutable': ['im_func', 'im_self'],
        'virtual': {'ni': 'ni/Objects/pclassobject.c:psyco_computed_method',
                    'weight': 0, 'weight_at_calls': 0,
                    'constructor': 'Objects/classobject.c:PyMethod_New',
                    'new': 'ni/Objects/pclassobject.h:PsycoMethod_New'}},
    'Include/cellobject.h:PyCellObject': {
        # a cell can change under any call
        'shape': '666fb76e2c4d', 'type': 'PyCell_Type', 'mutable': ['ob_ref']},
}

# ---- layouts: the CPython fields Ni relies on (stage 0, D19, D76) ----------

# Every struct field whose offset, size or representation Ni builds into
# emitted code or a field descriptor. gen_layouts.py finds each struct by
# name in Include/**/*.h (then in 'from', for a struct private to a C file,
# which it copies under the name 'copy'), checks each field is declared
# with the C type given, and writes layouts_gen.h: NI_OFS_, NI_SIZE2_ and
# NI_UNSIGNED_<struct>__<field>, with a _Static_assert per assumption.
# Fields are (path, declared C type, how Ni reads it): the read is ptr, f64,
# i8/u8 ... i64/u64, with '/low' when Ni reads only the first bytes of a
# wider field and '/bits' when it reads the bytes whatever the declared
# kind or sign. 'bits': bit-fields Ni tests by mask, from the first.
# Ni's own fields (virtual-object slots, guards) stay in core.
LAYOUTS = {
    # refcounts and types: the backend (x64/ipyencoding.*) and OB_type
    'PyObject': {'fields': [('ob_refcnt', 'Py_ssize_t', 'i64'),
                            ('ob_type', 'struct _typeobject *', 'ptr')]},
    'PyVarObject': {'fields': [('ob_size', 'Py_ssize_t', 'i64')]},
    # the backend: decref's dealloc call, type guards, the inline store's
    # GC test (a byte of tp_flags)
    'PyTypeObject': {'fields': [('tp_dealloc', 'destructor', 'ptr'),
                                ('tp_flags', 'unsigned long', 'u64'),
                                ('tp_version_tag', 'unsigned int', 'u32')]},
    # (PyGC_Head's gc_refs, the inline store's GC test: the store_gc
    # mechanism, ni/mechanisms/store_gc/, D77)
    'PyFloatObject': {'fields': [('ob_fval', 'double', 'f64')]},
    'PyLongObject': {'fields': [('ob_digit', 'digit[1]', 'u32')]},
    'PyListObject': {'fields': [('ob_item', 'PyObject **', 'ptr')]},
    'PyTupleObject': {'fields': [('ob_item', 'PyObject *[1]', 'ptr')]},
    'PyBytesObject': {'fields': [('ob_sval', 'char[1]', 'u8/bits')]},
    # len() reads ma_used's low 32 bits (pdictobject.c); the inline store
    # compares and increments all 64 (ipyencoding.c)
    'PyDictObject': {'fields': [('ma_used', 'Py_ssize_t', 'i32/low'),
                                ('ma_version_tag', 'uint64_t', 'u64'),
                                ('ma_keys', 'PyDictKeysObject *', 'ptr'),
                                ('ma_values', 'PyObject **', 'ptr')]},
    # 'state' is read as its low byte (punicodeobject.c)
    'PyASCIIObject': {'fields': [('length', 'Py_ssize_t', 'i64'),
                                 ('hash', 'Py_hash_t', 'i64'),
                                 ('state', 'struct {...}', 'u8/low')],
                      'bits': {'state': ('interned:2', 'kind:3', 'compact:1',
                                         'ascii:1', 'ready:1')}},
    'PySliceObject': {'fields': [('start', 'PyObject *', 'ptr'),
                                 ('stop', 'PyObject *', 'ptr'),
                                 ('step', 'PyObject *', 'ptr')]},
    'PyFunctionObject': {'fields': [
        ('func_code', 'PyObject *', 'ptr'), ('func_globals', 'PyObject *', 'ptr'),
        ('func_defaults', 'PyObject *', 'ptr'),
        ('func_kwdefaults', 'PyObject *', 'ptr'),
        ('func_closure', 'PyObject *', 'ptr'),
        ('func_annotations', 'PyObject *', 'ptr'),
        ('func_qualname', 'PyObject *', 'ptr')]},
    'PyMethodObject': {'fields': [('im_func', 'PyObject *', 'ptr'),
                                  ('im_self', 'PyObject *', 'ptr')]},
    'PyCFunctionObject': {'fields': [('m_ml', 'PyMethodDef *', 'ptr'),
                                     ('m_self', 'PyObject *', 'ptr')]},
    'PyCellObject': {'fields': [('ob_ref', 'PyObject *', 'ptr')]},
    # (PyThreadState's recursion_depth and overflowed, and curexc_type for
    # PyErr_Occurred inline: the recursion and exceptions mechanisms, D77)
    # private to a C file: copied under Ni's name
    'seqiterobject': {'from': ('Objects/iterobject.c',), 'copy': 'seqiterobject',
                      'fields': [('it_index', 'Py_ssize_t', 'i64'),
                                 ('it_seq', 'PyObject *', 'ptr')]},
    'rangeobject': {'from': ('Objects/rangeobject.c',), 'copy': 'rangeobject',
                    'fields': [('start', 'PyObject *', 'ptr'),
                               ('stop', 'PyObject *', 'ptr'),
                               ('step', 'PyObject *', 'ptr'),
                               ('length', 'PyObject *', 'ptr')]},
    'rangeiterobject': {'from': ('Objects/rangeobject.c',),
                        'copy': 'rangeiterobject',
                        'fields': [('index', 'long', 'i64'),
                                   ('start', 'long', 'i64'),
                                   ('step', 'long', 'i64'),
                                   ('len', 'long', 'i64')]},
    # shared keys: attribute loads (pobject.c) and the inline store
    'PyDictKeysObject': {'from': ('Objects/dict-common.h',),
                         'copy': 'ni_dictkeys_t',
                         'fields': [('dk_refcnt', 'Py_ssize_t', 'i64'),
                                    ('dk_size', 'Py_ssize_t', 'i64'),
                                    ('dk_usable', 'Py_ssize_t', 'i64'),
                                    ('dk_nentries', 'Py_ssize_t', 'i64'),
                                    ('dk_indices', 'char[]', 'u8/bits')]},
    'PyDictKeyEntry': {'from': ('Objects/dict-common.h',),
                       'copy': 'ni_dictentry_t',
                       'fields': [('me_hash', 'Py_hash_t', 'i64'),
                                  ('me_key', 'PyObject *', 'ptr'),
                                  ('me_value', 'PyObject *', 'ptr')]},
}

# Fields reached from a global variable, for pyruntime.c (built with
# Py_BUILD_CORE): name -> (variable, field path, declared type, read).
# runtime_layouts_gen.h gives NI_RUNTIME_<name>, the field as an lvalue.
RUNTIME = {
    # (gilstate.tstate_current and ceval.eval_breaker moved to the
    # tstate_runtime and eval_breaker_runtime mechanisms: where they are
    # changes between versions, D77)
}

# ---- what Ni accepts without proof ------------------------------------------

# C functions after which no eval_breaker test is needed (D53): they run
# briefly, can't block or release the GIL. 'ni': how psyfunc.c's
# cannot_block_init names them, checked to be the same list.
CANNOT_BLOCK = {
    'Objects/tupleobject.c:PyTuple_New': {'shape': '7ff2ed2715cf', 'ni': ('c', 'PyTuple_New')},
    'Objects/listobject.c:PyList_New': {'shape': 'a8c3b2aceee1', 'ni': ('c', 'PyList_New')},
    'Objects/dictobject.c:PyDict_New': {'shape': '320e221a669d', 'ni': ('c', 'PyDict_New')},
    'Objects/dictobject.c:_PyDict_NewPresized': {'shape': '98e640685d57', 'ni': ('c', '_PyDict_NewPresized')},
    'Objects/longobject.c:PyLong_FromLong': {'shape': '47e1c10789f2', 'ni': ('c', 'PyLong_FromLong')},
    'Objects/floatobject.c:PyFloat_FromDouble': {'shape': '7eb8b391a8de', 'ni': ('c', 'PyFloat_FromDouble')},
    'Objects/funcobject.c:PyFunction_NewWithQualName': {'shape': '48d8c8faf520', 'ni': ('c', 'PyFunction_NewWithQualName')},
    'Objects/classobject.c:PyMethod_New': {'shape': '5d0a51784187', 'ni': ('c', 'PyMethod_New')},
    'Python/pystate.c:PyThreadState_Get': {'shape': 'e9626a2b469d', 'ni': ('c', 'PyThreadState_Get')},
    'Objects/longobject.c:long_richcompare': {'shape': 'daafe12cf7d8', 'ni': ('c', 'PyLong_Type.tp_richcompare')},
    'Objects/floatobject.c:float_richcompare': {'shape': '09e80dfabbf1', 'ni': ('c', 'PyFloat_Type.tp_richcompare')},
    # a Python function's body tests eval_breaker itself (ASSUMPTIONS)
    'Objects/funcobject.c:function_call': {'shape': 'a500be4b3c97', 'ni': ('c', 'PyFunction_Type.tp_call')},
    'Objects/listobject.c:list_append': {'shape': 'cb8131eea016', 'ni': ('method', 'PyList_Type', 'append')},
    'Objects/clinic/listobject.c.h:list_reverse': {'shape': 'b85602eaf2c4', 'ni': ('method', 'PyList_Type', 'reverse')},
    'Objects/dictobject.c:dictkeys_new': {'shape': 'b4e51a30898e', 'ni': ('method', 'PyDict_Type', 'keys')},
    'Objects/dictobject.c:dictitems_new': {'shape': '2b45fecb4474', 'ni': ('method', 'PyDict_Type', 'items')},
    'Objects/dictobject.c:dictvalues_new': {'shape': 'd217d97d5166', 'ni': ('method', 'PyDict_Type', 'values')},
    # str methods: an argument's __index__ can run (ASSUMPTIONS)
    'Objects/unicodeobject.c:unicode_find': {'shape': 'e715046deb39', 'ni': ('method', 'PyUnicode_Type', 'find')},
    'Objects/unicodeobject.c:unicode_rfind': {'shape': 'de4ca4c24cae', 'ni': ('method', 'PyUnicode_Type', 'rfind')},
    'Objects/unicodeobject.c:unicode_index': {'shape': '4a370a83b8ab', 'ni': ('method', 'PyUnicode_Type', 'index')},
    'Objects/unicodeobject.c:unicode_rindex': {'shape': 'f926d4306123', 'ni': ('method', 'PyUnicode_Type', 'rindex')},
    'Objects/unicodeobject.c:unicode_startswith': {'shape': 'b393893c043c', 'ni': ('method', 'PyUnicode_Type', 'startswith')},
    'Objects/unicodeobject.c:unicode_endswith': {'shape': '20d1a9b6a7ad', 'ni': ('method', 'PyUnicode_Type', 'endswith')},
    'Objects/unicodeobject.c:unicode_partition': {'shape': 'bb1256f1e675', 'ni': ('method', 'PyUnicode_Type', 'partition')},
    'Objects/unicodeobject.c:unicode_rpartition': {'shape': 'b5ddf9760757', 'ni': ('method', 'PyUnicode_Type', 'rpartition')},
    'Objects/clinic/unicodeobject.c.h:unicode_lower': {'shape': '549c079278e3', 'ni': ('method', 'PyUnicode_Type', 'lower')},
    'Objects/clinic/unicodeobject.c.h:unicode_upper': {'shape': 'fc23208e3c5d', 'ni': ('method', 'PyUnicode_Type', 'upper')},
    'Objects/clinic/unicodeobject.c.h:unicode_strip': {'shape': '5c47f24759a9', 'ni': ('method', 'PyUnicode_Type', 'strip')},
    'Objects/clinic/unicodeobject.c.h:unicode_lstrip': {'shape': '8488edfb062a', 'ni': ('method', 'PyUnicode_Type', 'lstrip')},
    'Objects/clinic/unicodeobject.c.h:unicode_rstrip': {'shape': '434d35da26f3', 'ni': ('method', 'PyUnicode_Type', 'rstrip')},
    'Objects/clinic/unicodeobject.c.h:unicode_replace': {'shape': '06040776f555', 'ni': ('method', 'PyUnicode_Type', 'replace')},
    'Objects/clinic/unicodeobject.c.h:unicode_split': {'shape': '89edb5485cbd', 'ni': ('method', 'PyUnicode_Type', 'split')},
    'Objects/clinic/unicodeobject.c.h:unicode_isdigit': {'shape': 'accfc916a1be', 'ni': ('method', 'PyUnicode_Type', 'isdigit')},
    'Objects/clinic/unicodeobject.c.h:unicode_isalpha': {'shape': '419f0c4d11fe', 'ni': ('method', 'PyUnicode_Type', 'isalpha')},
    'Objects/clinic/unicodeobject.c.h:unicode_isspace': {'shape': '1d7cdd2ee1c9', 'ni': ('method', 'PyUnicode_Type', 'isspace')},
    'Python/bltinmodule.c:builtin_ord': {'shape': '75203b28b953', 'ni': ('builtin', 'ord')},
    'Python/clinic/bltinmodule.c.h:builtin_chr': {'shape': 'da9c1e42f6fc', 'ni': ('builtin', 'chr')},
    'Python/bltinmodule.c:builtin_callable': {'shape': '5c698f522314', 'ni': ('builtin', 'callable')},
}

# types whose dealloc runs no Python: a decref of one doesn't forget known
# types (rule forget). range's fields are ints, which the effect rules can't
# see. Checked against both copies in Ni.
NO_DEL_TYPES = ['PyUnicode_Type', 'PyBool_Type', 'PyFloat_Type', 'PyLong_Type',
                '_PyNone_Type', 'PyRange_Type']

# descriptor types whose __get__ runs no Python, so an attribute load can be
# specialised on them (pobject.c:load_kind; PATTERNS P26)
SAFE_DESCRIPTORS = ['PyMemberDescr_Type', 'PyMethodDescr_Type',
                    'PyGetSetDescr_Type', 'PyClassMethodDescr_Type',
                    'PyFunction_Type', 'PyStaticMethod_Type', 'PyClassMethod_Type']

# ---- effect rules: the data effects.py starts from --------------------------

# definitions that are themselves a reason, where the walk stops: decref
# (a dealloc of an unknown type can run __del__), gc (a collection: rule
# gc_ignored), blocks (releases the GIL), sets_error (raises: making the
# exception of a built-in class is taken to run no Python, though a
# formatted message's %R can), alloc (the allocators: impure, no Python, no
# blocking; rule allocator_pure), fatal (the process ends: nothing after it
# counts)
SEEDS = {
    'Include/object.h:Py_DECREF': {'shape': 'cccf3ce864a0', 'reason': 'decref'},
    'Include/object.h:Py_XDECREF': {'shape': 'de55f93aa101', 'reason': 'decref'},
    'Include/object.h:Py_CLEAR': {'shape': 'b4930632b907', 'reason': 'decref'},
    'Include/object.h:Py_SETREF': {'shape': '21234fb92350', 'reason': 'decref'},
    'Include/object.h:Py_XSETREF': {'shape': '9ec695f01240', 'reason': 'decref'},
    'Modules/gcmodule.c:collect_generations': {'shape': '78142109d4cb', 'reason': 'gc'},
    'Include/ceval.h:Py_BEGIN_ALLOW_THREADS': {'shape': 'a919b9e72151', 'reason': 'blocks'},
    'Python/ceval.c:PyEval_SaveThread': {'shape': 'c54c983679f6', 'reason': 'blocks'},
    'Python/errors.c:PyErr_SetObject': {'shape': 'a2d4e569f670', 'reason': 'sets_error'},
    'Python/errors.c:PyErr_FormatV': {'shape': 'b0ede5d10755', 'reason': 'sets_error'},
    'Python/errors.c:PyErr_Format': {'shape': '57c410a9f0e8', 'reason': 'sets_error'},
    'Objects/obmalloc.c:PyObject_Malloc': {'shape': '6a6fcd583fed', 'reason': 'alloc'},
    'Objects/obmalloc.c:PyObject_Calloc': {'shape': '0c93ef2a2198', 'reason': 'alloc'},
    'Objects/obmalloc.c:PyObject_Realloc': {'shape': '7ab30f22c37c', 'reason': 'alloc'},
    'Objects/obmalloc.c:PyObject_Free': {'shape': '19876eceed4f', 'reason': 'alloc'},
    'Objects/obmalloc.c:PyMem_Malloc': {'shape': '8dd5c0f03b9a', 'reason': 'alloc'},
    'Objects/obmalloc.c:PyMem_Calloc': {'shape': 'd14119f1b0c8', 'reason': 'alloc'},
    'Objects/obmalloc.c:PyMem_Realloc': {'shape': '1babdb33cb01', 'reason': 'alloc'},
    'Objects/obmalloc.c:PyMem_Free': {'shape': 'e03b93b5cb56', 'reason': 'alloc'},
    'Objects/obmalloc.c:PyMem_RawMalloc': {'shape': 'f462d9c0b83c', 'reason': 'alloc'},
    'Objects/obmalloc.c:PyMem_RawCalloc': {'shape': 'f09f483432b1', 'reason': 'alloc'},
    'Objects/obmalloc.c:PyMem_RawRealloc': {'shape': 'dda74da5262c', 'reason': 'alloc'},
    'Objects/obmalloc.c:PyMem_RawFree': {'shape': 'd104e15ea9ac', 'reason': 'alloc'},
    'Python/pylifecycle.c:Py_FatalError': {'shape': 'ad435365cd05', 'reason': 'fatal'},
    'Include/pymacro.h:Py_UNREACHABLE': {'shape': '45fb1e8945ba', 'reason': 'fatal'},
}

# functions outside CPython's source (libc, libm), which no call graph sees:
# pure (no side effect: can run at compile time), no_python, blocks. A
# blocking system call in CPython is inside Py_BEGIN_ALLOW_THREADS (SEEDS),
# so none is listed as blocks; any other is taken as no_python, not pure
EXTERNAL = {
    'fmod': 'pure', 'copysign': 'pure', 'fabs': 'pure', 'floor': 'pure',
    'ceil': 'pure', 'sqrt': 'pure', 'exp': 'pure', 'log': 'pure', 'pow': 'pure',
    'sin': 'pure', 'cos': 'pure', 'tan': 'pure', 'asin': 'pure', 'acos': 'pure',
    'atan': 'pure', 'atan2': 'pure', 'sinh': 'pure', 'cosh': 'pure',
    'tanh': 'pure', 'modf': 'pure', 'frexp': 'pure', 'ldexp': 'pure',
    'hypot': 'pure', 'round': 'pure',
    'memcmp': 'pure', 'strlen': 'pure', 'strcmp': 'pure', 'strncmp': 'pure',
}

# effects Ni assumes that the analysis can't prove, each with its reason
ASSUMPTIONS = {
    'Python/errors.c:PyErr_Restore': {
        'shape': 'eaf51356ed47', 'assume': ['no_python'],
        'why': "replacing the current exception decrefs the old one; Ni accepts"
               " that an exception's dealloc runs no Python (PyErr_Clear,"
               " PyErr_Fetch and PyDict_GetItem's error suppression go here)"},
    'Python/errors.c:PyErr_SetString': {
        'shape': '6f7f1f9a4690', 'assume': ['no_python', 'no_block', 'raises'],
        'why': 'with a built-in exception class (PyExc_*) and an ASCII message,'
               ' the exception is made by C code that neither runs Python nor'
               ' releases the GIL (the message decoder could, on an error'
               " handler); gen_float.py copies C that calls it"},
    'Objects/dictobject.c:PyDict_GetItem': {
        'shape': '73f877432a78', 'assume': ['no_python'],
        'why': "KNOWN_BUGS F34: an instance dict looked up by an exact str name;"
               " only a str-subclass key's __eq__ could run, which Ni accepts"},
    'Objects/dictobject.c:_PyDict_GetItem_KnownHash': {
        'shape': 'c6a01d89a6a6', 'assume': ['no_python'], 'why': 'F34, as PyDict_GetItem'},
    'Objects/funcobject.c:function_call': {
        'shape': 'a500be4b3c97', 'assume': ['no_block'],
        'why': "D53: a Python function's body tests eval_breaker itself"},
    'Objects/abstract.c:PyNumber_Index': {
        'shape': '0902066dcb68', 'assume': ['no_block'],
        'why': "D53: the str methods in CANNOT_BLOCK can call an argument's"
               " __index__, which Ni accepts"},
}

# ---- entries: one per CPython definition a hand translation mirrors ---------
# From doc/INVENTORY.md's translated and mixed rows (dead code left out).

ENTRIES = {
    'Objects/frameobject.c:PyFrame_BlockSetup': {'shape': '2808be75881d', 'ni': ['ni/Python/pycompiler.c:block_setup']},
    'Objects/frameobject.c:PyFrame_BlockPop': {'shape': '025ac072c2ff', 'ni': ['ni/Python/pycompiler.c:block_pop']},
    'Python/errors.c:PyErr_Fetch': {'shape': 'dafca4286947', 'ni': ['ni/Python/pycompiler.c:cimpl_pyerr_fetch']},
    'Python/ceval.c:_PyEval_EvalFrameDefault': {
        'shape': '8fbb1f607b64',
        'ni': ['ni/Python/pycompiler.c:cimpl_push_exc_info', 'ni/Python/pycompiler.c:psyco_pycompiler_mainloop', 'ni/psyfunc.c:psyco_check_eval_breaker', 'ni/psyfunc.c:tracing_on', 'ni/mergepoints.c:BREAKER_CHECK', 'ni/mechanisms/eval_breaker_runtime/ceval_loop_top.h:ni_eval_breaker_handler', 'ni/mechanisms/eval_breaker_runtime/ceval_loop_top.h:ni_eval_breaker_at_entry'],
        'policy': [
            ('forget', 'handler'),
            ('likely', 'false', 'eval_breaker set'),
            ('likely', 'false', 'tstate->use_tracing'),
            ('other', 'trace_fallback', 'C23, D46'),
        ]},
    'Python/ceval.c:UNWIND_EXCEPT_HANDLER': {
        'shape': '1e849cd8dbee',
        'ni': ['ni/Python/pycompiler.c:cimpl_pop_exc_info', 'ni/Python/pycompiler.c:unwind_except_handler'],
        'policy': [
            ('ref', 'no_steal', 'the saved exception'),
        ]},
    'Objects/frameobject.c:PyFrame_New': {'shape': 'f61d12d90a3b', 'ni': ['ni/Python/pycompiler.c:cimpl_new_frame']},
    'Python/traceback.c:PyTraceBack_Here': {'shape': '15968e96a8b4', 'ni': ['ni/Python/pycompiler.c:cimpl_rt_traceback', 'ni/Python/pycompiler.c:cimpl_vt_traceback']},
    'Objects/frameobject.c:_PyFrame_New_NoTrack': {'shape': 'db57cf1c5257', 'ni': ['ni/Python/pycompiler.c:psy_get_builtins', 'ni/psyfunc.c:callee_builtins']},
    'Python/ceval.c:call_function': {
        'shape': '9adad90cff0d',
        'ni': ['ni/Python/pycompiler.c:psyco_ext_do_calls'],
        'policy': [
            ('virtual', 'args_tuple', 'positional arguments'),
        ]},
    'Python/ceval.c:TARGET(CALL_FUNCTION_EX)': {
        'shape': '2b95343e7f7d',
        'ni': ['ni/Python/pycompiler.c:cimpl_call_function_ex', 'ni/Python/pycompiler.c:psyco_ext_do_call_ex'],
        'policy': [
            ('ref', 'keep_alive', 'kwargs during the call'),
            ('promote', 'type', 'callargs'),
            ('shortcut', 'direct_meta', 'an exact tuple without **'),
        ]},
    'Python/ceval.c:check_args_iterable': {'shape': 'c64cc9801246', 'ni': ['ni/Python/pycompiler.c:cimpl_call_function_ex', 'ni/Python/pycompiler.c:cimpl_build_unpack']},
    'Python/ceval.c:format_kwargs_mapping_error': {'shape': '8a34b9309457', 'ni': ['ni/Python/pycompiler.c:cimpl_call_function_ex', 'ni/Python/pycompiler.c:cimpl_build_map_unpack']},
    'Python/ceval.c:format_exc_check_arg': {'shape': '4a1f4255a9e4', 'ni': ['ni/Python/pycompiler.c:unbound_local_error']},
    'Python/ceval.c:format_exc_unbound': {'shape': '7670fed16a8a', 'ni': ['ni/Python/pycompiler.c:unbound_deref_error']},
    'Python/ceval.c:import_from': {'shape': '0f68c89c4d5f', 'ni': ['ni/Python/pycompiler.c:cimpl_import_from']},
    'Python/ceval.c:cmp_outcome': {'shape': 'be6025dd04f3', 'ni': ['ni/Python/pycompiler.c:cimpl_exc_match', 'ni/Python/pycompiler.c:case COMPARE_OP']},
    'Python/ceval.c:TARGET(STORE_DEREF)': {
        'shape': '4c4c566b974a',
        'ni': ['ni/Python/pycompiler.c:cimpl_store_deref', 'ni/Python/pycompiler.c:case STORE_DEREF'],
        'policy': [
            ('ref', 'incref', 'v, which ceval steals'),
        ]},
    'Python/ceval.c:TARGET(BUILD_TUPLE_UNPACK_WITH_CALL)': {
        'shape': '661922c6394b',
        'ni': ['ni/Python/pycompiler.c:cimpl_build_unpack', 'ni/Python/pycompiler.c:case BUILD_TUPLE_UNPACK_WITH_CALL', 'ni/Python/pycompiler.c:case BUILD_TUPLE_UNPACK'],
        'policy': [
            ('virtual', 'concat', 'parts that are tuples with known items'),
            ('restype', 'PyTuple_Type', 'BUILD_TUPLE_*'),
        ]},
    'Python/ceval.c:TARGET(BUILD_TUPLE_UNPACK)': {'shape': 'b1abba726c4d', 'ni': ['ni/Python/pycompiler.c:cimpl_build_unpack', 'ni/Python/pycompiler.c:case BUILD_TUPLE_UNPACK_WITH_CALL', 'ni/Python/pycompiler.c:case BUILD_TUPLE_UNPACK']},
    'Python/ceval.c:TARGET(BUILD_LIST_UNPACK)': {
        'shape': 'c95697e345d5',
        'ni': ['tools/metac/gen_ceval.py:translate'],
        'policy': [
            ('restype', 'PyList_Type'),
        ]},
    'Python/ceval.c:TARGET(BUILD_MAP_UNPACK)': {
        'shape': '84eba75c8103',
        'ni': ['ni/Python/pycompiler.c:cimpl_build_map_unpack', 'ni/Python/pycompiler.c:case BUILD_MAP_UNPACK', 'ni/Python/pycompiler.c:case BUILD_MAP_UNPACK_WITH_CALL'],
        'policy': [
            ('restype', 'PyDict_Type', 'BUILD_MAP_*'),
        ]},
    'Python/ceval.c:TARGET(BUILD_MAP_UNPACK_WITH_CALL)': {'shape': '1304d703ab21', 'ni': ['ni/Python/pycompiler.c:cimpl_build_map_unpack', 'ni/Python/pycompiler.c:case BUILD_MAP_UNPACK', 'ni/Python/pycompiler.c:case BUILD_MAP_UNPACK_WITH_CALL']},
    'Python/ceval.c:TARGET(BUILD_SET_UNPACK)': {
        'shape': 'bce59f084ed5',
        'ni': ['tools/metac/gen_ceval.py:translate'],
        'policy': [
            ('restype', 'PySet_Type'),
        ]},
    'Python/ceval.c:special_lookup': {'shape': '733e627cd8eb', 'ni': ['ni/Python/pycompiler.c:special_lookup']},
    'Python/ceval.c:TARGET(SETUP_WITH)': {'shape': '5484de92c04f', 'ni': ['ni/Python/pycompiler.c:cimpl_setup_with']},
    'Python/ceval.c:TARGET(WITH_CLEANUP_START)': {'shape': 'a33864999e5e', 'ni': ['ni/Python/pycompiler.c:cimpl_call_exit']},
    'Python/ceval.c:TARGET(LOAD_BUILD_CLASS)': {'shape': '5d23b6efa3cf', 'ni': ['ni/Python/pycompiler.c:cimpl_load_build_class', 'ni/Python/pycompiler.c:case LOAD_BUILD_CLASS']},
    'Python/ceval.c:TARGET(DELETE_DEREF)': {'shape': '21086dea1389', 'ni': ['ni/Python/pycompiler.c:cimpl_delete_deref', 'ni/Python/pycompiler.c:case DELETE_DEREF']},
    'Python/ceval.c:TARGET(LOAD_GLOBAL)': {
        'shape': 'fe591e989209',
        'ni': ['ni/Python/pycompiler.c:cimpl_load_global', 'ni/Python/pycompiler.c:case LOAD_NAME', 'ni/Python/pycompiler.c:case LOAD_GLOBAL'],
        'policy': [
            ('guard', 'dict_version', 'globals and builtins ma_version_tag (D25)'),
            ('promote', 'constant', 'the global found'),
        ]},
    'Python/ceval.c:unpack_iterable': {'shape': 'f2defec6bca8', 'ni': ['ni/Python/pycompiler.c:unpack_iterable', 'ni/Python/pycompiler.c:cimpl_unpack_iterable']},
    'Python/ceval.c:TARGET(UNPACK_EX)': {
        'shape': 'ccc51a5023cc',
        'ni': ['ni/Python/pycompiler.c:cimpl_unpack_ex', 'ni/Python/pycompiler.c:case UNPACK_EX'],
        'policy': [
            ('restype', 'PyList_Type', 'the starred target'),
        ]},
    'Python/ceval.c:TARGET(UNPACK_SEQUENCE)': {
        'shape': '477cfd41b0d4',
        'ni': ['ni/Python/pycompiler.c:cimpl_unpack_list', 'ni/Python/pycompiler.c:case UNPACK_SEQUENCE'],
        'policy': [
            ('shortcut', 'compile_time_unpack', 'a virtual tuple of the right length'),
            ('ref', 'need_reference', 'the items'),
        ]},
    'Python/ceval.c:do_raise': {
        'shape': 'bbe63b1fe89c',
        'ni': ['ni/Python/pycompiler.c:cimpl_reraise', 'ni/Python/pycompiler.c:cimpl_do_raise'],
        'policy': [
            ('ref', 'no_steal', 'exc and cause'),
        ]},
    'Python/errors.c:_PyErr_GetTopmostException': {'shape': '87bdd1a8224e', 'ni': ['ni/mechanisms/exceptions/curexc_triple.h:ni_exc_info_topmost']},
    'Python/ceval.c:import_name': {'shape': '83a19cc303d9', 'ni': ['ni/Python/pycompiler.c:cimpl_import_name']},
    'Python/ceval.c:import_all_from': {'shape': 'e643326dceb4', 'ni': ['ni/Python/pycompiler.c:cimpl_import_all_from']},
    'Python/ceval.c:TARGET(FORMAT_VALUE)': {
        'shape': '2024d177721d',
        'ni': ['ni/Python/pycompiler.c:cimpl_format_value', 'ni/Python/pycompiler.c:case FORMAT_VALUE'],
        'policy': [
            ('shortcut', 'identity', 'an exact str with no conversion or spec'),
            ('restype', 'PyUnicode_Type', 'KNOWN_BUGS F98: a __format__ may return a subclass'),
        ]},
    'Python/ceval.c:TARGET(BUILD_STRING)': {
        'shape': 'fed73d9e1625',
        'ni': ['ni/Python/pycompiler.c:cimpl_build_string', 'ni/Python/pycompiler.c:case BUILD_STRING'],
        'policy': [
            ('limit', 'max_args', '4 parts as arguments, more as a tuple'),
            ('restype', 'PyUnicode_Type'),
        ]},
    'Python/bltinmodule.c:min_max': {
        'shape': 'd027ceb9f18e',
        'ni': ['ni/Python/pycompiler.c:cimpl_minmax_step', 'ni/Python/pycompiler.c:cimpl_minmax_result', 'ni/Python/pbltinmodule.c:pbuiltin_min_max'],
        'policy': [
            ('promote', 'type', 'every argument'),
            ('shortcut', 'known_length', 'two or more positional arguments'),
            ('likely', 'false', 'the item compares better than the current best'),
        ]},
    'Objects/genobject.c:gen_send_ex': {'shape': 'dbfa5af99336', 'ni': ['ni/Python/pycompiler.c:cimpl_generator_stop']},
    'Python/errors.c:_PyErr_ChainExceptions': {'shape': '8624abd89f50', 'ni': ['ni/Python/pycompiler.c:cimpl_generator_stop']},
    'Python/ceval.c:TARGET(UNARY_NOT)': {'shape': '1186f933f995', 'ni': ['tools/metac/gen_ceval.py:translate']},
    'Python/ceval.c:TARGET(LIST_APPEND)': {'shape': 'd2b86bdd5af1', 'ni': ['tools/metac/gen_ceval.py:translate']},
    'Python/ceval.c:TARGET(DELETE_SUBSCR)': {'shape': 'b1a468b9cda7', 'ni': ['tools/metac/gen_ceval.py:build']},
    'Python/ceval.c:TARGET(RETURN_VALUE)': {
        'shape': '6e2836ffbc99',
        'ni': ['ni/Python/pycompiler.c:case RETURN_VALUE'],
        'policy': [
            ('virtual', 'fuse', "a fused generator expression returns its consumer's result"),
        ]},
    'Python/ceval.c:TARGET(YIELD_VALUE)': {
        'shape': '5a91ab79884b',
        'ni': ['ni/Python/pycompiler.c:case YIELD_VALUE'],
        'policy': [
            ('virtual', 'fuse', 'list, set, sum, min, max, any, all'),
            ('shortcut', 'direct_meta', 'sum() adds by PsycoNumber_Add when the type is known'),
            ('likely', 'false', 'any()/all() exit early'),
        ]},
    'Python/ceval.c:TARGET(STORE_ATTR)': {'shape': '7a20351f6b63', 'ni': ['tools/metac/gen_ceval.py:translate']},
    'Python/ceval.c:TARGET(DELETE_ATTR)': {'shape': 'cedf0d1cb61f', 'ni': ['tools/metac/gen_ceval.py:translate']},
    'Python/ceval.c:TARGET(STORE_GLOBAL)': {'shape': '8c0f15c532b8', 'ni': ['ni/Python/pycompiler.c:case STORE_NAME', 'ni/Python/pycompiler.c:case STORE_GLOBAL']},
    'Python/ceval.c:TARGET(STORE_NAME)': {'shape': '70e9ba8d2a5b', 'ni': ['ni/Python/pycompiler.c:case STORE_NAME', 'ni/Python/pycompiler.c:case STORE_GLOBAL']},
    'Python/ceval.c:TARGET(DELETE_GLOBAL)': {'shape': '52ffe7abb0dc', 'ni': ['ni/Python/pycompiler.c:case DELETE_NAME', 'ni/Python/pycompiler.c:case DELETE_GLOBAL']},
    'Python/ceval.c:TARGET(DELETE_NAME)': {'shape': '4c9a396fff41', 'ni': ['ni/Python/pycompiler.c:case DELETE_NAME', 'ni/Python/pycompiler.c:case DELETE_GLOBAL']},
    'Python/ceval.c:TARGET(LOAD_NAME)': {'shape': '169ba7986f56', 'ni': ['ni/Python/pycompiler.c:case LOAD_NAME', 'ni/Python/pycompiler.c:case LOAD_GLOBAL']},
    'Python/ceval.c:TARGET(DELETE_FAST)': {'shape': '88625a6a9a19', 'ni': ['tools/metac/gen_ceval.py:translate']},
    'Python/ceval.c:TARGET(LOAD_CLOSURE)': {'shape': '11f8250608e3', 'ni': ['tools/metac/gen_ceval.py:translate']},
    'Python/ceval.c:TARGET(LOAD_DEREF)': {
        'shape': '2e6c9f5e9bed',
        'ni': ['ni/Python/pycompiler.c:case LOAD_DEREF'],
        'policy': [
            ('ref', 'need_reference', 'the cell content'),
        ]},
    'Python/ceval.c:TARGET(BUILD_TUPLE)': {'shape': '0db52e5ab2d9', 'ni': ['tools/metac/gen_ceval.py:translate']},
    'Python/ceval.c:TARGET(BUILD_LIST)': {'shape': '60183638719d', 'ni': ['tools/metac/gen_ceval.py:translate']},
    'Python/ceval.c:TARGET(BUILD_MAP)': {'shape': '2a0606f6f2ba', 'ni': ['tools/metac/gen_ceval.py:translate']},
    'Python/ceval.c:TARGET(BUILD_CONST_KEY_MAP)': {'shape': '3f6a652605b8', 'ni': ['ni/Python/pycompiler.c:case BUILD_CONST_KEY_MAP']},
    'Python/ceval.c:TARGET(MAP_ADD)': {'shape': '97181b1620fd', 'ni': ['tools/metac/gen_ceval.py:translate']},
    'Python/ceval.c:TARGET(BUILD_SET)': {
        'shape': 'f137fb543930',
        'ni': ['tools/metac/gen_ceval.py:translate'],
        'policy': [
            ('restype', 'PySet_Type'),
        ]},
    'Python/ceval.c:TARGET(SET_ADD)': {'shape': 'c0396bf8b3b0', 'ni': ['tools/metac/gen_ceval.py:translate']},
    'Python/ceval.c:TARGET(LOAD_ATTR)': {'shape': '6249c7dcf5e3', 'ni': ['tools/metac/gen_ceval.py:translate']},
    'Python/ceval.c:TARGET(LOAD_METHOD)': {
        'shape': '559aa5079123',
        'ni': ['ni/Python/pycompiler.c:case LOAD_METHOD'],
        'policy': [
            ('virtual', 'result', 'PyMethod_Type'),
        ]},
    'Python/ceval.c:TARGET(COMPARE_OP)': {
        'shape': 'f6b147c1740c',
        'ni': ['ni/Python/pycompiler.c:case COMPARE_OP'],
        'policy': [
            ('shortcut', 'word_compare', 'is, is not'),
        ]},
    'Python/ceval.c:TARGET(IMPORT_NAME)': {'shape': 'b192173e7a57', 'ni': ['ni/Python/pycompiler.c:case IMPORT_NAME']},
    'Python/ceval.c:TARGET(IMPORT_STAR)': {'shape': '66ad65b82d90', 'ni': ['ni/Python/pycompiler.c:case IMPORT_STAR']},
    'Python/ceval.c:TARGET(IMPORT_FROM)': {'shape': 'cc9c45f8467c', 'ni': ['ni/Python/pycompiler.c:case IMPORT_FROM']},
    'Python/ceval.c:TARGET(JUMP_FORWARD)': {'shape': '6d28fc612e60', 'ni': ['tools/metac/gen_ceval.py:translate']},
    'Python/ceval.c:TARGET(POP_JUMP_IF_FALSE)': {
        'shape': '71362d8c5285',
        'ni': ['tools/metac/gen_ceval.py:translate'],
        'policy': [
            ('forget', 'before_branch', 'PyObject_IsTrue may have run code (F55): the branch forgets what it knew of classes'),
        ]},
    'Python/ceval.c:TARGET(POP_JUMP_IF_TRUE)': {
        'shape': '3d67d69a303c',
        'ni': ['tools/metac/gen_ceval.py:translate'],
        'policy': [
            ('forget', 'before_branch', 'PyObject_IsTrue may have run code (F55): the branch forgets what it knew of classes'),
        ]},
    'Python/ceval.c:TARGET(JUMP_IF_FALSE_OR_POP)': {
        'shape': '9d16f541b99d',
        'ni': ['tools/metac/gen_ceval.py:translate'],
        'policy': [
            ('forget', 'before_branch', 'PyObject_IsTrue may have run code (F55): the branch forgets what it knew of classes'),
        ]},
    'Python/ceval.c:TARGET(JUMP_IF_TRUE_OR_POP)': {
        'shape': 'a933d0612595',
        'ni': ['tools/metac/gen_ceval.py:translate'],
        'policy': [
            ('forget', 'before_branch', 'PyObject_IsTrue may have run code (F55): the branch forgets what it knew of classes'),
        ]},
    'Python/ceval.c:TARGET(JUMP_ABSOLUTE)': {'shape': '88c405bbfbc1', 'ni': ['tools/metac/gen_ceval.py:translate']},
    'Python/ceval.c:TARGET(FOR_ITER)': {
        'shape': '579975832e35',
        'ni': ['ni/Python/pycompiler.c:case FOR_ITER', 'ni/Objects/pabstract.c:cimpl_iternext', 'ni/Objects/pabstract.c:PsycoIter_Next'],
        'policy': [
            ('likely', 'true', 'the next item is non-NULL'),
        ]},
    'Python/ceval.c:TARGET(MAKE_FUNCTION)': {
        'shape': '3490db0c373d',
        'ni': ['ni/Python/pycompiler.c:case MAKE_FUNCTION'],
        'policy': [
            ('virtual', 'result', 'PyFunction_Type'),
            ('limit', 'nested_weight', 'closure, annotations and defaults'),
        ]},
    'Python/ceval.c:TARGET(BUILD_SLICE)': {'shape': '4f7d2d7b815b', 'ni': ['tools/metac/gen_ceval.py:translate']},
    'Python/ceval.c:TARGET(EXTENDED_ARG)': {'shape': '9bb225918986', 'ni': ['ni/Python/pycompiler.c:case EXTENDED_ARG']},
    'Objects/typeobject.c:PyType_IsSubtype': {'shape': '68f9fd0d8e28', 'ni': ['ni/Objects/pobject.c:Psyco_TypeIsSubtype']},
    'Objects/object.c:PyObject_IsTrue': {
        'shape': 'a01d2496db32',
        'ni': ['ni/Objects/pobject.c:PsycoObject_IsTrue'],
        'policy': [
            ('shortcut', 'const_fold', 'None'),
        ]},
    'Objects/object.c:PyObject_GetAttr': {
        'shape': '999d93e1d6a6',
        'ni': ['ni/Objects/pobject.c:PsycoObject_GetAttr'],
        'policy': [
            ('mega', 'type', 'the object'),
            ('shortcut', 'specialise_only', 'a constant exact-str name, a heap type or PyObject_GenericGetAttr'),
        ]},
    'Objects/object.c:_PyObject_GenericGetAttrWithDict': {
        'shape': 'a2a61354cb81',
        'ni': ['ni/Objects/pobject.c:cimpl_instance_dict_get', 'ni/Objects/pobject.c:instance_dict_get', 'ni/Objects/pobject.c:load_kind', 'ni/Objects/pobject.c:generic_getattr'],
        'policy': [
            ('shortcut', 'inline_read', 'a split dict value by the shared keys (D31)'),
            ('likely', 'true', 'the instance has a dict'),
            ('likely', 'true', 'its keys are the cached shared keys'),
            ('other', 'compile_time_hash', 'the attribute name'),
            ('shortcut', 'type_whitelist', 'safe builtin descriptor types (SAFE_DESCRIPTORS)'),
            ('guard', 'generic_for_heap', 'a descriptor of a heap type takes the generic lookup'),
            ('mega', 'type', 'the object'),
            ('guard', 'version_tag', 'per (type, name, kind), repaired at run time (D30, D60)'),
            ('promote', 'constant', 'a class attribute without __get__'),
            ('likely', 'true', 'found in the instance dict, unless the class has the attribute'),
            ('ref', 'need_reference', 'the dict value'),
            ('ref', 'held', 'the descriptor'),
            ('other', 'ends_block', 'after the guard'),
        ]},
    'Objects/dictobject.c:_PyDict_GetItem_KnownHash': {
        'shape': 'c6a01d89a6a6',
        'ni': ['ni/Objects/pobject.c:cimpl_dict_get_known'],
        'policy': [
            ('other', 'compile_time_hash', 'the attribute name'),
        ]},
    'Objects/dictobject.c:lookdict_split': {'shape': '42270a67629a', 'ni': ['ni/Objects/pobject.c:instance_dict_get']},
    'Objects/object.c:PyObject_GenericGetAttr': {'shape': 'eec97f4eb45f', 'ni': ['ni/Objects/pobject.c:PsycoObject_GenericGetAttr']},
    'Objects/object.c:_PyObject_GenericSetAttrWithDict': {
        'shape': 'b136a3f0b3d2',
        'ni': ['ni/Objects/pobject.c:cimpl_instance_dict_set', 'ni/Objects/pobject.c:cimpl_dict_set_known', 'ni/Objects/pobject.c:store_kind', 'ni/Objects/pobject.c:heap_setattr'],
        'policy': [
            ('other', 'compile_time_hash', 'the attribute name'),
            ('guard', 'generic_for_heap', 'a class attribute of a heap type counts as a data descriptor'),
            ('guard', 'store_guard', 'per (type, name, kind), repaired at run time (D39)'),
            ('likely', 'true', 'the instance has a dict'),
            ('likely', 'true', 'the dict is combined'),
            ('other', 'ends_block', 'after the guard'),
        ]},
    'Objects/dictobject.c:_PyObjectDict_SetItem': {'shape': '102b3181aa73', 'ni': ['ni/Objects/pobject.c:cimpl_store_miss', 'ni/Objects/pobject.c:heap_setattr']},
    'Objects/dictobject.c:insertdict': {
        'shape': '854ccc2dc1b2',
        'ni': ['ni/Objects/pobject.c:inline_store'],
        'policy': [
            ('forget', 'store', "the old value's dealloc"),
            ('shortcut', 'skip_check', 'GC tracking from the known type'),
            ('likely', 'false', 'the store cache misses'),
        ]},
    'Objects/object.c:PyObject_SetAttr': {
        'shape': '7f2594059a7c',
        'ni': ['ni/Objects/pobject.c:PsycoObject_SetAttr'],
        'policy': [
            ('mega', 'type', 'the object'),
            ('shortcut', 'specialise_only', 'a constant exact-str name'),
        ]},
    'Objects/object.c:_Py_SwappedOp': {'shape': 'cfe40181e844', 'ni': ['ni/Objects/pobject.c:swapped_op']},
    'Objects/object.c:do_richcompare': {'shape': '9ba14ef60646', 'ni': ['ni/Objects/pobject.c:try_rich_compare', 'ni/Objects/pobject.c:PsycoObject_RichCompare']},
    'Objects/object.c:opstrings': {'shape': '50f0a36819d3', 'ni': ['ni/Objects/pobject.c:opstrings']},
    'Objects/object.c:PyObject_RichCompare': {
        'shape': '86e62585f71c',
        'ni': ['ni/Objects/pobject.c:PsycoObject_RichCompare'],
        'policy': [
            ('virtual', 'result', 'PyBool_Type (condition code)'),
        ]},
    'Objects/object.c:PyObject_RichCompareBool': {'shape': '09b8e3017aae', 'ni': ['ni/Objects/pobject.c:PsycoObject_RichCompareBool']},
    'Objects/object.c:PyObject_Hash': {'shape': 'ad375cb16529', 'ni': ['ni/Objects/pobject.c:PsycoObject_Hash']},
    'Include/object.h:PyObject_TypeCheck': {'shape': '888f9816c6d8', 'ni': ['ni/Objects/pobject.h:PyType_TypeCheck']},
    'Include/abstract.h:PyIter_Check': {'shape': 'd43c4196f3b7', 'ni': ['ni/Objects/pobject.h:PyType_TypeCheck']},
    'Objects/dict-common.h:_dictkeysobject': {'shape': 'aa0c0e83a5c7', 'ni': ['tools/metac/gen_layouts.py:copy_struct']},
    'Objects/dict-common.h:PyDictKeyEntry': {'shape': 'a42b235499c6', 'ni': ['tools/metac/gen_layouts.py:copy_struct']},
    'Objects/abstract.c:type_error': {'shape': '121037b863a2', 'ni': ['ni/Objects/pabstract.c:type_error_tp']},
    'Objects/call.c:PyObject_Call': {'shape': '61774182df72', 'ni': ['ni/Objects/pabstract.c:PsycoObject_Call']},
    'Objects/abstract.c:PyObject_GetItem': {'shape': '636cdcd5389b', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyObject_SetItem': {'shape': '2c0fb58c9dd6', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyObject_DelItem': {'shape': '922ec42abbdc', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyObject_Size': {'shape': '9861cd31b9b8', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyMapping_Size': {'shape': '2f44f1295dd8', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/tupleobject.c:tuplelength': {'shape': 'c26701baea26', 'ni': ['ni/Objects/pabstract.c:psyco_generic_immut_ob_size']},
    'Objects/listobject.c:list_length': {'shape': '03a5f134f09c', 'ni': ['ni/Objects/pabstract.c:psyco_generic_mut_ob_size']},
    'Objects/abstract.c:PySequence_GetItem': {
        'shape': 'dd19d399aba4',
        'ni': ['tools/metac/gen_abstract.py:generate'],
        'policy': [
            ('likely', 'false', 'i < 0'),
        ]},
    'Objects/abstract.c:PySequence_SetItem': {
        'shape': 'cb88b0595847',
        'ni': ['tools/metac/gen_abstract.py:generate'],
        'policy': [
            ('likely', 'false', 'i < 0'),
        ]},
    'Objects/abstract.c:PySequence_DelItem': {
        'shape': '8156fc551476',
        'ni': ['tools/metac/gen_abstract.py:generate'],
        'policy': [
            ('likely', 'false', 'i < 0'),
        ]},
    'Objects/abstract.c:PySequence_Contains': {'shape': 'd2be1c0b60b5', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:_PySequence_IterSearch': {'shape': '85839b7e7d20', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PySequence_Tuple': {
        'shape': 'b0b47f97a0b1',
        'ni': ['tools/metac/gen_abstract.py:generate'],
        'policy': [
            ('restype', 'PyTuple_Type'),
        ]},
    'Objects/abstract.c:PySequence_List': {
        'shape': '9a4514bee4c6',
        'ni': ['tools/metac/gen_abstract.py:generate'],
        'policy': [
            ('restype', 'PyList_Type'),
        ]},
    'Objects/tupleobject.c:tuplecontains': {
        'shape': '9840edeba42d',
        'ni': ['tools/metac/gen_abstract.py:generate'],
        'policy': [
            ('likely', 'true', 'cmp == 0'),
        ]},
    'Objects/abstract.c:NB_SLOT': {'shape': '3a266ba2f176', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:NB_BINOP': {'shape': '2160f98ee50a', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:NB_TERNOP': {'shape': 'af1401f8c92c', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyNumber_Negative': {'shape': '4c145ae574c3', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyNumber_Positive': {'shape': 'df1058d91efa', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyNumber_Invert': {'shape': '2e18fefefdfd', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyNumber_Absolute': {'shape': 'd80c1819acf3', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:binary_op1': {'shape': 'b07641ef2bfc', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:binop_type_error': {'shape': '9c406ff3be15', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:binary_op': {'shape': '88c3557958f7', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:BINARY_FUNC': {'shape': 'fb844d89a981', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyNumber_FloorDivide': {'shape': 'b67662baeda7', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyNumber_TrueDivide': {'shape': 'fb6fc71946f1', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyNumber_MatrixMultiply': {'shape': 'c51ff682c16f', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:sequence_repeat': {'shape': 'd5d1ee582f2c', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyNumber_Add': {'shape': '773b92704612', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyNumber_Multiply': {'shape': '90bb8f103f3b', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyNumber_InPlaceAdd': {'shape': '351ed3fd1169', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyNumber_InPlaceMultiply': {'shape': '07b2f46558e8', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyNumber_Remainder': {'shape': '41c76f5e308e', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyNumber_Power': {
        'shape': 'eddf18a1e93c',
        'ni': ['ni/Objects/pabstract.c:PsycoNumber_Power'],
        'policy': [
            ('shortcut', 'type_whitelist', 'PyLong_Type, PyFloat_Type'),
        ]},
    'Objects/abstract.c:binary_iop1': {'shape': 'cf96db8f5466', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:binary_iop': {'shape': 'f391f933fafa', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:INPLACE_BINOP': {'shape': 'f15df2cac8ff', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyNumber_InPlaceFloorDivide': {'shape': '28595b299826', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyNumber_InPlaceTrueDivide': {'shape': 'ff0e0a722368', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyNumber_InPlaceMatrixMultiply': {'shape': '48a649cec7c9', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyNumber_InPlaceRemainder': {'shape': '8bcb42cad4e7', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyNumber_InPlacePower': {'shape': '337da8595b87', 'ni': ['tools/metac/gen_abstract.py:generate']},
    'Objects/abstract.c:PyObject_GetIter': {
        'shape': 'f2cd675205cb',
        'ni': ['ni/Objects/pabstract.c:PsycoObject_GetIter'],
        'policy': [
            ('shortcut', 'skip_check', "PyIter_Check of a static type's tp_iter result"),
            ('virtual', 'result', 'PySeqIter_Type'),
        ]},
    'Objects/abstract.c:PyIter_Next': {
        'shape': '47b86b4f3ac6',
        'ni': ['ni/Objects/pabstract.c:PsycoIter_Next'],
        'policy': [
            ('other', 'not_translation', "FOR_ITER's call: StopIteration is left set"),
        ]},
    'Objects/tupleobject.c:tuplesubscript': {
        'shape': '45d0b3343d67',
        'ni': ['ni/Objects/pabstract.c:psyco_generic_subscript'],
        'policy': [
            ('shortcut', 'inline_index', 'an int key'),
        ]},
    'Objects/listobject.c:list_subscript': {
        'shape': 'c025877a770a',
        'ni': ['ni/Objects/pabstract.c:psyco_generic_subscript', 'ni/Objects/plistobject.c:cimpl_list_slice', 'ni/Objects/plistobject.c:plist_subscript'],
        'policy': [
            ('promote', 'type', 'self'),
            ('restype', 'PyList_Type', 'a slice'),
        ]},
    'Objects/listobject.c:list_ass_subscript': {
        'shape': 'a38e24c79035',
        'ni': ['ni/Objects/pabstract.c:psyco_generic_ass_subscript', 'ni/Objects/plistobject.c:cimpl_list_ass_slice', 'ni/Objects/plistobject.c:plist_ass_subscript'],
        'policy': [
            ('shortcut', 'inline_index', 'an int key'),
            ('promote', 'type', 'item'),
        ]},
    'Objects/longobject.c:MEDIUM_VALUE': {
        'shape': '04cef22dc7ac',
        'ni': ['ni/Objects/plongobject.c:plong_medium_value'],
        'policy': [
            ('shortcut', 'word_compare', 'one unsigned compare for -1 <= ob_size <= 1'),
            ('likely', 'true', 'Py_ABS(Py_SIZE(x)) <= 1'),
            ('likely', 'false', 'Py_SIZE(x) == 0'),
        ]},
    'Objects/longobject.c:PyLong_AsLong': {'shape': '519d736695d6', 'ni': ['ni/Objects/plongobject.c:PsycoLong_AsLong']},
    'Objects/abstract.c:PyNumber_AsSsize_t': {
        'shape': '07bc4ee19b50',
        'ni': ['ni/Objects/plongobject.c:PsycoNumber_AsSsize_t'],
        'policy': [
            ('shortcut', 'const_fold', 'a constant'),
            ('shortcut', 'one_digit', 'a virtual or one-digit int'),
        ]},
    'Objects/longobject.c:PyLong_AsDouble': {'shape': '919d57a2e20b', 'ni': ['ni/Objects/plongobject.c:PsycoLong_AsDouble', 'ni/Objects/pfloatobject.c:cimpl_fp_from_long']},
    'Objects/longobject.c:PyLong_FromUnsignedLong': {
        'shape': '1538ca373a89',
        'ni': ['ni/Objects/plongobject.c:PsycoLong_FromUnsignedLong'],
        'policy': [
            ('restype', 'PyLong_Type'),
        ]},
    'Objects/longobject.c:CHECK_BINOP': {'shape': '9053880c8281', 'ni': ['ni/Objects/plongobject.c:both_longs']},
    'Objects/longobject.c:long_hash': {
        'shape': '111562f9a706',
        'ni': ['ni/Objects/plongobject.c:cimpl_long_hash1', 'ni/Objects/plongobject.c:plong_hash'],
        'policy': [
            ('shortcut', 'one_digit', 'cases -1, 0, 1'),
        ]},
    'Objects/longobject.c:long_bool': {'shape': '942117e8373b', 'ni': ['ni/Objects/plongobject.c:plong_bool']},
    'Objects/longobject.c:long_richcompare': {
        'shape': 'daafe12cf7d8',
        'ni': ['ni/Objects/plongobject.c:plong_richcompare'],
        'policy': [
            ('extra', 'one_digit', 'compare'),
            ('restype', 'PyBool_Type'),
        ]},
    'Objects/longobject.c:long_long': {
        'shape': '03f3faa2d90d',
        'ni': ['ni/Objects/plongobject.c:RETLONG', 'ni/Objects/plongobject.c:RETFLOAT'],
        'policy': [
            ('restype', 'PyLong_Type'),
        ]},
    'Objects/longobject.c:long_abs': {
        'shape': '2ea94da7dbeb',
        'ni': ['ni/Objects/plongobject.c:RETLONG', 'ni/Objects/plongobject.c:RETFLOAT'],
        'policy': [
            ('restype', 'PyLong_Type'),
        ]},
    'Objects/longobject.c:long_true_divide': {
        'shape': 'f1bb95e4bb2b',
        'ni': ['ni/Objects/plongobject.c:RETLONG', 'ni/Objects/plongobject.c:RETFLOAT'],
        'policy': [
            ('restype', 'PyFloat_Type'),
        ]},
    'Objects/longobject.c:long_and': {
        'shape': 'e9396bb7d788',
        'ni': ['ni/Objects/plongobject.c:plong_bitop'],
        'policy': [
            ('extra', 'one_digit', '& | ^ << >>'),
            ('likely', 'true', '<< count <= 32'),
        ]},
    'Objects/longobject.c:long_or': {'shape': 'f408ed157787', 'ni': ['ni/Objects/plongobject.c:plong_bitop']},
    'Objects/longobject.c:long_xor': {'shape': 'edd8cba493f3', 'ni': ['ni/Objects/plongobject.c:plong_bitop']},
    'Objects/longobject.c:long_lshift': {'shape': 'cf3e2dd05ef1', 'ni': ['ni/Objects/plongobject.c:plong_bitop']},
    'Objects/longobject.c:long_rshift': {'shape': 'e77da5e5ff78', 'ni': ['ni/Objects/plongobject.c:plong_bitop']},
    'Objects/longobject.c:fast_floor_div': {
        'shape': '76a601aa3d81',
        'ni': ['ni/Objects/plongobject.c:cimpl_floor_div', 'ni/Objects/plongobject.c:cimpl_floor_mod', 'ni/Objects/plongobject.c:floor_divmod_words'],
        'policy': [
            ('extra', 'power_of_two', 'a constant divisor: shift and mask'),
        ]},
    'Objects/longobject.c:fast_mod': {'shape': 'e79a04bdf1fb', 'ni': ['ni/Objects/plongobject.c:cimpl_floor_div', 'ni/Objects/plongobject.c:cimpl_floor_mod', 'ni/Objects/plongobject.c:floor_divmod_words']},
    'Objects/longobject.c:long_div': {
        'shape': 'dc868765a9fc',
        'ni': ['ni/Objects/plongobject.c:plong_divmod_op'],
        'policy': [
            ('extra', 'one_digit', 'a zero dividend, and divmod()'),
            ('virtual', 'result', 'PyTuple_Type of two PyLong_Type'),
        ]},
    'Objects/longobject.c:long_mod': {'shape': 'a22cbffd18a9', 'ni': ['ni/Objects/plongobject.c:plong_divmod_op']},
    'Objects/longobject.c:long_divmod': {'shape': '77555067fae7', 'ni': ['ni/Objects/plongobject.c:plong_divmod_op']},
    'Objects/longobject.c:long_pow': {'shape': '558e7ea17514', 'ni': ['ni/Objects/plongobject.c:plong_pow']},
    'Include/longobject.h:PyLong_Check': {'shape': '167ecaf64918', 'ni': ['ni/Objects/plongobject.h:PsycoLong_Check']},
    'Include/longintrepr.h:_longobject': {'shape': '8ccf94d3f22e', 'ni': ['ni/Objects/plongobject.h:LONG_ob_size', 'ni/Objects/plongobject.h:LONG_ob_digit0', 'ni/Objects/plongobject.h:iLONG_OB_DIGIT0']},
    'Objects/floatobject.c:float_richcompare': {
        'shape': '09e80dfabbf1',
        'ni': ['ni/Objects/pfloatobject.c:cimpl_fp_eq_fp', 'ni/Objects/pfloatobject.c:pfloat_richcompare'],
        'policy': [
            ('shortcut', 'type_whitelist', 'PyFloat_Type with PyFloat_Type'),
            ('restype', 'PyBool_Type'),
        ]},
    'Python/structmember.c:PyMember_GetOne': {
        'shape': '81f3827c0062',
        'ni': ['ni/Objects/pfloatobject.c:cimpl_fp_from_float', 'ni/Objects/pfloatobject.c:PsycoFloat_FromFloat', 'ni/Objects/pstructmember.c:PsycoMember_GetOne'],
        'policy': [
            ('ref', 'need_reference', 'T_OBJECT, T_OBJECT_EX'),
            ('likely', 'true', 'the member pointer is non-NULL'),
            ('restype', 'PyUnicode_Type', 'T_STRING, T_STRING_INPLACE'),
            ('other', 'mutable_fields', 'every struct member read as mutable'),
        ]},
    'Objects/floatobject.c:float___trunc___impl': {
        'shape': 'd5089ab6cb91',
        'ni': ['ni/Objects/pfloatobject.c:cimpl_fp_fits_long', 'ni/Objects/pfloatobject.c:cimpl_fp_to_long', 'ni/Objects/pfloatobject.c:cimpl_fp_trunc', 'ni/Objects/pfloatobject.c:PsycoFloat_Trunc', 'ni/Objects/pfloatobject.c:PsycoFloat_WholeToLong', 'ni/Python/pbltinmodule.c:float_whole_to_int'],
        'policy': [
            ('virtual', 'result', 'PyLong_Type'),
            ('likely', 'true', 'the double fits a long'),
        ]},
    'Objects/floatobject.c:float___round___impl': {'shape': '76fa6887b4b3', 'ni': ['ni/Objects/pfloatobject.c:cimpl_fp_round_even', 'ni/Objects/pfloatobject.c:PsycoFloat_RoundEven', 'ni/Python/pbltinmodule.c:float_whole_to_int']},
    'Objects/floatobject.c:CONVERT_TO_DOUBLE': {'shape': 'a29ca6d6c073', 'ni': ['ni/Objects/pfloatobject.c:CONVERT_TO_DOUBLE_CORE', 'ni/Objects/pfloatobject.c:CONVERT_TO_DOUBLE', 'ni/Objects/pfloatobject.c:RELEASE_DOUBLE', 'ni/Objects/pfloatobject.c:return_null', 'ni/Objects/pfloatobject.c:psyco_convert_to_double']},
    'Objects/floatobject.c:convert_to_double': {'shape': '8ba659cc76fe', 'ni': ['ni/Objects/pfloatobject.c:psyco_convert_to_double']},
    'Objects/floatobject.c:float_hash': {'shape': 'ee5774f0b8dd', 'ni': ['ni/Objects/pfloatobject.c:pfloat_hash']},
    'Objects/floatobject.c:float_float': {'shape': '7e1c71fb5f26', 'ni': ['ni/Objects/pfloatobject.c:pfloat_pos']},
    'Include/floatobject.h:PyFloatObject': {'shape': '1458f11765b5', 'ni': ['ni/Objects/pfloatobject.h:FLOAT_ob_fval', 'ni/Objects/pfloatobject.h:iFLOAT_OB_FVAL', 'ni/Objects/pfloatobject.h:FLOAT_TOTAL', 'ni/Objects/pfloatobject.h:PsycoFloat_Check']},
    'Include/floatobject.h:PyFloat_Check': {'shape': 'c550f09007dc', 'ni': ['ni/Objects/pfloatobject.h:FLOAT_ob_fval', 'ni/Objects/pfloatobject.h:iFLOAT_OB_FVAL', 'ni/Objects/pfloatobject.h:FLOAT_TOTAL', 'ni/Objects/pfloatobject.h:PsycoFloat_Check']},
    'Include/floatobject.h:PyFloat_AS_DOUBLE': {'shape': '1c13bbe6b768', 'ni': ['ni/Objects/pfloatobject.h:PsycoFloat_AS_DOUBLE']},
    'Objects/floatobject.c:PyFloat_AsDouble': {'shape': '0a58f1bcb013', 'ni': ['ni/Modules/pmath.c:PMATH_CONVERT_TO_DOUBLE', 'ni/Modules/pmath.c:PMATH_RELEASE_DOUBLE']},
    'Modules/mathmodule.c:math_1_to_whatever': {'shape': 'cd6f59f052aa', 'ni': ['ni/Modules/pmath.c:cimpl_math_1']},
    'Modules/mathmodule.c:is_error': {'shape': '4134a5ed9548', 'ni': ['ni/Modules/pmath.c:cimpl_math_1']},
    'Modules/mathmodule.c:FUNC1': {
        'shape': 'f0fd927ef403',
        'ni': ['ni/Modules/pmath.c:PMATH_FUNC1', 'ni/Modules/pmath.c:PMATH_TO_INT'],
        'policy': [
            ('virtual', 'result', 'PyFloat_Type'),
        ]},
    'Modules/mathmodule.c:math_1': {'shape': 'be63fd72b546', 'ni': ['ni/Modules/pmath.c:PMATH_FUNC1']},
    'Modules/mathmodule.c:math_1_to_int': {'shape': '6972e8ae844b', 'ni': ['ni/Modules/pmath.c:cimpl_floor_to_int', 'ni/Modules/pmath.c:cimpl_ceil_to_int']},
    'Modules/mathmodule.c:math_ceil': {
        'shape': 'eaf343d6c390',
        'ni': ['ni/Modules/pmath.c:PMATH_TO_INT'],
        'policy': [
            ('shortcut', 'type_whitelist', 'PyFloat_Type'),
            ('shortcut', 'skip_check', '_PyObject_LookupSpecial(__ceil__/__floor__)'),
        ]},
    'Modules/mathmodule.c:math_floor': {'shape': '4df6c1a0e122', 'ni': ['ni/Modules/pmath.c:PMATH_TO_INT']},
    'Python/bltinmodule.c:builtin_any': {
        'shape': '2eed9384b744',
        'ni': ['ni/Python/pbltinmodule.c:pbuiltin_any'],
        'policy': [
            ('other', 'untraced_only', 'generator-expression fusion (D46)'),
        ]},
    'Python/bltinmodule.c:builtin_all': {
        'shape': '9e99ee94cdba',
        'ni': ['ni/Python/pbltinmodule.c:pbuiltin_all'],
        'policy': [
            ('other', 'untraced_only', 'generator-expression fusion (D46)'),
        ]},
    'Python/bltinmodule.c:builtin_id': {'shape': '6cb6d3e675c7', 'ni': ['ni/Python/pbltinmodule.c:pbuiltin_id']},
    'Python/bltinmodule.c:builtin_globals_impl': {'shape': 'c7657d4e0e53', 'ni': ['ni/Python/pbltinmodule.c:pbuiltin_globals']},
    'Python/bltinmodule.c:builtin_len': {
        'shape': 'ed1073a2df2e',
        'ni': ['ni/Python/pbltinmodule.c:pbuiltin_len'],
        'policy': [
            ('virtual', 'result', 'PyLong_Type'),
        ]},
    'Python/bltinmodule.c:builtin_abs': {'shape': 'ce718b61be96', 'ni': ['ni/Python/pbltinmodule.c:pbuiltin_abs']},
    'Python/bltinmodule.c:builtin_divmod_impl': {'shape': 'd4ee89bf5868', 'ni': ['ni/Python/pbltinmodule.c:pbuiltin_divmod']},
    'Python/bltinmodule.c:builtin_hash': {
        'shape': 'beec34eaba7f',
        'ni': ['ni/Python/pbltinmodule.c:pbuiltin_hash'],
        'policy': [
            ('virtual', 'result', 'PyLong_Type'),
        ]},
    'Python/bltinmodule.c:builtin_pow_impl': {'shape': '602fb145c78e', 'ni': ['ni/Python/pbltinmodule.c:pbuiltin_pow']},
    'Objects/abstract.c:PyObject_IsInstance': {
        'shape': '3b2b3063c680',
        'ni': ['ni/Python/pbltinmodule.c:isinstance_known'],
        'policy': [
            ('shortcut', 'const_fold', 'an exact-type cls and a static type of obj'),
            ('limit', 'depth', '8 nested tuples, then the real call'),
            ('guard', 'no_fold_heap', 'cls or the object type a heap type: __bases__ can change'),
        ]},
    'Objects/abstract.c:recursive_isinstance': {'shape': '8505e1a6833c', 'ni': ['ni/Python/pbltinmodule.c:isinstance_known']},
    'Python/bltinmodule.c:builtin_isinstance_impl': {
        'shape': 'b53e4c00e3ee',
        'ni': ['ni/Python/pbltinmodule.c:pbuiltin_isinstance'],
        'policy': [
            ('promote', 'static_type', 'obj'),
            ('guard', 'constant_class', 'D62'),
            ('restype', 'PyBool_Type'),
        ]},
    'Python/bltinmodule.c:builtin_sum_impl': {
        'shape': '6adf2ab71762',
        'ni': ['ni/Python/pbltinmodule.c:pbuiltin_sum'],
        'policy': [
            ('promote', 'type', 'every item'),
            ('shortcut', 'known_length', 'a tuple'),
        ]},
    'Python/bltinmodule.c:builtin_round_impl': {
        'shape': 'f310ba644b35',
        'ni': ['ni/Python/pbltinmodule.c:pbuiltin_round'],
        'policy': [
            ('shortcut', 'type_whitelist', 'PyLong_Type, PyFloat_Type'),
            ('shortcut', 'skip_check', '_PyObject_LookupSpecial(__round__)'),
        ]},
    'Objects/typeobject.c:super_init': {'shape': '114fed12258f', 'ni': ['ni/Python/pbltinmodule.c:cimpl_super_noargs']},
    'Objects/typeobject.c:type_call': {
        'shape': 'af0c85d9198b',
        'ni': ['ni/Python/pbltinmodule.c:ptype_call'],
        'policy': [
            ('promote', 'static_type', 'the type called'),
            ('shortcut', 'type_whitelist', 'type(x), int(x), float(x) of PyLong_Type, PyFloat_Type'),
            ('likely', 'false', 'the callee is super'),
            ('virtual', 'result', 'PyRange_Type'),
        ]},
    'Objects/listobject.c:list_item': {
        'shape': '2c1e265db2fe',
        'ni': ['ni/Objects/plistobject.c:plist_item'],
        'policy': [
            ('virtual', 'fast_path', 'a virtual list and a constant index'),
            ('ref', 'need_reference', 'the item'),
        ]},
    'Objects/listobject.c:list_ass_item': {
        'shape': '3599fa4d4138',
        'ni': ['ni/Objects/plistobject.c:plist_ass_item'],
        'policy': [
            ('virtual', 'fast_path', 'a virtual list and a constant index'),
        ]},
    'Objects/listobject.c:list_concat': {
        'shape': 'ea616ecebd4c',
        'ni': ['ni/Objects/plistobject.c:psyco_plist_concat'],
        'policy': [
            ('promote', 'type', 'bb'),
            ('virtual', 'concat', 'two virtual lists'),
            ('restype', 'PyList_Type'),
        ]},
    'Objects/listobject.c:PyList_Append': {'shape': '5a78f1bd8020', 'ni': ['ni/Objects/plistobject.c:PsycoList_Append']},
    'Objects/tupleobject.c:tupleconcat': {
        'shape': '0d7660e5e007',
        'ni': ['ni/Objects/ptupleobject.c:PsycoTuple_Concat'],
        'policy': [
            ('promote', 'verify_type', 'a and bb'),
            ('shortcut', 'identity', 'an empty operand, the other an exact tuple'),
            ('virtual', 'concat', 'operands of known length'),
            ('restype', 'PyTuple_Type'),
        ]},
    'Objects/tupleobject.c:tupleitem': {'shape': 'e61481faa2e9', 'ni': ['ni/Objects/ptupleobject.c:ptuple_item']},
    'Objects/iterobject.c:seqiterobject': {'shape': '24ea9b846c56', 'ni': ['tools/metac/gen_layouts.py:copy_struct']},
    'Objects/object.c:PyObject_SelfIter': {'shape': 'ba0edf658c0b', 'ni': ['ni/Objects/piterobject.c:piter_getiter', 'ni/Objects/prangeobject.c:prangeiter_getiter']},
    'Objects/iterobject.c:iter_iternext': {
        'shape': 'f4f8c9902f51',
        'ni': ['ni/Objects/piterobject.c:cimpl_seqiter_drop_seq', 'ni/Objects/piterobject.c:piter_ends', 'ni/Objects/piterobject.c:piter_iternext'],
        'policy': [
            ('likely', 'true', 'the item fetch raised IndexError'),
            ('likely', 'false', 'it raised StopIteration'),
            ('guard', 'memory', 'it_seq cached, compared with memory; mismatch: residual iter_iternext (F48)'),
            ('shortcut', 'direct_meta', "a list's item by list_item"),
            ('promote', 'type', 'seq'),
        ]},
    'Objects/rangeobject.c:rangeobject': {'shape': '1534db80b98a', 'ni': ['tools/metac/gen_layouts.py:copy_struct']},
    'Objects/rangeobject.c:rangeiterobject': {'shape': 'd9be9f22f87f', 'ni': ['tools/metac/gen_layouts.py:copy_struct']},
    'Objects/rangeobject.c:get_len_of_range': {'shape': '966634e07870', 'ni': ['ni/Objects/prangeobject.c:cimpl_range_len']},
    'Objects/rangeobject.c:fast_range_iter': {'shape': '171e7bf3cd4f', 'ni': ['ni/Objects/prangeobject.c:cimpl_rangeiter_new']},
    'Objects/rangeobject.c:range_iter': {
        'shape': '2c98745a710e',
        'ni': ['ni/Objects/prangeobject.c:prange_iter'],
        'policy': [
            ('virtual', 'result', 'PyRangeIter_Type'),
        ]},
    'Objects/rangeobject.c:rangeiter_next': {
        'shape': '87272454bc0e',
        'ni': ['ni/Objects/prangeobject.c:prangeiter_next'],
        'policy': [
            ('virtual', 'result', 'PyLong_Type'),
            ('likely', 'true', 'r->index < r->len'),
        ]},
    'Objects/call.c:_Py_CheckFunctionResult': {
        'shape': '53925d685559',
        'ni': ['ni/Objects/pmethodobject.c:cimpl_null_result', 'ni/Objects/pmethodobject.c:cimpl_result_with_error', 'ni/Objects/pmethodobject.c:check_result'],
        'policy': [
            ('shortcut', 'inline_read', 'PyErr_Occurred as tstate->curexc_type'),
        ]},
    'Objects/call.c:PyCFunction_Call': {
        'shape': '2121dd458ebc',
        'ni': ['ni/Objects/pmethodobject.c:PsycoCFunction_Call'],
        'policy': [
            ('mega', 'residual', 'the callable (D43)'),
            ('promote', 'value', 'm_ml'),
        ]},
    'Objects/call.c:_PyMethodDef_RawFastCallDict': {'shape': 'ac01bf846e2e', 'ni': ['ni/Objects/pmethodobject.c:PsycoCFunction_Call', 'ni/Objects/pmethodobject.c:cimpl_call_fast', 'ni/Objects/pmethodobject.c:cimpl_call_fast_kw']},
    'Objects/funcobject.c:func_descr_get': {
        'shape': '86d34d7ac37f',
        'ni': ['ni/Objects/pfuncobject.c:pfunc_descr_get'],
        'policy': [
            ('virtual', 'result', 'PyMethod_Type'),
        ]},
    'Objects/classobject.c:method_call': {
        'shape': 'a1037f25ae8f',
        'ni': ['ni/Objects/pclassobject.c:pinstancemethod_call'],
        'policy': [
            ('likely', 'true', 'im_self != NULL'),
            ('virtual', 'args_tuple', 'self prepended'),
        ]},
    'Objects/descrobject.c:member_get': {'shape': '1e4827b8796b', 'ni': ['ni/Objects/pdescrobject.c:pmember_get']},
    'Objects/descrobject.c:method_get': {
        'shape': '234c927d374d',
        'ni': ['ni/Objects/pdescrobject.c:pmethod_get'],
        'policy': [
            ('virtual', 'result', 'PyCFunction_Type'),
        ]},
    'Objects/dictobject.c:PyDict_New': {
        'shape': '320e221a669d',
        'ni': ['ni/Objects/pdictobject.c:PsycoDict_New'],
        'policy': [
            ('restype', 'PyDict_Type'),
        ]},
    'Objects/dictobject.c:_PyDict_NewPresized': {
        'shape': '98e640685d57',
        'ni': ['ni/Objects/pdictobject.c:_PsycoDict_NewPresized'],
        'policy': [
            ('extra', 'small_presized', 'minused <= 5'),
            ('restype', 'PyDict_Type'),
        ]},
    'Objects/dictobject.c:PyDict_SetItem': {'shape': '5a5b634a6f8b', 'ni': ['ni/Objects/pdictobject.c:PsycoDict_SetItemC']},
    'Objects/dictobject.c:dict_length': {'shape': 'e490f691ac9c', 'ni': ['ni/Objects/pdictobject.c:psyco_dict_length']},
    'Objects/bytesobject.c:bytes_subscript': {
        'shape': '5c4dee939553',
        'ni': ['ni/Objects/pbytesobject.c:cimpl_bytes_slice', 'ni/Objects/pbytesobject.c:pbytes_subscript'],
        'policy': [
            ('promote', 'type', 'item'),
            ('shortcut', 'const_fold', 'a slice of a constant exact bytes (D56)'),
            ('restype', 'PyBytes_Type', 'a slice'),
            ('virtual', 'result', 'PyLong_Type'),
            ('likely', 'false', 'i < 0'),
        ]},
    'Objects/unicodeobject.c:PyUnicode_RichCompare': {
        'shape': 'c65fc8b707fc',
        'ni': ['ni/Objects/punicodeobject.c:both_str', 'ni/Objects/punicodeobject.c:cimpl_unicode_eq', 'ni/Objects/punicodeobject.c:punicode_richcompare'],
        'policy': [
            ('promote', 'known_only', 'both operands'),
            ('shortcut', 'const_fold', 'two constants'),
            ('virtual', 'result', 'PyBool_Type (condition code)'),
        ]},
    'Objects/unicodeobject.c:unicode_length': {
        'shape': '26b64b51971a',
        'ni': ['ni/Objects/punicodeobject.c:punicode_length'],
        'policy': [
            ('likely', 'true', 'PyUnicode_IS_READY'),
        ]},
    'Objects/unicodeobject.c:unicode_subscript': {
        'shape': '0ed1cb52e969',
        'ni': ['ni/Objects/punicodeobject.c:cimpl_unicode_item', 'ni/Objects/punicodeobject.c:cimpl_unicode_slice', 'ni/Objects/punicodeobject.c:punicode_subscript'],
        'policy': [
            ('promote', 'type', 'item'),
            ('shortcut', 'const_fold', 'a slice of a constant exact str (D56)'),
            ('restype', 'PyUnicode_Type', 'a slice'),
        ]},
    'Objects/unicodeobject.c:unicode_getitem': {
        'shape': 'bedc42ced6a6',
        'ni': ['ni/Objects/punicodeobject.c:cimpl_unicode_item', 'ni/Objects/punicodeobject.c:punicode_item'],
        'policy': [
            ('shortcut', 'inline_read', 'compact ASCII, or a compact constant'),
            ('likely', 'true', 'PyUnicode_IS_COMPACT_ASCII'),
            ('likely', 'false', 'index < 0'),
            ('restype', 'PyUnicode_Type'),
        ]},
    'Objects/unicodeobject.c:PyUnicode_Contains': {
        'shape': '500a01f02f96',
        'ni': ['ni/Objects/punicodeobject.c:punicode_contains'],
        'policy': [
            ('shortcut', 'const_fold', 'two constants'),
        ]},
    'Python/ceval.c:_PyEval_SliceIndex': {
        'shape': '2e166f506279',
        'ni': ['ni/Objects/psliceobject.c:slice_index'],
        'policy': [
            ('shortcut', 'const_fold', 'a constant int'),
            ('shortcut', 'one_digit', 'a virtual or one-digit int'),
            ('promote', 'type', 'v'),
        ]},
    'Objects/sliceobject.c:PySlice_Unpack': {
        'shape': 'd616af16ca35',
        'ni': ['ni/Objects/psliceobject.c:PsycoSlice_Unpack'],
        'policy': [
            ('virtual', 'argument', 'PySlice_Type with step None'),
        ]},
    'Python/ceval.c:_PyEval_EvalCodeWithName': {
        'shape': 'bd852b791f75',
        'ni': ['ni/psyfunc.c:fncall_collect_arguments', 'ni/psyfunc.c:fncall_store_arguments'],
        'policy': [
            ('virtual', 'args_tuple', '*args'),
        ]},
    'Include/ceval.h:Py_LeaveRecursiveCall': {'shape': '96958779c1fd', 'ni': ['ni/mechanisms/recursion/depth_up.h:cimpl_leave_overflowed', 'ni/mechanisms/recursion/depth_up.h:leave_recursive_call', 'ni/mechanisms/recursion/depth_up.h:ni_recursion_leave']},
    'Include/pystate.h:PyThreadState_GET': {'shape': '95c63709fb9d', 'ni': ['ni/mechanisms/tstate/runtime_address.h:ni_tstate_load']},
    'Include/ceval.h:_Py_MakeRecCheck': {'shape': 'bafb6442b39b', 'ni': ['ni/mechanisms/recursion/depth_up.h:add_to_depth']},
    'Include/ceval.h:Py_EnterRecursiveCall': {'shape': '6ad7744f89dc', 'ni': ['ni/mechanisms/recursion/depth_up.h:enter_recursive_call', 'ni/mechanisms/recursion/depth_up.h:ni_recursion_enter']},
    'Objects/call.c:_PyFunction_FastCallDict': {
        'shape': '4b4a040692f0',
        'ni': ['ni/psyfunc.c:COMPUTE_DEFCOUNT', 'ni/psyfunc.c:psyco_call_pyfunc', 'ni/psyfunc.c:MAX_CALL_FREES'],
        'policy': [
            ('likely', 'true', 'argdefs != NULL'),
            ('forget', 'direct_call'),
            ('ref', 'need_reference', 'cells of a run-time closure (C49)'),
            ('virtual', 'fuse', 'a called generator expression stays pending'),
            ('limit', 'max_frees', 'MAX_CALL_FREES'),
        ]},
    'Objects/call.c:function_code_fastcall': {'shape': 'e5dc42d0e67f', 'ni': ['ni/psyfunc.c:psyco_call_pyfunc', 'ni/psyfunc.c:MAX_CALL_FREES']},
    'Python/ceval.c:NEXTOPARG': {'shape': 'c465b25bec82', 'ni': ['ni/mergepoints.c:NEXTOPARG']},
    'Python/sysmodule.c:sys_getframe': {'shape': 'bba0db280d85', 'ni': ['ni/Python/frames.c:ni_getframe']},
}
