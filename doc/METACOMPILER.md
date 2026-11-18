---
written: 2026-11-16
rewritten: 2026-11-18 (rules only; reports, measurements and history in git: `git show e404100:doc/METACOMPILER.md`)
---
# Meta-compiler: how the generator works

The aim is in `GOAL.md` (long-term aim) and `DECISIONS.md` D16–D20. The patterns it must produce are in `PATTERNS.md`; the plan of work in `GENERATION.md`.

**Rule: use the simplest thing that works.** Move to a heavier tool only when the current one demonstrably fails on real input. When that happens, record the failure in the failures log below, with the CPython construct that caused it, before you switch.

## Versions (D67)

One Ni source serves every CPython version from 3.7 on; none is dropped.

- **Hand-written, version-free:** the core, the backend (with `llvm-mc` templates, D65), and the generator's rules: the policy table (what to promote, make virtual, guard), representation rules, effect rules. No `#if` on versions anywhere.
- **Generated, per build:** everything that mirrors CPython, from the source of the version being built for: bytecode cases, slots, `abstract.c`, builtins, the call path, layouts, virtual objects' constructors. Not checked in.
- **Bytecode input:** `bytecodes.c` through CPython's parser for 3.12+; for 3.7–3.11 a reader of `ceval.c`'s `TARGET` cases that produces the same form, so everything after it is shared.
- **Mechanisms:** where Ni itself must work differently (entry, tracing, frames…), a detector looks in that version's source and the matching snippet goes into the generated source (D75, "Mechanisms").
- **New features** go into the core or the rules, never into one version's output. Each version's tests run on every change.
- **Order:** generate everything possible on 3.7 first, compared with the hand translations, then 3.8, 3.9 and on.
- **At build time** (D68): `setup.py` runs `tools/metac/generate.py --python EXE --out DIR`, which finds the source for EXE's version (`NI_CPYTHON_SRC` or the cached, checksum-checked tarball, `cpython_src.py`), runs the policy check, then each generator in its `GENERATORS` list. A generator is a module with `build(target)` returning `(header text, report lines)`, or raising `cslots.GenError` to stop the build; `target` is `cslots.Target(src, version, python, pyconfig)`. The headers go to the build directory, never into `ni/`. `--past-policy` runs past the policy table's problems, for a dry run (PORT38.md).

## Stages (D19)

| Stage | Output | Approach |
|---|---|---|
| 0 | Field descriptors and layouts of the object structs ("Stage 0: layouts") | Token-scan the struct declarations; generated code uses `offsetof()`/`sizeof()` with a `static_assert` per assumption, so a layout change fails the build. |
| 1 | Compile-time versions of a small, regular slice | Token-level rules in plain Python. Since D74/D83/D87 the general translator does this too. |
| 2 | Arbitrary C functions ("General translation") | Analysis added only for the failures seen: binding times, branches on run-time values, ownership. |
| Bytecode | The opcode cases (`pycompiler_gen.h`) | CPython's own `Tools/cases_generator` parser, vendored, then templated bodies. |

Token rewriting means working on C tokens (comments, strings and preprocessor lines handled correctly, `tools/metac/ctokens.py`), never on raw text.

## Tooling options for when the simple approach stops working

| Option | Good for | Costs | Switch when |
|---|---|---|---|
| Token rules and `cparse.py` (current) | Mechanical patterns, struct scanning, statements and expressions | No full C types | A failure the reader can't express |
| pycparser + stub headers | Real ASTs, clear parse errors | Needs preprocessed input and stub headers for CPython's macros; comments lost | Token rules can't express a pattern, or declared types or nested expressions are needed |
| Coccinelle (`spatch`) | Matching across control flow, macros unexpanded | OCaml and SmPL; built for in-place rewrites | Many syntactic variants of one pattern |
| `Tools/cases_generator` | The bytecode front end | Tied to upstream's DSL, which is the point | Default for the bytecode stage |
| clang (libclang/LibTooling) | Anything the others can't do | Large; much to learn and teach (D20) | Last resort, for a proven need |

The C preprocessor is already used where token rules were wrong (`cparse.cpp`, "General translation"); pycparser has not been needed.

## Bytecode stage

**3.12's DSL** (desk read of 3.12.9's `bytecodes.c`) [read]: every instruction in `bytecodes.c` declares its stack effect and error exits (`ERROR_IF`, `DECREF_INPUTS()`), and "until the last `DEOPT_IF`, no objects may be allocated, `INCREF`ed, or `DECREF`ed" is Psyco's restart rule (PATTERNS rule 1). Its specialised family members are a guard plus a fast path: specialisation knowledge, not code to translate (QUESTIONS 30). Its globals guard (`LOAD_GLOBAL_MODULE`/`BUILTIN`) uses the dict keys version (`ma_keys->dk_version`) plus an entry index, not `ma_version_tag` (QUESTIONS 29). **3.7** states none of this: the `ceval.c` reader infers the same form, so the back half is shared. Calls, frames, generators and exceptions are core on every version.

