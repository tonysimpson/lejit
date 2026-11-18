# Generator design

The design for the meta-compiler that generates Ni's compile-time CPython layer. It replaces today's `tools/metac/` (about 20,000 lines, grown one named rule at a time) with one pipeline. Status: **proposed**, revised after a review of every generator file against it (2026-11-18); the open bugs that review found in today's code are KNOWN_BUGS G5–G18.

## What it does

For each CPython C function Ni mirrors, it writes the function's **generating extension**: C that runs inside Ni's compiler and emits machine code for that function, given what is known at compile time. This is Psyco's meta function, derived from the C instead of written by hand. The theory is partial evaluation (Jones, Gomard and Sestoft, *Partial Evaluation and Automatic Program Generation*, 1993).

Constraints (GOAL, DECISIONS):
- One Ni source for every CPython from 3.7 on (D67); the target version is only an input.
- Generated at build time, not checked in (D68).
- The input is CPython's C source. LLVM only as an assembler for templates (D65).
- Breaks loudly: a construct with no rule is refused with its C location and reason, never guessed.
- **Resume keeps it correct:** where the generator can't specialise, the run-time code is CPython's own. Specialising more is a speed question, never a correctness one.

## Pipeline

```
source ─▶ gcc -E (+ prelude, vocabulary shim) ─▶ pycparser ─▶ typed AST
                                                                 │
      rules (Python) ──▶ effects ─▶ binding times ─▶ promotion placement
                                                                 │
                                   specialiser ─▶ C builder ─▶ *_gen.h + report.txt
```

One front end serves everything: the translator, effects, layouts, mechanisms, the policy checks and the done check. Today there are four separate C readers.

## Front end

**Preprocessing.** `gcc -E` with the target's include paths and `pyconfig.h`, and CPython's own build flags from its Makefile: `-DNDEBUG`, `Py_BUILD_CORE`, and `Py_BUILD_CORE_BUILTIN` for modules. One deliberate departure: `-DUSE_COMPUTED_GOTOS=0`, because computed gotos (`&&label`, `goto *`) don't parse, and Ni doesn't need them.

