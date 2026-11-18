"""Why each hand-written definition that mirrors CPython is still
hand-written (D90): the one place such reasons are kept as data.
donecheck.py reads it; a definition with no reason here, no policy item in
policy_table.ENTRIES and no refusal from a generator fails GOAL's done test.

REASONS: 'ni/path:name' -> (kind, refs, why). refs name where the reason is
argued: 'D85', 'F48', 'P26', 'C11', or 'failures:NAME' for a row of
METACOMPILER.md's failures log that names NAME. donecheck.py checks every
key against what it finds in Ni and every reference against the docs. An
entry here wins over a policy item or a refusal, so it is also where a
definition the table or a generator would give a weaker reason is named
core.
"""

KINDS = {
    'core': "Ni's own machinery (calls, frames, the block stack, promotion,"
            " virtual objects and their representation, guards and their"
            " repair) with at most a fragment of CPython in it; MAP names it"
            " core (GOAL point 1)",
    'mechanism': 'a mechanism snippet, one per variant, chosen by the'
                 " source's answers (D75, D77)",
    'limit': 'the generator cannot read it yet: open work for the phase,'
             ' with the construct named in the failures log',
    'judgement': "Ni's choice to keep it hand-written, argued where the refs"
                 ' point',
    'helper': 'used only by hand-written definitions that have their own'
              ' reason; it goes when they go',
    'not_mirror': 'its name matches a CPython definition but it mirrors none',
}

# not read at all
EXCLUDED = {
    'ni/Objects/pstringobject.c': "Py2 str, compiled out on Py3 (MAP)",
    'ni/Objects/pstringobject.h': "Py2 str, compiled out on Py3 (MAP)",
}