**The back half** (`tools/metac/gen_ceval.py`, writes `pycompiler_gen.h`, included in `pycompiler.c`'s opcode switch):
- **Shape of a case:** inputs read with `NTOP` and left on the stack, then the body; nothing popped or released until the end (top first), so an error exit is `break`, leaving the inputs to the unwinding code.
- **Vocabulary:** `GETLOCAL`/`SETLOCAL`, `GETITEM(consts, oparg)`, `Py_INCREF`, `Py_DECREF` of an input, `Py_NewRef`, `ERROR_IF`, `X = F(args)`; `assert`, `STAT_INC` and `#if ENABLE_SPECIALIZATION` dropped. Anything else is skipped with the reason.
- **Counterparts by declaration, not a table** (`gen_abstract.psyco_name`): `F` in CPython's `Include/*.h`, `Psyco` + `F[2:]` in a `ni/Objects/p*.h` with `po` first; `PyObject *` pairs with `vinfo_t *` (NULL is error), `int` with `bool`. One without `po` returning a `vinfo_t *` builds a virtual and can't fail. None, or another arity, is a residual call with the flags `gen_abstract.Translator.flags` computes.
- **Representation:** a local is unbound only if it is a known NULL; `SETLOCAL(oparg, NULL)` stores `psyco_vi_Zero()`; a code-object constant is a compile-time vinfo; `Py_None` an argument is a `psyco_vi_None()` temporary.
- **Restart rule, checked:** no irreversible statement (incref, `SETLOCAL`) before the last call or `ERROR_IF`, and no owned temporary live at an error exit.
- **PATTERNS E:** `X = PyT_New(n)`, its `ERROR_IF` and the loop filling it from an input array become `PsycoT_New([po,] n, values)`, then the array is released.
- **Checked in every generation:** each effect read equals `dis.stack_effect` in the target interpreter at 13 opargs (0 to 513); a mismatch stops the build. `--try` runs the back half on 3.12's `bytecodes.c`.

## `ceval.c` reader (D69)

`tools/metac/ceval_reader.py` reads the `TARGET(op)` cases of the source's `Python/ceval.c` and gives `gen_ceval.py` the `InstDef`s CPython's parser gives for a `bytecodes.c`: it writes each case in that syntax (`inst(NAME, (ins -- outs)) { body }`) and parses it with the vendored parser. `gen_ceval.definitions` picks the front end by what the source has, not by version. `python3 tools/metac/ceval_reader.py SRC` prints what it reads.

**How a case is read** (D69, D73, D79, D93): a symbolic run over its statements.
- **Positions** are linear forms in oparg (`Lin`) from the entry stack pointer; a non-linear expression in oparg is one atom; `int` locals set from such expressions stand for their value; `#define` and `enum` constants come from the headers and `ceval.c`. Positions are ordered by their difference, at sixteen sample opargs if not constant; a sign that changes is refused.
- **Stack:** `POP`, `TOP`, `SECOND`…, `PEEK(n)` read slots; `PUSH`, `SET_TOP`…, `SET_VALUE` write them; `STACKADJ`, `STACK_GROW`, `STACK_SHRINK`, `stack_pointer += n` move the pointer. The touched positions cut the stack into intervals: inputs (named by their variable, `values[n]`, `args[n]` if a helper popped them, `unused[n]` if left in place) and outputs. A 0/1 size is conditional (`step if (oparg == 3)`). Refused: reading a slot it wrote or popped, two names for one input, overwriting or dropping an unread input, pushing an unset slot, an array cut in two.
- **Loops** popping k values per turn on `oparg`, or reading `PEEK(a * i + b)`, read input arrays; a loop releasing only part of what it pops is refused. **Pointers:** `stack_pointer - n` handed to a call reads n slots; `stack_pointer + n` then `STACKADJ(n)` are n outputs the call wrote. **Helpers:** a stack-pointer copy handed by address to a `ceval.c` function moves by what its body pops.
- **Branches:** a compile-time condition whose arm starts `x = POP()` is a conditional input; a branch on `opcode` folds in its own `TARGET`; any other branch that touches the stack, jumps or rebinds an input runs as separate paths, each checked, agreeing where they meet.
- **Error exits:** `if (C) goto error;`, or input releases plus at most one known error statement, is `ERROR_IF(C, label)`; any other block is `if (C) { statements; ERROR_IF(true, error); }`. Each input on every path to an exit is in its slot, or released or stored, never both.
- **Generic path** (`type_test_generic_path`, judgement): `if (T) A else B` where T only tests types (and an exact type's size against oparg) keeps B: CPython's exact-type fast paths give the generic call's result, and Psyco's counterpart dispatches on the type itself.
- **Identity fast paths** (`singleton_test_generic_path`): `if (x == K) { A; DISPATCH(); }`, K a singleton, is dropped when the rest calls a function whose body starts by answering K (`PyObject_IsTrue`, read from `object.c`) and then does A; checked against the C.
- **Arms that end the case:** `if (C) { A; DISPATCH(); } REST` is `if (C) A else { REST }`; an if-chain on an `int` whose last `else` is an error exit tests the error first. **Jumps:** `JUMPTO`/`JUMPBY` mark their path; slots only jumping paths keep are outputs `x if (jump)`; a `#if` whose arms each dispatch is a dispatch. **Falls through** (`fall_through`): a `TARGET` falling into the next is read with that body. **Duplicates** get a new name and `Py_NewRef`.

**Refused, by kind:** core (block stack, frame state `f->`, `tstate`), shared code (`fast_block_end`, `WHY_*`, `fast_yield`, `dispatch_opcode`), stack (`PUSH(NULL)`, arms leaving different stacks, an error exit with a pushed value or an unreleased input), control flow (`goto error` after its own cleanup). `-v` lists each opcode with its kind.

**The back half's rules for read cases** (most named in `policy_table.RULES`):
- **Run-time ints** (P05): `int x = F(…)` whose counterpart returns a `vinfo_t *` is a run-time int, NULL where C's is negative; a branch on it is `gen_abstract.int_condition`. A condition code is never held across a call. `bool_select` (P23): arms making one output `Py_True` or `Py_False` are `PsycoBool_FromCondition`; `identity_compare`: `v == w` on objects is `integer_cmp`.
- **Jumps:** `JUMP_IF_CC(cc, JUMPTO(oparg), npop, forget)` (npop at the target by `NEXT_INSTR_POP`; forget from `('forget', 'before_branch')`), or `JUMPED()`; both core macros in `pycompiler.c`.
- **Loops and compiler ints** (D81): `gen_abstract.loop_rule`; an error exit inside releases what the turn owns and jumps to `error_<name>`; items released once, checked at opargs 0–4 and 7. Locals from oparg, opcode and constants are compiler ints (`ct_eval`). `restype` is `Psyco_AssertType`; `freevars[i]` is `GETLOCAL(co->co_nlocals + i)` (`frame_arrays`).
- **Helpers** (D82, D88): `inline_helpers` (`ceval_inline.py`) puts a static `ceval.c` helper's body in place (a `switch` an if-chain, `return E` as an assignment; on 3.8 `_PyX(tstate, …)` as public `PyX`); `helper_copy` calls one it refuses as a run-time copy `ceval_<name>` (`pycompiler_copies_gen.h`; object parameters, public API, string macros only); `compile_time_copy` runs a helper's body in the compiler with Ni's compile-time exception. With `('promote', 'known_only')`, an arm testing a parameter's type is resumable (`NI_RESUME`): Ni's known type, else a residual call of the copy.
- **Fields:** a `TYPES`/`LAYOUTS` field read is `psyco_get_field` with Ni's descriptor (`FMUT` matching `TYPES`); `field_store` is `psyco_put_field`, the reference going to the field. A `Py_XDECREF`/`Py_DECREF` of a value read at run time is the core's `psyco_xdecref_v`/`psyco_decref_v`, setting `class_unchecked` unless the type is in `NO_DEL_TYPES` (F55).
- **`error_handler`:** `if (F(args) OP V) { H; releases; goto error; }`, or an assigned call tested for NULL, is a wrapper that runs H on failure. **`residual_wrapper`:** an input array goes as up to `ARRAY_ARGS_MAX` words, past that a virtual tuple with count -1 (`('limit', 'max_args')`); a call of constants only feeding the next is made inside. **`switch_arms`:** a `switch` on a compiler int copies the rest into each arm, a function pointer set in an arm is that function. **`type_test_generic`:** with `('shortcut', 'specialise_only')` a type test is false where Ni doesn't know the type. **`concat_shortcut`:** `('virtual', 'concat')` is P22's virtual tuple where every part's length is known.
- **Smaller:** an input moved to a local carries Ni's reference; one assigned after its release is a new local; an absent conditional input is NULL to a residual call; residual ints the C never tests are `CfReturnTypeInt` plus the effect flags.

Calls, the block stack and exceptions are core (D58, D85); what else is hand-written is in `hand_table.py`.

## Policy table (D71)

The generator's judgement is data: `tools/metac/policy_table.py`, checked by `tools/metac/policy.py`, which `generate.py` runs before any generator, so a mismatch fails the build. `policy.py --src SRC` runs the checks alone; `--shape KEY…` prints a definition's current shape.

**Keys and shapes.** A key names a definition in CPython's source (`tools/metac/csource.py`): `path:name` for a function, macro, struct (tag or typedef) or initialised variable; `Python/ceval.c:TARGET(OP)` for an opcode case. An entry's `shape` is the first 12 hex digits of a sha256 of the definition's tokens (comments and spacing don't count). A shape that differs means the C changed: the check names the key and both shapes, and someone reads the entry again against the new C and adds the new shape. `shape` may be a tuple, one per source, so no version number is written anywhere (D67). Ni's side is named the same way (`ni/Python/pycompiler.c:case LOAD_GLOBAL`) and must exist.

**Sections:** `ENTRIES` (one per CPython definition a hand translation mirrors: shape, the Ni code that carries it, policy items `(kind, value[, note])`); `RULES` (what is one rule rather than per-function items, with its decisions and where Ni applies it); `LIMITS` (Ni's numbers: `MEGAMORPHIC_MAX`, `MP_BLOCK_CAP`, `MAX_MP_DISTANCE`, `INLINE_MAXIMUM_WEIGHT`, `VLIST_LENGTH_MAX`, `CT_TUPLE_LOAD_SIZE_LIMIT`, `FAST_MAX`, `MAX_CALL_FREES`, `STORE_CACHE_MAX_REFRESHES`, `ARRAY_ARGS_MAX`); `LIGHT_OPCODES` (not counted in the inlining weight); `TYPES` (per type struct: type object, mutable and immutable fields, representation, virtual form and weights); `LAYOUTS`, `RUNTIME` (stage 0); `CANNOT_BLOCK` (D53's list as CPython functions); `NO_DEL_TYPES`, `SAFE_DESCRIPTORS` (F55's decref rule, attribute specialisation); `SEEDS`, `EXTERNAL`, `ASSUMPTIONS` (effect rules); `TRANSLATED_SLOTS`, `TRANSLATED_FUNCTIONS`, `SLOT_ARGS`, `METH_ARGS`, `SCALAR_CODE` (what the units translate and how Ni calls it).

**Vocabulary** (`policy.VOCABULARY`, the authority, with each value's meaning): `promote` (`type`, `verify_type`, `static_type`, `value`, `constant`, `known_only`); `mega` (`type`, `value`, `residual`); `guard` (`version_tag`, `store_guard`, `dict_version`, `first_seen`, `memory`, `identity`, `constant_class`, `no_fold_heap`, `generic_for_heap`); `virtual` (`result`, `argument`, `fast_path`, `concat`, `fuse`, `args_tuple`); `shortcut` (in this C) and `extra` (no C behind it in this source, so the generator must emit it from the table or same code shows a loss), both over `one_digit`, `const_fold`, `type_whitelist`, `inline_index`, `inline_read`, `no_tuple_call`, `known_length`, `identity`, `power_of_two`, `small_presized`, `word_compare`, `skip_check`, `specialise_only`, `compile_time_unpack`, `direct_meta`, `if_known`, `zero_or_one`, `bool_word`; `likely` (`true`, `false`; the note names the condition as the C wrote it); `restype` (a type object); `ref` (`need_reference`, `held`, `no_steal`, `incref`, `keep_alive`, `null_arg`); `forget` (`before_branch`, `direct_call`, `handler`, `store`); `limit` (`nested_weight`, `max_args`, `depth`, `max_frees`); `other` (`compile_time_hash`, `ends_block`, `untraced_only`, `trace_fallback`, `mutable_fields`, `frame_binding`, `unpack_arguments`, `not_translation`). Items whose meaning is a translator rule are described where the rule is.

**`RULES`** hold, each with its text: `slot_guard` (D54: every slot read on a possibly-heap type through `Psyco_TypeSlot`), `restart` (PATTERNS rule 1), `error_unlikely` (P05), `effects`, `forget` (D59, D62, D63, F55), `megamorphic` (P04, D43, D66), `eval_breaker` (D45, D53), `gc_ignored`, `allocator_pure`, `virtual_types`, `loops`, and the reader's and translator's rules named in the sections below. A hand translation's drift from 3.7 is not policy: a faithful translation loses it.

**Checks against Ni's code.** Where the table copies something Ni still hard-codes, the two must agree until a generator emits it: `LIMITS` against the `#define`s; `LIGHT_OPCODES` against `mergepoints.c:IS_LIGHT_INSTR` and `Include/opcode.h`; `CANNOT_BLOCK` against `psyfunc.c:cannot_block_init`; `NO_DEL_TYPES` against `pobject.c:collect_undeletable_vars` and `ipyencoding.h:decref_may_run_python`; `SAFE_DESCRIPTORS` against `pobject.c:c_descriptor`/`safe_descriptor`; `TYPES` against the struct, `INIT_SVIRTUAL`'s weights, Ni's descriptors and `FMUT`; `SLOT_ARGS` against each Ni caller. Unknown keys, values or Ni names fail by name.

## Effect rules (D71)

`tools/metac/effects.py --python PY NAME…` prints why each flag holds or not; the build calls `effects.analyse` for the flags of generated residual calls. It reads every file Makefile.pre.in links into libpython, Setup's built-in modules and every mirrored file through the front end (`front.py`), so only the compiled `#if` arms count and macros are expanded. Each function gets five flags [read]:
- **pure:** its result depends only on its arguments' values; any read through a pointer or of a non-`const` file-scope or `static` variable breaks it.
- **readonly:** it writes nothing but its own locals (refcounts aside).
- **no_python**, **no_block** (doesn't release the GIL), **no_raise**.

A call through anything but a function's name, a callee declared in CPython's headers but defined in no file read, and GNU `asm` break every flag; a function named without a call is a callee. A function declared only by a system header is libc's: no Python, no blocking, pure only if `EXTERNAL` says so, and then it still reads (and, unless the pointer is to `const`, writes) what a pointer argument points to. A decref (vocabulary, so its argument keeps its type) breaks all but no_raise, unless the argument is a singleton or a local declared with a `NO_DEL_TYPES` struct (typedefs followed).

The walk stops at `SEEDS` (decref, gc, blocks, sets_error, alloc, fatal) and holds the flags `ASSUMPTIONS` names: `PyErr_SetString`, `PyDict_GetItem` and `_PyDict_GetItem_KnownHash` (F34), `PyErr_Restore`, `function_call` (D53), `PyNumber_Index`. A file that isn't mirrored and doesn't preprocess for the target is left out, so its functions are the worst case.

Onto Ni's flags (`Effects.cf_flags`): `CfPure` needs pure, readonly, no_python and no_raise (Ni calls it at compile time when every argument is known); otherwise `CfNoPython` from no_python; `CfNoBlock` from no_block. `cannot_raise` is no_python and no_raise. The analysis is path-insensitive and knows a local's type only from its declaration. Snippet tests: `tests/effect_snippets.py`.

## Mechanisms (D75)

Where Ni's own mechanism must differ between versions (D67 (4)), `tools/metac/mechanisms.py` asks the target's source and picks a snippet; `generate.py` writes it into the build directory.
- **Detectors** each answer one question from the source with the definition read; header questions search every `Include/**/*.h`, fields by the last name in each declaration (both `#if` branches count).
- **`VARIANTS`** maps each mechanism's tuple of answers to a variant, the file `ni/mechanisms/<mechanism>/<variant>.h`. Answers not in the table stop generation and remove the old headers.
- **Output:** one `mechanism_<name>_gen.h` per mechanism: answers and evidence in a comment, an include guard, `#line` back to the snippet, the snippet. A variant not written yet is a stub, `#error "... is not written yet (file; D75)"`.
- **Pairs** (D77): code both in `Python/pyruntime.c` (`Py_BUILD_CORE`, the only file that sees `_PyRuntime`) and in emitters is two entries with the same detectors, `<name>_runtime` and `<name>`.
- `python3 tools/metac/mechanisms.py SRC...` prints the answers; `tests/mechanisms.py` checks them on fetched tarballs.

**Variants chosen** ("same" is the variant to its left; every variant 3.7 doesn't use is still a stub; the detector answers are in `tests/mechanisms.py`):

| Mechanism | 3.7.17 | 3.8.20 | 3.11.11 | 3.12.9 | 3.13.2 |
|---|---|---|---|---|---|
| entry | `frame_eval_hook` | `frame_eval_hook` | `frame_eval_hook_interp` | `vectorcall` | `vectorcall` |
| tracing | `tstate_use_tracing` | same | `cframe_use_tracing` | `monitoring` | `monitoring` |
| frames | `frame_object` | same | `interpreter_frame_cframe` | same | `interpreter_frame` |
| eval_breaker_runtime | `ceval_loop_top` | `ceval_loop_top_signals` | `static_handler_copy` | `internal_handler_copy` | `handler_call` |
| eval_breaker | `int32_address` | same | same | same | `tstate_word` |
| tstate_runtime, tstate | `runtime_address` | same | same | `thread_local` | `thread_local` |
| recursion | `depth_up` | same | `remaining_down` | `py_remaining_down` | same |
| exceptions | `curexc_triple` | same | `curexc_triple_info_value` | `current_exception` | same |
| store_gc | `gc_refs` | `gc_next` | `gc_next` | `gc_next` | `gc_next` |

So 3.8 needs two snippets written. On 3.11 the hook stays usable (the specialiser refuses Python calls while one is set); on 3.12 vectorcall is the entry (PORT312.md). [read]

**What the 3.7 snippets hold** (each file's comment says more): the entry hook and its installation (the rest of entry is the core's `eval_hook.c:ni_entry_point` and `NiCode_Run`); the tracing test, at run time and emitted; `ni_frame_t` and the frame-chain accessors; the eval-breaker address, handler (a copy of ceval's loop top) and emitted test; the running thread state's address and load; recursion enter and leave; the raised and handled exception (gets borrowed, puts take the references; a triple because 3.7's bytecode pushes six items) and `NI_EXC_OCCURRED_OFFSET`; the inline store's GC test. Loading the thread state, `psyco_check_eval_breaker` and frame *objects*' fields (`f_lasti`, `PyFrame_New`) stay core. The handler could be generated from the loop top by the reader (D77 (6)); not done.

## Stage 0: layouts (D76)

The CPython fields Ni relies on are data, `policy_table.LAYOUTS` and `RUNTIME`; `tools/metac/gen_layouts.py` checks them against the target's headers and writes `layouts_gen.h` (included by `psyco.h`) and `runtime_layouts_gen.h` (`pyruntime.c` only). `gen_layouts.py SRC...` reports per tree what is missing or changed.
- **Listed:** the public fields Ni reads (object header, sizes, type flags and version tag, float, long, list, tuple, bytes, dict, `PyASCIIObject` with its `state` bits, slice, function, method, C function, cell) and private structs copied under Ni's name (`seqiterobject`, `rangeobject`, `rangeiterobject`, `ni_dictkeys_t`, `ni_dictentry_t`).
- **Checked:** the struct found by tag or typedef in `Include/**/*.h` (then the entry's `from` files), the headers' macros expanded in it, anonymous members taken as the parent's; a field missing or declared differently stops generation, naming struct, field and header. The header gives `NI_OFS_`, `NI_SIZE2_`, `NI_UNSIGNED_` and four `_Static_assert`s per field (type, size Ni reads, kind, sign), and `NI_BIT_` masks low bit first.
- **Used by** `vcompiler.h:CPY_FIELD`, `CPY_NONNEG_FIELD`, `CPY_ARRAY`, `CPY_OFFSET`. A field not in the table doesn't compile.
- **Stays hand-written:** Ni's own structs and representation (`DEF_FIELD`s, the index chain, `FMUT`, `FPYREF`, `FARRAY`, virtual-only slots); arrays with no struct field (`LIST_itemsarray`, `INSTANCE_dict`, `T_*`, str data); mechanism snippets' fields, with the same assertions; fields Ni's C reads through the headers.
- **Other versions:** 3.8 changes one field Ni reads (the GC test, a mechanism), 3.11 six, 3.12 and 3.13 fourteen; several are mechanisms rather than layout entries.

## General translation (D74)

`tools/metac/gen_abstract.py`'s units write the object, number and builtin headers below (D19 stage 2). `pabstract_gen.h` has moved to the new translator, `tools/metac/gen2/abstract.py` (GENERATOR_DESIGN, Migration step 3; D104): the same rules on pycparser's AST, so what follows holds for it too, except that it reads no `cparse` and keeps its policy items in `gen2/policy.py`. [read]

**Reading C.** `tools/metac/cparse.py` reads statements and expressions over ctokens' tokens, each keeping its token span so it can be copied back as C. `cparse.cpp` runs `gcc -E -dM` (macros as the file is compiled) and `gcc -E -P` (typedefs, struct fields, prototypes: `cparse.Decls`) with the target's `pyconfig.h` and CFLAGS (`NDEBUG`).

**The walk.** Per function: a summary of its callees (meta kind, parameter binding times, which parameters' types it reads, whether it may return NotImplemented or always fails), then a walk with a state per path (binding time and ownership per variable, promoted types, slots read since the last call, whether a run-time call has happened). An `if` on a compile-time condition is a C `if` in the meta, one known at generation time folds, one on a run-time integer is a condition code. At a merge, differing ownership is an `own_x` shadow; slots and types are kept only where both paths have them. `&&`/`||` keep C's order. The walk repeats (at most four times) until the promotions it hoists to the entry are all it needs; one still needed after a call is a refusal.

**Run-time copies.** Resuming CPython's algorithm calls the exported function if nothing ran yet, else a `cimpl_` copy: a whole static function, or the statements from the resume point on, live variables as parameters; static callees are copied too. Only rare paths reach a copy.

**Slots** are passed as `offsetof(PyNumberMethods, nb_add)`, as `NB_SLOT` expands against the target's headers. A definition whose policy items the translator doesn't implement stays hand-written (D74 (2)); an `int` result's convention can't be read from the C and needs a Ni declaration.

## Loops (D78)

A known count is unrolled; otherwise the run-time function runs, after any case the meta knows (Psyco's practice). Text: `RULES['loops']`. `cparse.Parser(loops=True)` reads loops, `break`, `continue`, `++`/`--` and the comma operator; liveness runs to a fixed point. `gen_abstract.loop_rule`, shared with `gen_ceval.py`:
1. **Counted loop, known bound: unrolled.** A `for` with a conjunct `V < E` (or `<=`, `!=`, the downward forms), V stepped by one in the head and assigned nowhere else, E unassigned in the body, both known at compile time; the body is emitted per turn, run-time tests as condition codes. An integer compile-time on entry and run-time after a turn is made run-time from its declaration (at most six rounds).
2. **Known length:** a bound reading a `known_length` size field (immutable) asks `PsycoTuple_Load`: a virtual tuple's length or a constant one's up to `CT_TUPLE_LOAD_SIZE_LIMIT`, items through `PsycoTuple_GET_ITEM`; at -1 CPython's function resumes there.
3. **Any other loop is CPython's:** the meta resumes at it; where every path from a point with nothing called loops or fails, the whole function is called there (`every_path_loops`).
4. **Promotions inside a loop** needed after its first call are made at its entry if nothing was called before, else refused (PATTERNS rule 1).
5. **Errors:** a call's error in a loop is tested at once when the error value ends the loop and is returned (`errors_leave_loop`); resuming where an error test held is refused.

**Object units.** `gen_abstract.py --unit X` translates the slots `TRANSLATED_SLOTS` names for a file, found from the type's initialiser and tables (`slot_functions`), into `p<file>_gen.h`, registered with `Psyco_DefineMeta`; a static slot function resumes through the type's slot. A type's initialiser is read from the file that defines it (`type_tokens`), so `object.c:PyObject_SelfIter` is the iterators' `tp_iter`. gen2's families name their slots in `gen2/scope.py` instead (`gen2/slots.py`, D110).

## Virtual objects (D80)

A virtual type's support is generated from CPython's constructor and its `TYPES` entry, and a call of that constructor in translated C is the virtual object. Representation policy (which types, weights, item limit, fields kept when forced) stays in the table.
- **Input:** `TYPES[...]['virtual']`: Ni's variable, weights, the CPython `constructor`, Ni's `new`, optional `max_items` and `forced: drop_fields`. `policy.py` checks them, and refuses a `('virtual', 'result')` item on an entry a generator writes.
- **`gen_virtual.py`** reads each constructor of a type with no mutable fields (field ← parameter, `if (P == NULL) P = Py_None;` a default; an array type's size and `_SET_ITEM` stores, followed through a call, `stores_through_call`; a function in `Include/` read with its caller's macros), with fields from `LAYOUTS`, and writes `<file>_virtual_gen.h`: `direct_compute_X`, `compute_X` (all constants: built now, `allocator_pure`; else the constructor at run time with effects.py's flags), Ni's `new`, `gen_virtual_X_init()`.
- **The constructor rule** (`gen_abstract.py`): a `TYPES` constructor call is Ni's `new`; an array's size must be known and within `max_items`, else CPython's function resumes there; never NULL. A `PyObject **` local is an offset into known items or the virtual being built; `P[i] = x` stores x's vinfo. A resume while a virtual is half built is refused.
- Lists are mutable and keep the hand virtual form. A folded slice is new per compile, so respawn's replay compares slices by type (`dispatcher.c:respawn_flatten`).

## Container slots (D84)

- **Type-field slots** (`tp_richcompare`, `tp_iternext`, `tp_descr_get`) are found by position in the macro-expanded initialiser (`type_field_slot`); how Ni calls such a meta is `SLOT_ARGS` (`tp_richcompare` `"vvl"`, an `l` known at compile time), checked against each caller.
- **Switches and statement macros:** with `switches=True` `cparse` reads `switch` and `do { } while (0)`; a header macro that is such a statement is expanded (`Source.statement_macros`). A compile-time switch is a C switch in the meta; a fall-through, or a resume inside after a call, is refused. `Py_RETURN_TRUE`/`FALSE`/`NONE` are Ni's constants.
- **Aliases:** `vt = (PyTupleObject *)v` is `v` (`Var.alias`). A run-time int tested for truth is `!= 0`.
- **Fields of run-time objects:** `p->f`, `Py_SIZE(p)` with a `TYPES` `descriptors` entry is `psyco_get_field` (`object_field`; gen2's `actions.field_read` for list, dict and bytes, D116); `FMUT` decides re-reading after calls.
- **Tuple slots** (still gen_abstract's): `returned_length`, `bounds_test` (`x < 0 || x >= n` as one unsigned compare), `field_array_read` (`TUPLE_ob_item` from the layout), `incref_returned`.

## Argument binding (D85)

`tools/metac/gen_call.py` generates the binding part of `ceval.c:_PyEval_EvalCodeWithName` into `psyfunc_gen.h`, run by the compiler; making, inlining and running the frame stay core.
- **Input:** the entry with `('other', 'frame_binding')` and Ni's declaration `psyfunc.h:_PsycoEval_EvalCodeWithName`, which gives binding times: `vinfo_t **` parameters are values, the rest the compiler's; a parameter it leaves out may not be read.
- **The slice:** from the first statement taking the frame's locals to the first using `f` otherwise.
- **Rules:** a local takes the binding time of what is assigned to it (fixed point; two C types refused); one only assigned what has no form is undeclared and reading it ends the binding. Expressions over C values are C in the binder. `SETLOCAL`'s `Py_XDECREF` is `vinfo_xdecref`, `Py_INCREF` `vinfo_incref`; `PyTuple_New(n)` is `PsycoTuple_New(n, NULL)` behind `n > max_items → return -1`. Anything without a compile-time form, an error, or a `goto` out is `return -1` (the real call).
- **Kept core** (D85 (5)): `fncall_init`'s refusals, `COMPUTE_DEFCOUNT`, keyword and kw-only calls, inlining, recursion, tracing, genexpr fusion, `PsycoCFunction_Call`'s dispatch.

## Representation rules (D83)

How Ni represents a value is data in `TYPES`, its rules written as C, parsed after the file with its macros (`gen2/types.py`) and matched structurally with `x` for any variable, casts ignored; `gen2/reps.py` applies them in the numbers family (`plongobject_gen.h`, `pfloatslots_gen.h`, D110). For ints (`TYPES['Include/longintrepr.h:_longobject']['word']`): `test` `Py_ABS(Py_SIZE(x)) <= 1` is `load` (`PsycoLong_MediumValue`: 1 a word, 0 not, -1 error) as a `switch` whose 0 arm resumes CPython; `value` `MEDIUM_VALUE(x)` is the word; `sign` `Py_SIZE(x)` against 0 is `PsycoLong_Sign` against 0, as a value `psyco_vinfo_condition`. Casts, `+ - *`, unary `-` and constants are `integer_*` calls, checked by interval arithmetic (the C must not overflow its type, the result must fit a word); a word is released after its last read in the statement; a `constructors` call given a word is `new` (`PsycoLong_FROM_WORD`), whose result needs no type assertion. A branch on the sign needs a `likely` item. [read] [run]

## Builtins (D86)

`tools/metac/gen_builtins.py` translates what `TRANSLATED_FUNCTIONS` names in `Python/bltinmodule.c` (clinic header spliced in) into `pbltinmodule_gen.h` (math is gen2's, D115); the `PyModuleDef`'s method table gives each C function and `ml_flags`, and flags with no `METH_ARGS` entry are skipped.
- A root's parameters follow `METH_ARGS[flags]['c']` (`METH_FASTCALL`'s array and count are the one tuple Ni passes, `vargs`). A root that resumes before any call is `METH_ARGS[flags]['fallback']`, so wrong counts, star arguments and keywords keep CPython's messages.
- `('other', 'unpack_arguments')` on a callee (3.7: `_PyArg_UnpackStack`) tests the count, the fallback outside, and translates the rest once per count, each output a borrowed `PsycoTuple_GET_ITEM(vargs, i)` that promotes like a parameter.
- A variable assigned and tested against `NULL` folds; a `bool` Ni function's result tested as `< 0`, `!= 0`, `== -1` is `!(call)`; `zero_or_one` marks a result 0 or 1 after its error test, `bool_word` makes it the bool's word. `nested_scalar_call`: a call that cannot fail, returning a scalar, as another's argument.

## Scalar code (D87)

Float slots, `PyFloat_AsDouble`, int's `nb_float` and math come from gen2 (`gen2/scalar.py`, `gen2/inline.py`; D113–D115, GENERATOR_DESIGN "The scalar helper"). gen_abstract's remaining units read doubles through `tools/metac/scalars.py` (`ScalarRules`), whose rules are below; none of them now makes a helper.
- **Representation** (`TYPES['Include/floatobject.h:PyFloatObject']['double']`): reading `ob_fval` (directly, through a cast or `PyFloat_AS_DOUBLE`) is `PsycoFloat_AS_DOUBLE`; `PyFloat_FromDouble` of a double is `PsycoFloat_FROM_DOUBLE`, a virtual float.
- **Copy-through** (`copy_through`, D19): the backend has no floating point, so the longest run of statements on C scalars only (the representation's locals, integers, constants, compiler built-ins, `EXTERNAL`'s pure functions, `SCALAR_CODE`'s raising functions and variables such as `errno`, static scalar functions copied as `cimpl_*`, another file's functions `proven_pure`) becomes a C helper the meta calls. A run must read a double; its inputs are the helper's arguments, its one live result the return. A tail (every path an error or a scalar's constructor) returns the scalar, `return NULL` as `return -1`. Flags `CfPure`, and `CfPyErrCheck` where it can raise.
- **`inline_static`:** a static function called with a local's address or a function's name is its body at the call (`convert_to_double(&v, &a)`, `math_1(args, acos, 0)`), for `return f(...)` and `if (f(...) OP K)` (or `if (f(...))`).
- A double result whose meta returns `vinfo_t *` has `-1.0` as its error; a non-parameter's unknown type resumes CPython from the statement; an exported callee with a hand meta (`PyLong_AsDouble`) is called.

## str and descriptors (D89)

str's slots are gen2's (D116): `bit_field` is `actions.bit_test`, `runtime_choice` `Walk.choice`, `const_fold` the `FoldMayFail` item, and `PyUnicode_Concat` a `Whole` item. The descriptor rules are in `cparse.py`, `tools/metac/conditions.py` and `tools/metac/pointers.py`.
- **`preprocess_conditionals`:** the file's directives, with marker lines, through `gcc -E -dD -P` with the target's configuration: inactive lines blanked, each obeyed `#undef` back with the definition in force before it, so a macro is the one in force where used. A function that calls itself is refused. `(assert(X), E)` is E.
- **`bit_field`:** `((PyASCIIObject *)s)->state.ready`, with the bits in `LAYOUTS` and a descriptor in `TYPES`, is one unsigned compare against `NI_BIT_PyASCIIObject__state__ready`; the `likely` item names the macro as written.
- **`runtime_choice`:** `PyUnicode_READY(s)` is `(IS_READY ? 0 : _PyUnicode_Ready(s))`, the `likely` arm leaving the legacy path to CPython.
- **`raw_pointer`:** an `l` parameter's fields are C reads in the compiler; `addr += E` adds to an offset; `*(T *)addr` is `psyco_get_field_offset` with T's range. A `PyObject *` read so holds no reference (`Py_INCREF` is `need_reference`; returning it without one is refused), and its NULL test is a run-time test whose NULL path runs to the function's end.
- **`switch_break`:** a `break`, or the last case running off, is the statements after the switch on that path.

## Failures log

What the generator can't do: the CPython construct and what was done. `tools/metac/hand_table.py` refers to rows as `failures:NAME` and `donecheck.py` checks NAME appears below. Fixed rows are in git (`git show e404100:doc/METACOMPILER.md`).

| CPython function | Why the generator skips it | What we did |
|---|---|---|
| `longobject.c:long_richcompare` | `long_compare` is a digit loop; reducing it to one digit is judgement. 3.12's has a compact fast path [inferred]. | Hand-written. |
| `longobject.c:long_div`, `long_mod`, `long_divmod`, shifts, `& ^ \|` | No one-digit fast path in 3.7's C (`fast_floor_div`/`fast_mod` read `ob_digit[0]` and `Py_SIZE` apart); no signed divide in the backend. | Hand-written, `extra` items (word helpers). |
| `longobject.c:PyLong_AsDouble` (word rule) | A unit has no list of the declared functions to translate; the word test is on a `PyObject *` after `PyLong_Check(v)`, not a struct variable. | Hand-written (`PsycoLong_AsDouble`, `cimpl_fp_from_long`), `limit`. |
| `longobject.c:_PyLong_Frexp` | A double and an exponent through `int *`. | Not translated; no generated caller. |
| `float_divmod`, `float_floor_div`, `float_pow` | Return a tuple (a scalar run returns one value); `float_pow`'s `int` locals and early object returns. | Residual. |
| `float___trunc___impl` (nb_int) | Returns an int; the backend can't convert a double to a word. | Residual. |
| `floatobject.c:float_richcompare` | Switches on `w`'s type, exact int/float compare. | Hand-written (P26). |
| `boolobject.c:bool_and`, `bool_or`, `bool_xor` | A call through a static type's slot; an object compared with `Py_True`. | CPython's slot. |
| `mathmodule.c` `FUNC1` on `m_*`, `FUNC1A`, `FUNC2`, `pow`/`fmod`/`hypot` | `m_*` under `#ifdef HAVE_*`; conditionals, arrays and loops in `m_erf`…; `METH_VARARGS` with `PyArg_UnpackTuple`; `_PyArg_ParseStack`'s format. | The C function is called. |
| `ceval.c:BINARY_ADD`, `INPLACE_ADD`, `BINARY_MODULO` | Exact-type fast paths: the reader's judgement rule, not a failure. | `type_test_generic_path`. |
| `ceval.c:IMPORT_NAME` | `import_name` takes the frame (core) and has a local array. | Hand-written, core. |
| `ceval.c:UNPACK_SEQUENCE`, `UNPACK_EX` | `unpack_iterable` writes the outputs through `stack_pointer + oparg - 1`; the hand case unpacks a virtual tuple with no code (P22). | Hand-written. |
| `ceval.c:BUILD_CONST_KEY_MAP` | Where Ni doesn't know `keys`, the loop reads items of a tuple Ni has not loaded (only `PsycoTuple_Load` gives items). | Hand-written, `limit`. |
| `ceval.c:CALL_FUNCTION_EX` | `PyDict_New` failing leaks `kwargs`: CPython 3.7.17's own bug. | Refused; calls are core. |
| `ceval.c:BEFORE_ASYNC_WITH` | An error exit with a pushed value. | Refused (core). |
| `abstract.c:PyObject_LengthHint`, `PySequence_Check`, `_PyObject_HasLen`; `dictobject.c:PyDict_Contains` | An `int` result's convention is in no test the C makes. | Not generated; needs a Ni declaration. |
| `abstract.c:PySequence_Fast` | After a failed call it calls `PyErr_ExceptionMatches`, a run-time call with the error pending. | Not generated; no caller. |
| `abstract.c:PyIter_Next` | `PsycoIter_Next` is FOR_ITER's call and leaves StopIteration set. | `NotTranslation` in `gen2/policy.py`. |
| `listobject.c:PyList_New` support (`compute_vlist`, `PsycoList_New`) | A list is mutable: its virtual is a vlist of at most `VLIST_LENGTH_MAX` items, not the struct's fields. | Hand-written, core. |
| `listobject.c:list_contains`, `list_richcompare`; `bytesobject.c:bytes_contains`, `bytes_item`, `bytes_richcompare` | A mutable `ob_size`; a comma operator in `PyBytes_AS_STRING`, `likely` items, `memcmp`. | Slot calls; `pbytes_subscript` kept (D56). |
| `rangeobject.c:range_length`, `compute_range_item`, `get_len_of_range`, `range_iter`, `rangeiter_next` | Ni's range is a virtual of words; an object constant where a vinfo is wanted; `get_len_of_range` returns `unsigned long`. | Hand `prangeobject.c`. |
| `tupleobject.c:tuplecontains` (cost) | `cmp == 0 && i < n` in C's order: one test of `cmp` too many. | Accepted. |
| `unicodeobject.c:PyUnicode_RichCompare` | An identity compare of two objects; D37's shortcuts aren't in the C. | Hand-written. |
| `unicodeobject.c:unicode_subscript`, `unicode_getitem`; `bltinmodule.c:builtin_ord` | PEP 393's data and kind, D56's slices. | Hand-written; `ord` the C function. |
| `iterobject.c:iter_iternext` | F48's guard on `it_seq`. | Hand-written. |
| `descrobject.c:method_get` | Builds a `PyCFunction`, whose virtual is hand-written with no `TYPES` constructor. | Hand-written, core. |
| `object.c:PyObject_Hash`, `PyObject_RichCompareBool`, `do_richcompare` | No `likely` item for `PyType_Ready(tp) < 0`, and `tp_hash` wants F50's heap-type guard; `v == w` on two objects (F109); "resuming needs res, which has no value here". | Hand-written, `limit`. |
| `call.c:_PyMethodDef_RawFastCallDict`, `PyCFunction_Call` (pmethodobject.c's dispatch) | How Ni emits the call, not translation. | Hand-written, core (D85). |
| `bltinmodule.c:builtin_iter`, `builtin_next`, `builtin_chr_impl` | A virtual seq-iterator is the wrong type for `iter()`; `tp_iternext`'s NULL without an error has no form; `PyArg_Parse`'s format. | The C functions. |
| `bltinmodule.c:min_max`, `builtin_sum_impl`, `builtin_any`, `builtin_all`, `builtin_round_impl`, `builtin_globals_impl`, `typeobject.c:type_call` | Iterator loops, genexpr fusion (D46), known-tuple shortcuts, P26, the frame's globals (F47), virtual `range` and `super()`. | Hand-written (D86 (6)). |
| 3.8: `_PyEval_EvalCodeWithName`'s `_PyTuple_FromArray(args + n, argcount - n)` | Another file's function with no form. | `*args` is the real call. |
| 3.8: the rest | See PORT38.md "3.8 refusals by cause". | Not built for 3.8. |