**Two shims** (`tools/metac/shim/`, used by `front.py`):
1. **A GNU prelude** (`-include`) that neutralises what pycparser can't parse: `__attribute__`, `__asm__`, `__extension__`, `__restrict`, `_Float128`, `__builtin_va_arg`, `__builtin_offsetof`, `__auto_type`, `__typeof__`, `__builtin_types_compatible_p`. Because this drops `packed` and `aligned`, **the AST is never trusted for sizes, offsets or bit positions**: those come from the real compiler (Layouts).
2. **A vocabulary shim** spliced into the text just before each function being translated, after every header and every `.c`-file definition (ceval.c defines `TARGET`, `POP` and the rest after its includes, and 3.8's internal headers redefine the atomics late). It `#undef`s each vocabulary macro and redefines it as a call to a marker, so the name survives. `gcc -dD` checks that nothing redefines a vocabulary name after the shim; if something does, generation stops (owner: fix the shim when this breaks).

**Vocabulary entries have kinds**, because not every macro can become a function call:
- value (`Py_TYPE(x)`);
- constant (`Py_None`);
- statement (`Py_RETURN_NONE`);
- lvalue accessor (`Py_TYPE(o) = t`, 56 uses in 3.7);
- type argument (`PyObject_NEW(T, tp)`, 77 uses);
- writes its argument (`Py_CLEAR`, `Py_SETREF`);
- bracket pair (`Py_TRASHCAN_SAFE_BEGIN` … `END`).

The atomics, `PyThreadState_GET`, and ceval's `TARGET`, `DISPATCH`, `PREDICT` and stack macros are vocabulary.

**Parsing.** pycparser **≥ 3.00, pinned** (2.23 rejects `_Py_IDENTIFIER`'s `.string =` designator). Line markers are kept, so messages name the file and line. A message inside a macro names only its use site. The AST is typed from its declarations: every expression's C type, struct fields and typedefs. Header `static inline` functions are parsed and can be inlined.

**Rule strings** (C in the rules files) are preprocessed the same way, in a scratch translation unit that includes the same headers and shims, and matched against the AST structurally, ignoring casts.

**First gate:** a test that every mirrored CPython file parses, on 3.7 and 3.8, each with its own `pyconfig.h`.

Ni's own tree still needs a small reader, to check that every Ni name a rule gives exists and for the done check. It's a tokeniser over Ni's headers, not a C parser.

## One translator, four entry points

A slot, a builtin, an `abstract.c` function, an opcode case and argument binding are all a C body with parameters. Only the entry differs:

- **A function:** its parameters, with binding times from the conventions file (`SLOT_ARGS`, `METH_ARGS` and Ni's declarations).
- **A builtin:** found from the module's method table, with Clinic wrappers expanded. Its `ml_flags` choose the convention. Arguments arrive as a partially static array (length known, contents run-time); the body is translated once per argument count in a small range (polyvariance, capped), and any other count is a restart.
- **An opcode case:** a **stack-effect signature**, not an array. It has named inputs and outputs, sizes linear in `oparg`, and conditional items. On 3.12+ the signature comes from `bytecodes.c`. Before that, a reader derives it from ceval's stack macros, with positions symbolic in `oparg` and interval reasoning over `[0, 2^32)` rather than sampling. It is checked against `dis.stack_effect`, inputs and outputs separately, with `jump=` on 3.8+. Stated once for every case:
  - **Epilogue:** inputs stay on Ni's value stack until the last failure point, then pop, then push.
  - **Error protocol:** CPython's "release, then `goto error`" becomes Ni's "leave on the stack, `break`".
  - **Jumps** are a specialiser action (below). The same read emits each opcode's successors, jump units and conditional pops for `mergepoints.c`, which hand-codes them today.
  - **Frame table:** each ceval local and frame field maps to Ni with a binding time: `fastlocals`, `freevars`, `consts`, `names`, `f_globals` (known, so `LOAD_GLOBAL` can specialise), `f_builtins`, `f_locals` (run-time) and `tstate`. `PyFrame_BlockSetup` and `BlockPop` map to Ni's compile-time block stack, so 3.7 and 3.8's `SETUP_*` and `POP_BLOCK` can be generated.
  - **Core opcodes** are a declared list with a reason each (unwinding, return and yield, `EXTENDED_ARG`, the call protocol, tracing), cross-checked against the generator's refusals. Token tests stop deciding what's core.
- **Argument binding:** the part of `_PyEval_EvalCodeWithName` between making the frame and running it. It runs **entirely at compile time** over partially static arrays (length and which slots are NULL are known, contents aren't) and emits no code; anything else is a restart. The keyword path is refused until Ni passes keywords, rather than generated and never run.

## Abstract values

Each C value the translator tracks has:

**A binding time.**

| Time | Meaning |
|---|---|
| `gen` | known to the generator: folded away in the generated C |
| `meta` | a C value inside the meta, known when Ni compiles |
| `dynamic` | a vinfo |
| `pointer` | a run-time base plus a `meta` offset (an object's items, a `char *` into a struct) |
| `array` | length and NULL-ness `meta`, contents `dynamic` |

Whether a `dynamic` vinfo is compile-time, virtual or run-time is decided **online**, by the meta, when Ni compiles (`is_compiletime`, `Psyco_KnownType`). The generator emits that test as a **guard** (below). "Virtual" is a property of a vinfo, not a binding time.

**Ownership,** as a reference-count delta: new, borrowed, borrowed from memory (needs an incref to keep), stolen, or writes argument N. It comes from the vocabulary's signatures, and is checked at merges and exits. This replaces statement-order rules (`pending`, `incref_return`, `singleton`).

**Error state:** none, compile-time exception pending, run-time error, or the promotion pseudo-exception. On a failed path, no `PyErr_*` call that inspects or clears the error may run, and no residual call may be made.

**Return kinds,** a written contract with Ni's headers:

| Kind | Returns | Error |
|---|---|---|
| obj | a vinfo | NULL |
| val | a vinfo | NULL |
| ok | a bool | false |
| ct | an int | -1; refused after any run-time call |

The NotImplemented protocol (`CfPyErrNotImplemented`, `IS_IMPLEMENTED`, no `restype` assertion on NotImplemented) is part of this contract. Error conventions (`null`, `neg`, `minus1`, `notimpl`, `zero_ok`, `never`) are inferred by data flow from the callee and the C's own tests, not by reading the next statement's text.

## Analyses

In order, on the typed AST:

1. **Effects.** These are per target, on the preprocessed AST. Four separate flags:
   - `pure`: the result depends only on the arguments' values; reading mutable memory through a known pointer is not pure.
   - `readonly`: writes nothing.
   - `no_python`: never runs Python code.
   - `no_block`: never releases the GIL.

   Rules:
   - An indirect call, a function pointer of any kind, or a callee outside the scanned set gives the worst case.
   - A function name used without a call is still an edge.
   - Everything taken on trust is an entry in the effects rules with a reason: GC running finalisers, `%R` in `PyErr_Format`, and a subclass's `__del__` under a known C type.
2. **Liveness,** backwards, a fixed point over loops and gotos.
3. **Binding times,** to a fixed point. An operation is `meta` when its inputs are and it is `pure`. A test on a `meta` value picks its branch. A loop with a `meta` bound is unrolled within its budget; otherwise it runs at run time (D78).
4. **Promotion placement.** Only parameters can be promoted (F24); any other value gets a guard or a resume. No promotion may come after a run-time call (rule 1). Promotions are hoisted to function or loop entry. A callee's `needs` pass to its callers. A vocabulary entry says which arguments it promotes, and a hand-written Ni meta is assumed to promote its object arguments.
5. **Restart order.** Promotion and respawn re-run the case or meta, so nothing irreversible (an incref, a store, a `SETLOCAL`) may come before the last call or promotion. Respawn replays the meta, so its compile-time effects must be safe to replay (`const_fold`'s CPython call, `Psyco_HeapSlot`, recorded mros). This is one check over the specialised output, for opcodes and metas alike.

## The specialiser's actions

Each construct gets exactly one action, and each action lives in one place:

| Action | When | Emits |
|---|---|---|
| fold | inputs `gen`/`meta`, operation `pure` | a C value |
| fold-may-fail | a policy item allows calling CPython at compile time (`const_fold`) | the call in the meta; its error raised at run time |
| map | a vocabulary call or a type's `make` | the Ni function the rule names |
| residual call | a run-time call | `psyco_generic_call` with its effect flags |
| scalar helper | a run of statements on C scalars (below) | one C helper, called as a meta |
| inline | a static of the same file or a header `static inline`, by value (parameters become fresh locals) | its body, specialised in place |
| meta call | a static too large or recursive to inline | a call to its own meta, using its summary |
| guard | a fact the meta decides online (type known, slot unchanged, value compile-time, length loaded) | a test in the meta; resume or error on failure |
| promote | a parameter's type, where placement put it | `Psyco_NeedType` |
| build virtual | a constructor in `types` | a virtual; its items stored as vinfos |
| jump | an opcode's jump | a jump to the target's merge point; a conditional jump on condition codes with its pops |
| compile-time raise | an error known at compile time (P24) | Ni's compile-time exception |
| restart | nothing observable has happened yet | a call of the whole CPython function or slot |
| resume | anything else, at a resumable point | a run-time copy of CPython's code from here |
| refuse | a construct with no rule | nothing; reported with location and reason |

**The scalar helper**, spelled out:
- **The run:** the longest run of statements on C scalars only, starting at the current statement, whose result is at most one live variable, never split inside a loop; or the rest of the block, if every path returns.
- **Flags:** always `no_block` and `no_python`. `pure` only when it raises nothing and reads no C library state.
- **Library state:** errno and the floating-point environment never cross a helper boundary.
- **Function pointers:** a known function pointer in a run is checked against what it points to.
- **Errors:** a double's error is returned as an int status, with the value through an out-parameter, so the run-time check is a register compare, not `PyErr_Occurred()`.

**Resume** is one checked function, the only way to resume. It is refused:
- inside a meta loop (resume at the outermost loop instead);
- while a virtual is owned or partly built;
- with a value in a mixed-ownership state, or a `pointer` or `array` live;
- inside a switch after a call;
- mid-statement after that statement's run-time call;
- in a `ct`-kind function;
- on a failed path.

Its interface: parameters from liveness; owned values increfed in the copy; the result in the caller's convention. Statics it needs are copied, and recursive ones are refused.

**Budgets** are policy, reported when hit: unroll bound, virtual item limit (15), tail duplication for gotos and switches, polyvariance range (5), and re-walks.

## Rules (Python, with C inside)

Plain Python files; every C string is preprocessed and parsed as above and checked on load. **No file copies Ni's code.** Where Ni needs a list (`IS_LIGHT_INSTR`, `cannot_block_init`, the no-`__del__` type tests, the safe descriptors), the generator writes it from the rule. Where a rule needs a Ni constant, it names the macro, never its value.

| File | Holds |
|---|---|
| `vocabulary.py` | each idiom's kind, the Ni function, ownership signature and which arguments it promotes |
| `types.py` | each type's representation as Ni's own functions, its fields and descriptors, its `make`, and properties (no `__del__`, safe descriptor, immutable) |
| `conventions.py` | Ni's call sites: slot and `METH_*` formats with their fallbacks, return kinds |
| `scope.py` | which functions, slots, builtins and opcodes to attempt, and the declared core list with reasons |
| `effects.py` | seeds, external functions and assumptions, each with a reason |
| `policy.py` | the judgement calls: `specialise`, `likely`, `assume`, `no_promote`, `fold_may_fail`, budgets |

A **policy item** is a typed object whose arguments are C expressions matched against the typed AST. It has a **precondition checked against the source**: `likely(cond)` must match exactly one run-time branch, `specialise('w', T)` needs `w` to be a parameter whose type is read, `restype` must agree with every return path. A failure names the item.

**Keys** are semantic identities where one exists (`PyFloat_Type.tp_richcompare`, an opcode name, a method name), and an entry may give several C names. **No token-hash shapes.** On 3.7→3.8, 129 shapes changed, 45 of them by five tokens or fewer (renames), and only 43 sat on real judgements; meanwhile a change in a callee or struct changed no hash at all. Checked preconditions catch what matters and ignore renames. The only claims that can't be checked (assumptions in `effects.py`) are keyed on the effect analysis's summary of the function and its callees.

## Report, baseline and done check

- **The report** is a text file in the build directory, one sorted line per function: a key that names no file (so versions compare), status (generated, resumed in part with where, restarted at once, refused), reason and CPython location; a header line gives the source's SHA and the generator's commit.
- **A checked-in baseline** per CPython version, the same file; its keys, statuses and reasons are compared. Any change fails the build until accepted (copied over), so a function sliding from generated to refused can't pass silently.
- **The done check** reads the report and refuses a stale one. Reasons are **final** (core, mechanism, judgement, not a mirror) or **transitional** (refused, budget, limit, helper, a policy item the translator doesn't implement). It passes only with no transitional reasons, and reports them separately until then. A policy item is never more than transitional, so adding one can't pass the done test.

Today's generators already do this (`generate.py`, `donecheck.py`, `tools/metac/expected/`; INVENTORY.md). [run]

## Determinism

- `PYTHONHASHSEED=0` is fixed by `setup.py`.
- Every set iteration is sorted.
- A test generates under two seeds and compares: `check.sh` runs the generators with seeds 1 and 2 and compares both with the build's (seed 0), at no extra cost.
- Headers are written atomically, with a `try/finally` that removes them on any failure.
- Stale outputs are swept.

## Other generators

These share the front end, the rules loader and the C builder:
- **Layouts:** `static_assert`s on offsets and sizes from the real compiler, plus a compiled probe for bit-field order. The field list is derived from what Ni and the generator use (`CPY_FIELD`, `CPY_OFFSET`, descriptors), not kept by hand.
- **Mechanisms:** detectors run on the preprocessed, typed AST, so configuration counts and a free-threaded build isn't mistaken for the default. Each snippet declares the names, fields and types it relies on, and those are checked like layouts. Snippets hold no translated CPython code. Paired mechanisms (`eval_breaker` and its run-time half, `tstate` and its run-time half) are each one mechanism with two outputs.
- **Virtual types:** templates go through the C builder. The size index, the defaults and the conditions on stores are derived from the constructor's AST.

## Testing

- **Snippet tests per pass:** small C in, expected output out, for the shims, typing, each analysis and each action. Seconds to run.
- **The bugs found in review (G1–G18)** get snippet tests first, so the old generator isn't trusted as the oracle where it's wrong.
- **End to end,** as now: generation deterministic, `tests/run.py`, `cpython_tests.py`, `samecode.py`, and release-build instructions per warm iteration.

## Migration

1. **Gate:** the front end parses every mirrored file on 3.7 and 3.8. **Done:** `tools/metac/front.py` and `tests/parse_gate.py` (a `check.sh` step) parse 28 files on 3.7.17 and on 3.8.20, about 12 s per version (`e055c02`, D94, D95).
2. **Front end, effects and report** first, in use by today's generators. This fixes G1–G4 and G16–G18 on its own. **Done.** Effects: `tools/metac/effects.py` on the front end, with `tests/effect_snippets.py` (a `check.sh` step); the vocabulary shim carries the decrefs. Report: `report.txt`, the 3.7.17 and 3.8.20 baselines, `status_diff.py`, the done check from the report, the fixed hash seed and the loud unbalanced-file error.
3. **Move one family at a time:** abstract, numbers, containers, builtins, argument binding, opcodes. Each must give the same headers, or the same machine code by `samecode.py`, with every difference given a reason. Delete the family's old code as it moves. **Abstract: moved.** `tools/metac/gen2/` writes `pabstract_gen.h` (`df8385b`, D104–D107), with `tests/gen2_snippets.py` (29, a `check.sh` step). Against the old output on 3.7.17 and 3.8.20: the same 52 metas, 22 copies and refusals; the text differs only where pycparser joins adjacent string literals and `PY_ITERSEARCH_CONTAINS` is written as 3 (and, on 3.8, a copy writes `PyIndex_Check` as the source does). `samecode.py b6d3145 5bcffd4`: the same machine code in all 97 units. The report names statements as written, and `PySequence_Fast` is refused for a call with its error pending (G8). `gen_abstract.py` keeps its other units. [run]
4. **When all have moved, delete** `gen_abstract.py`, `gen_ceval.py`, `ceval_reader.py`, `ceval_inline.py`, `cparse.py`, `conditions.py`, `pointers.py`, `scalars.py`, `csource.py`, the old `policy_table.py` and the dead code in `cslots.py` (delete that now).
5. **Then extend.** `float_richcompare` and the other `type_whitelist` functions should each come from one `specialise` policy line.

## Decided (owner, 2026-11-18)

1. pycparser plus `gcc -E`, not libclang.
2. The annotated AST is not a "compiler IR" in GOAL's sense; that rule is about Ni's JIT compiler.
3. The shim is fixed when it breaks. The `gcc -dD` check makes the break loud.