REASONS = {
    # ---- the compiler loop and the call path (core by design) ----
    'ni/Python/pycompiler.c:psyco_pycompiler_mainloop': (
        'core', ['D58', 'D85'],
        "the compiler's opcode loop: dispatch, merge points, exceptions and"
        " the block stack; the generated cases are included into it"),
    'ni/psyfunc.c:psyco_call_pyfunc': (
        'core', ['D85'], "the direct call: inlining, recursion, tracing (D85 (5))"),
    'ni/psyfunc.c:fncall_bind': (
        'core', ['D85'], "runs the generated binder (psyfunc_gen.h)"),
    'ni/psyfunc.c:COMPUTE_DEFCOUNT': (
        'core', ['D85', 'F52'], "a compile-time defaults length behind F52's guard"),
    'ni/psyfunc.c:callee_builtins': (
        'core', ['F49'], "a compiled callee's builtins, fixed when it is compiled"),
    'ni/Python/pycompiler.c:psy_get_builtins': (
        'core', ['F49'], "the shared builtins of compiled frames"),
    'ni/Objects/pfuncobject.c:pfunction_call': (
        'core', ['D85'], "pfuncobject.c's call metas stay core (D85 (5))"),
    'ni/Objects/pfuncobject.c:PsycoFunction_NewWithQualName': (
        'core', [], "the virtual function MAKE_FUNCTION builds"),
    'ni/Objects/pfuncobject.c:cimpl_func_set': (
        'core', [], "forces a virtual function's optional fields"),
    'ni/Objects/pmethodobject.c:cimpl_call_fast': (
        'core', ['D85', 'D36', 'failures:_PyMethodDef_RawFastCallDict'],
        "pmethodobject.c's dispatch is emission strategy, not translation"),
    'ni/Objects/pmethodobject.c:cimpl_call_fast_kw': (
        'core', ['D85', 'D36', 'failures:_PyMethodDef_RawFastCallDict'],
        "pmethodobject.c's dispatch is emission strategy, not translation"),
    'ni/Objects/pmethodobject.h:PsycoCFunction_New': (
        'core', ['failures:method_get'], "the virtual builtin method"),
    'ni/Python/pycompiler.c:cimpl_generator_stop': (
        'core', ['D46'], "PEP 479 for fused generator expressions (D46)"),

    # ---- frames, tracebacks, the block stack (core) ----
    'ni/Python/pycompiler.c:block_setup': (
        'core', [], "the compile-time block stack (the ceval reader refuses"
        " block-stack cases as core)"),
    'ni/Python/pycompiler.c:block_pop': (
        'core', [], "the compile-time block stack"),
    'ni/Python/pycompiler.c:cimpl_pyerr_fetch': (
        'core', [], "Ni's stack holds None for NULL (a handler's exception triple)"),
    'ni/Python/pycompiler.c:cimpl_new_frame': (
        'core', ['C11'], "emulated frames for compiled code"),
    'ni/Python/pycompiler.c:cimpl_rt_traceback': (
        'core', ['C11'], "the emulated frame chain on an error"),
    'ni/Python/pycompiler.c:cimpl_vt_traceback': (
        'core', ['C11'], "a traceback for a virtual exception"),
    'ni/Python/pycompiler.c:PsycoTraceBack_Here': (
        'core', ['C11'], "chooses cimpl_rt_traceback or cimpl_vt_traceback"),
    'ni/Python/frames.c:ni_getframe': (
        'core', ['C11', 'F100'], "counts compiled frames for sys._getframe"),
    'ni/Objects/pabstract.c:PsycoObject_Call': (
        'core', ['D85', 'D77'],
        "the call path (D85 (5)): tp_call's meta behind the slot guard, with"
        " no recursion count or result check, which are the callee meta's"
        " and the recursion mechanism's; call.c's function and C-function"
        " arms are pfuncobject.c's and pmethodobject.c's metas"),
    'ni/mergepoints.c:NEXTOPARG': (
        'core', [],
        "mergepoints.c's own bytecode walk (MAP: core), decoding with the"
        " target's _Py_OPCODE/_Py_OPARG; the walk, not this macro, is what"
        " changes where the code format does (3.12's inline caches)"),
    'ni/Python/pycompiler.h:psyco_PyErr_Occurred': (
        'core', ['D77'], "the backend's inline error test; the field is the"
        " exceptions mechanism's"),

    # ---- representation and guards (core) ----
    'ni/Objects/pobject.c:Psyco_TypeIsSubtype': (
        'core', ['D54'], "the slot_guard rule's mro guard (RULES slot_guard"
        " applies it)"),
    'ni/Objects/pobject.c:cimpl_load_type_changed': (
        'core', ['D60'], "repairs a load guard at run time"),
    'ni/Objects/pobject.c:cimpl_generic_load_type_changed': (
        'core', ['D60'], "repairs a load guard at run time"),
    'ni/Objects/pobject.c:cimpl_store_attr_miss': (
        'core', ['D47'], "the store cache's miss refreshes it"),
    'ni/Objects/pobject.c:cimpl_store_type_changed': (
        'core', ['D39', 'D60'], "repairs a store guard at run time"),
    'ni/Objects/pobject.c:cimpl_store_miss': (
        'core', ['D38', 'D47'], "the inline store's miss path"),
    'ni/Objects/pfloatobject.h:FLOAT_ob_fval': (
        'core', ['D87'], "the double representation's field (TYPES double)"),
    'ni/Objects/pfloatobject.h:iFLOAT_OB_FVAL': (
        'core', ['D87'], "the double representation's field (TYPES double)"),
    'ni/Objects/pfloatobject.h:FLOAT_TOTAL': (
        'core', ['D87'], "the double representation's size"),
    'ni/Objects/pfloatobject.h:PsycoFloat_Check': (
        'core', ['D87'], "a type test of the representation"),
    'ni/Objects/pfloatobject.h:PsycoFloat_AS_DOUBLE': (
        'core', ['D87'], "TYPES double 'load': Ni's function the generator calls"),
    'ni/Objects/pfloatobject.h:PsycoFloat_FromDouble': (
        'core', ['D87'], "the virtual float (TYPES double 'new')"),
    'ni/Objects/plongobject.h:PsycoLong_Check': (
        'core', ['D83'], "a type test of the word representation"),
    'ni/Objects/plongobject.h:LONG_ob_size': (
        'core', ['D83'], "the word representation's fields"),
    'ni/Objects/plongobject.h:LONG_ob_digit0': (
        'core', ['D83'], "the word representation's fields"),
    'ni/Objects/plongobject.h:iLONG_OB_DIGIT0': (
        'core', ['D83'], "the word representation's fields"),
    'ni/Objects/plongobject.c:PsycoLong_FromLong': (
        'core', ['D83'], "TYPES word 'constructors': the virtual int of a word"),
    'ni/Objects/plongobject.c:PsycoLong_FromSsize_t': (
        'core', ['D83'], "the virtual int of a word"),
    'ni/Objects/plistobject.c:PsycoList_New': (
        'core', ['D80', 'failures:PsycoList_New'],
        "a list's virtual form is a vlist, not the struct's fields"),
    'ni/Objects/piterobject.c:cimpl_seqiter_exhausted': (
        'core', [], "materialises an exhausted virtual iterator"),
    'ni/Objects/piterobject.h:PsycoSeqIter_New': (
        'core', [], "the virtual sequence iterator"),
    'ni/Objects/prangeobject.c:cimpl_range_new': (
        'core', [], "forces a virtual range"),
    'ni/Objects/ptupleobject.h:PsycoTuple_GET_ITEM': (
        'core', [], "a virtual tuple's vinfo array"),
    'ni/Objects/ptupleobject.h:PsycoTuple_SET_ITEM': (
        'core', [], "a virtual tuple's vinfo array"),

    # ---- mechanisms (one snippet per variant) ----
    'ni/mechanisms/eval_breaker_runtime/ceval_loop_top.h:ni_eval_breaker_handler': (
        'mechanism', ['D77'], "eval_breaker_runtime: ceval_loop_top"),
    'ni/mechanisms/eval_breaker_runtime/ceval_loop_top.h:ni_eval_breaker_at_entry': (
        'mechanism', ['D77'], "eval_breaker_runtime: ceval_loop_top"),
    'ni/mechanisms/exceptions/curexc_triple.h:ni_exc_info_topmost': (
        'mechanism', ['D77'], "exceptions: curexc_triple"),
    'ni/mechanisms/recursion/depth_up.h:ni_recursion_enter': (
        'mechanism', ['D77'], "recursion: depth_up"),
    'ni/mechanisms/recursion/depth_up.h:ni_recursion_leave': (
        'mechanism', ['D77'], "recursion: depth_up"),
    'ni/mechanisms/recursion/depth_up.h:cimpl_leave_overflowed': (
        'mechanism', ['D77'], "recursion: depth_up"),
    'ni/mechanisms/recursion/depth_up.h:add_to_depth': (
        'mechanism', ['D77'], "recursion: depth_up"),
    'ni/mechanisms/recursion/depth_up.h:enter_recursive_call': (
        'mechanism', ['D77'], "recursion: depth_up"),
    'ni/mechanisms/recursion/depth_up.h:leave_recursive_call': (
        'mechanism', ['D77'], "recursion: depth_up"),
    'ni/mechanisms/tstate/runtime_address.h:ni_tstate_load': (
        'mechanism', ['D77'], "tstate: runtime_address"),

    # ---- generator limits, each a failures-log row ----
    'ni/Python/pycompiler.c:case BUILD_CONST_KEY_MAP': (
        'limit', ['failures:BUILD_CONST_KEY_MAP'],
        "the keys check folds where Ni knows keys (type_test, ct_if); where"
        " it doesn't, a run-time check would leave the loop reading the items"
        " of a tuple Ni has not loaded (PyTuple_GET_ITEM of a run-time"
        " tuple), which has no form without a whole-case run-time copy;"
        " LOAD_CONST always gives Ni the constant, the only case the hand"
        " code takes"),
    'ni/Python/pycompiler.c:case IMPORT_NAME': (
        'core', ['failures:IMPORT_NAME'],
        "import_name takes the frame (f_builtins, f_globals, f_locals):"
        " frame state, as LOAD_GLOBAL"),
    'ni/Objects/pdictobject.c:PsycoDict_SetItemC': (
        'limit', ['failures:BUILD_CONST_KEY_MAP'],
        "the hand BUILD_CONST_KEY_MAP's store of a compile-time key"),
    'ni/Objects/prangeobject.c:cimpl_range_len': (
        'limit', ['failures:get_len_of_range'], "returns unsigned long"),
    'ni/Python/pycompiler.c:unpack_iterable': (
        'limit', ['failures:unpack_iterable'], "writes outputs through the"
        " stack pointer; for the hand UNPACK_SEQUENCE and UNPACK_EX"),
    'ni/Python/pycompiler.c:cimpl_unpack_iterable': (
        'limit', ['failures:unpack_iterable'], "as unpack_iterable"),
    'ni/Python/pycompiler.c:cimpl_import_name': (
        'limit', ['failures:import_name'], "import_name takes the frame and"
        " a local array; for the hand IMPORT_NAME"),
    'ni/Modules/pmath.c:cimpl_floor_to_int': (
        'limit', ['D87', 'failures:float___trunc___impl'],
        "an int result from a double: ceil and floor stay hand-written"),
    'ni/Modules/pmath.c:cimpl_ceil_to_int': (
        'limit', ['D87', 'failures:float___trunc___impl'],
        "an int result from a double: ceil and floor stay hand-written"),

    'ni/Objects/pobject.c:PsycoObject_Hash': (
        'limit', ['F50', 'failures:object.c:PyObject_Hash'],
        "refused: no 'likely' item for the tp_dict == NULL arm (PyType_Ready);"
        " past it, tp_hash read behind F50's heap-type guard (Psyco_TypeSlot),"
        " which the translator doesn't emit"),
    'ni/Objects/pobject.c:PsycoObject_RichCompareBool': (
        'limit', ['F109', 'failures:object.c:PyObject_Hash'],
        "the translation resumes at its first statement, v == w on two"
        " objects (F109: a virtual is identical only to itself); then the"
        " result's truth without a promotion"),
    'ni/Objects/pobject.c:try_rich_compare': (
        'limit', ['F50', 'failures:object.c:PyObject_Hash'],
        "do_richcompare: refused (resuming needs res, which has no value);"
        " its slot calls want F50's heap-type guard"),
    'ni/Objects/plongobject.c:PsycoLong_AsDouble': (
        'limit', ['D87', 'failures:longobject.c:PyLong_AsDouble'],
        "the word test on a PyObject *, and no list of the long unit's"
        " declared functions to translate"),
    'ni/Objects/pfloatobject.c:cimpl_fp_from_long': (
        'limit', ['D87', 'failures:longobject.c:PyLong_AsDouble'],
        "PsycoLong_AsDouble's helper for a virtual int's word"),

    # ---- judgement ----
    'ni/Python/pbltinmodule.c:pbuiltin_globals': (
        'judgement', ['D86', 'F47', 'failures:builtin_globals_impl'],
        "the compiled frame's globals, not PyEval_GetGlobals"),

    # ---- helpers of hand-written definitions with their own reason ----
    'ni/Objects/pobject.h:PyType_TypeCheck': (
        'helper', [], "PyObject_TypeCheck on a PyTypeObject, as Ni holds"
        " types; of PsycoLong_Check, PsycoFloat_Check and hand metas"),
    'ni/Objects/pabstract.c:type_error_tp': (
        'helper', [], "of PsycoObject_GetIter and PsycoObject_Call"),
    'ni/Objects/pfloatobject.c:cimpl_fp_ne_fp': (
        'helper', ['P26'], "of pfloat_richcompare (its float/float whitelist)"),
    'ni/Objects/pfloatobject.c:cimpl_fp_le_fp': (
        'helper', ['P26'], "of pfloat_richcompare (its float/float whitelist)"),
    'ni/Objects/pfloatobject.c:cimpl_fp_lt_fp': (
        'helper', ['P26'], "of pfloat_richcompare (its float/float whitelist)"),
    'ni/Objects/pfloatobject.c:cimpl_fp_round_even': (
        'helper', ['D86'], "of pbuiltin_round"),
    'ni/Objects/pfloatobject.c:PsycoFloat_RoundEven': (
        'helper', ['D86'], "of pbuiltin_round"),
    'ni/Objects/plongobject.c:both_longs': (
        'helper', [], "of plong_bitop, plong_richcompare and plong_divmod_op"),
    'ni/Objects/plongobject.c:plong_div': (
        'helper', [], "binds the slot to plong_divmod_op"),
    'ni/Objects/plongobject.c:plong_mod': (
        'helper', [], "binds the slot to plong_divmod_op"),
    'ni/Objects/plongobject.c:plong_divmod': (
        'helper', [], "binds the slot to plong_divmod_op"),
    'ni/Objects/plistobject.c:PsycoList_Append': (
        'helper', [], "of the hand YIELD_VALUE's fused list comprehension"),
    'ni/Objects/pobject.c:PsycoObject_GenericGetAttr': (
        'helper', [], "registers generic_getattr as PyObject_GenericGetAttr's meta"),
    'ni/Objects/pobject.c:swapped_op': (
        'helper', [], "of try_rich_compare"),
    'ni/Objects/pobject.c:opstrings': (
        'helper', [], "of try_rich_compare"),
    'ni/Objects/prangeobject.c:cimpl_rangeiter_new': (
        'helper', [], "of prange_iter"),
    'ni/Python/pbltinmodule.c:pbuiltin_min': (
        'helper', [], "binds min to pbuiltin_min_max"),
    'ni/Python/pbltinmodule.c:pbuiltin_max': (
        'helper', [], "binds max to pbuiltin_min_max"),
    'ni/Python/pbltinmodule.c:cimpl_super_noargs': (
        'helper', [], "of ptype_call's super()"),
    'ni/Python/pycompiler.c:special_lookup': (
        'helper', [], "of cimpl_setup_with (SETUP_WITH, the block stack)"),
    'ni/Python/pycompiler.c:cimpl_import_all_from': (
        'helper', [], "of the hand IMPORT_STAR (frame state: core)"),

    # ---- name matches that mirror nothing ----
    'ni/ni.c:psyco_fatal_error': (
        'not_mirror', [], "Ni's own fatal error"),
    'ni/vcompiler.c:psyco_compiler_init': (
        'not_mirror', [], "initialises Ni's compiler"),
}
