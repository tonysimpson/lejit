---
written: 2026-11-16
status: stage 1 started (`64f88e7` ints, `749985f` floats); bytecode stage started (gen_ceval.py, D26)
---
# Meta-compiler: approach and tooling options

The aim is in `GOAL.md` (long-term aim) and `DECISIONS.md` D16–D20. The patterns it must produce are in `PATTERNS.md`.

**Rule: use the simplest thing that works.** Move to a heavier tool only when the current one demonstrably fails on real input. When that happens, record the failure here, with the CPython construct that caused it, before you switch.

## Stages (D19)

| Stage | Output | Simplest approach |
|---|---|---|
| 0 | Field descriptors and layouts (`DEF_FIELD` entries) for the 3.12 object structs | Token-scan the struct declarations in the headers for field names and declared types. Let the C compiler do the arithmetic: generated code uses `offsetof()`/`sizeof()`, with a `static_assert` for each assumed size and signedness, so a layout change fails the build. |
| 1 | Compile-time versions of a small, regular slice (float slots, parts of `abstract.c`) | Token-level pattern rewriting in plain Python, with no dependencies. Diff the result against the hand-written `ni/Objects/p*.c`. |
| 2 | Whatever stage 1 can't do | Add analysis only for the failures seen. Expected first: run-time vs compile-time values, branches on run-time values, reference ownership. |
| Bytecode | Bytecode compiler from `Python/bytecodes.c` (3.12+) | Reuse CPython's own `Tools/cases_generator` parser (pure Python, upstream-maintained), then template the instruction bodies. |

Token rewriting means working on C tokens (skipping comments, strings and preprocessor lines correctly), never on raw text.

## Tooling options for when the simple approach stops working

| Option | What it is | Good for | Costs | Switch when |
|---|---|---|---|---|
| Plain token rewriting (current) | A tokeniser and pattern rules in Python | Mechanical patterns (`PATTERNS.md` M-class), struct scanning | No types, no nesting, no control flow | Rules need to know an expression's type or structure, or patterns spread across statements |
| pycparser + stub headers | Pure-Python C99 parser giving an AST and C output (Debian `python3-pycparser` 2.22) | Real ASTs. Easy to read and change. Clear parse errors fit "breaks loudly". | Needs preprocessed input. The fix is our own stub headers that declare CPython macros (`Py_INCREF`, `Py_TYPE`, `Py_RETURN_NONE`…) as functions, so they survive as recognisable calls. Comments are lost; GNU extensions need workarounds. | Token rules can't express a pattern, or we need declared types or nested expressions |
| Coccinelle | Linux kernel's semantic-patch tool (`spatch`; Debian `coccinelle` 1.3.0) | Pattern matching with metavariables and some control flow, macros left unexpanded, patches that read like diffs | OCaml, its own language (SmPL). Built for in-place rewrites, whereas we generate a differently shaped function. | A pattern needs matching across control flow or many syntactic variants, or we're bulk-fixing the existing hand-written code |
| `Tools/cases_generator` | CPython's parser for `bytecodes.c` | The bytecode front end only | Tied to upstream's DSL. Fine, because it moves with it. | Default for the bytecode stage |
| clang (libclang/LibTooling) | Full C front end with types and macro expansion tracking | Anything the others can't do | Large. We'd have to learn and teach a lot of clang (D20). | Last resort, and only for a proven need |

## First slice: ints (`64f88e7`, QUESTIONS 26)

`tools/metac/gen_long.py` reads 3.7's `Objects/longobject.c` and writes `ni/Objects/plongobject_gen.h` (checked in; `tools/check.sh` fails if it is out of date, D24). Plain token rewriting was enough. [run]

- **Tokens:** `tools/metac/ctokens.py`. It skips comments, keeps literals whole, takes out preprocessor lines and keeps `#define` bodies, so macros such as `CHECK_BINOP` are expanded only where a rule asks.
- **Slot table:** `long_as_number` is mapped by position against `PyNumberMethods` in `Include/object.h`. The comments in the table can't be trusted (`/*tp_bool*/`).
- **Shape matched:** `[decls] [CHECK_BINOP(a, b);] if (Py_ABS(Py_SIZE(a)) <= 1 && …) { [T v = EXPR;]* return PyLong_FromLong|FromLongLong(EXPR); } rest`.
  - The fast path becomes word arithmetic on virtual ints; `rest` becomes a residual call of the slot itself.
  - That is correct because the code before the guard has no side effects.
- **Range analysis:** a few lines of interval arithmetic over `+ - *`, unary `-`, casts and integer literals. It checks that the C itself can't overflow its type (signed overflow is undefined) and that the result fits a word. So "no overflow check needed" is derived, not assumed.
- **Result:** 5 of 22 slots generated (add, sub, mul, neg, invert).
  - Compared with the hand-written reference (`3702e3f`), it emits the same calls in the same order. `~` is new; it was a residual call.
  - All `tests/cases/ints.py` cases and `check.sh` pass.

Still hand-written: the representation of a virtual int. That is `plong_medium_value` (`MEDIUM_VALUE` and its guard on an int that has no `ob_size`), the residual fallbacks, and the `compute`/`direct_compute` functions. This is the runtime-model layer that QUESTIONS 23 asks about; the generator emits calls to it.

## Second slice: floats (`749985f`)

`tools/metac/gen_float.py` reads `Objects/floatobject.c` and writes `ni/Objects/pfloatobject_gen.h`. The shared parts live in `tools/metac/cslots.py`. [run]

- **Shape matched:**
  - `double` locals, then `CONVERT_TO_DOUBLE(param, x);` for each operand, or reads of `param->ob_fval`.
  - Then code on doubles only.
  - Then `return PyFloat_FromDouble(EXPR);`, or `return EXPR;` for `nb_bool`.
- **Copy-through:** the backend has no floating-point instructions, so the double-only code is copied, nearly verbatim, into a C helper that the meta calls.
  - The helper is `CfPure`, so it folds on constants. It is `CfPyErrCheck` if it can raise, and `return NULL` becomes `return -1`.
  - Every name in the copied code must be a double local, a whitelisted libm function (`fmod`, `copysign`, `fabs`, `floor`), `PyErr_SetString` or a `PyExc_*`. So code that touches objects can't slip through.
- **Macros:** the `PyFPE_*` macros are dropped only after checking that `pyconfig.h` doesn't define `WANT_SIGFPE_HANDLER`. The output also has an `#error` guard for that case.
- **Result:** 8 of 14 slots (+ - * / % unary - abs bool).
  - It replaces the hand-written + - * / unary - and abs, with the same helper calls.
  - `%` and `bool` are new; they were residual calls.
  - Same speed on nbody and mandel. Test: `tests/cases/floats.py`.
- **Representation, hand-written:** `psyco_convert_to_double` for `CONVERT_TO_DOUBLE`, `PsycoFloat_AS_DOUBLE` for `ob_fval`, and the compute functions.

Compared with ints, the floats needed no range analysis, because the C helper *is* the C code. That makes copy-through the cheapest pattern so far. It works wherever the backend lacks an operation and the code touches only C scalars.

## Bytecode stage: desk read of 3.12's `bytecodes.c` (2026-11-16)

Read from the 3.12.9 source tarball (`Python/bytecodes.c`, `Tools/cases_generator/`); nothing was built or run. The question: does 3.12's DSL make the eval loop (the hand-written `ni/Python/pycompiler.c`) easier to meta-compile than 3.7's `ceval.c`?

**What the DSL gives** [read]:
- Every one of the 186 instructions declares its stack effect, e.g. `inst(BINARY_OP, (unused/1, lhs, rhs -- res))`. There are no transitional forms without one. Inline-cache entries are declared the same way (`index/1`).
- Error exits are explicit: `ERROR_IF(cond, label)`, `DECREF_INPUTS()`.
- The spec's own constraint, "Until the last `DEOPT_IF`, no objects may be allocated, `INCREF`ed, or `DECREF`ed", is Psyco's restart rule (PATTERNS rule 1). The DSL is written to keep instructions restartable, which is what Psyco needs.
- The parser (`Tools/cases_generator/parser.py`, `lexer.py`) is pure Python with standard-library imports only. It matches stage "Bytecode" in the table above.

**3.7, for contrast** [read]: 119 `TARGET` cases in `ceval.c`. Stack effects are implicit in `POP()`/`TOP()`/`SET_TOP()` sequences, usually popping first (`BINARY_ADD`), which is the order Ni's restart rule forbids ("use TOP() not POP()"). Error paths are `goto error` after manual decrefs. A 3.7 translator has to track the stack symbolically and commit the pops at the end. It can be done, but the generator must infer what 3.12 states outright.

**How the 184 parsed instructions split** (a rough regex classification of the bodies, after removing the `#if ENABLE_SPECIALIZATION` blocks) [run]:

| Group | Count | For Ni |
|---|---|---|
| Plain: stack plus C-API calls (`LOAD_FAST`, `UNARY_*`, `BUILD_*`, generic `BINARY_OP` = `binary_ops[oparg](lhs, rhs)`…) | ~56 | Translatable with a small vocabulary: `GETLOCAL`, `SETLOCAL`, each C call mapped to its Psyco meta. |
| Specialised family members (`BINARY_OP_ADD_INT`, `LOAD_ATTR_SLOT`, `CALL_NO_KW_ISINSTANCE`, `LOAD_GLOBAL_BUILTIN`…) | ~57 | Not code to translate but **specialisation knowledge**: each is `DEOPT_IF(guard)` + a fast path, i.e. a promotion/type switch plus a meta. CPython's developers have written the guards Psyco needs. |
| Control flow, frame and thread state (`JUMPBY`, `POP_JUMP_IF_*`, `FOR_ITER`, `GLOBALS()`, `frame->`) | ~26 | Small, mechanical vocabulary mapped to Ni's core (jumps, merge points, `LOC_GLOBALS`). |
| Calls, frames, generators, returns (`CALL`, `RETURN_VALUE`, `YIELD_VALUE`, `SEND`…) | ~11 | Core: Ni does calls and frames itself. Hand-written. |
| Exceptions (`RAISE_VARARGS`, `RERAISE`, `POP_EXCEPT`…) | ~17 (overcounted) | Core. 3.11+ has no block stack: handlers come from the code object's exception table (`ceval.c:get_exception_handler`). Ni's `SETUP_*`/block-stack unwinding must be rewritten for 3.12 whatever the meta-compiler does. |
| Instrumentation (`INSTRUMENTED_*`) | 17 | Skip (monitoring, PEP 669). |

**Conclusions** [inferred]:
1. On 3.12 the bytecode stage is closer to stage 1 than to a new problem. The stack effects, error exits and restartability are given. The generic instructions mostly reduce to "call the meta for this C function", which is what the slot generators already produce.
2. The specialised families answer a question this project hasn't asked yet: *which* type cases to specialise. They could steer the generated code (which metas to call, which promotions to emit) without being translated themselves.
3. The hard parts are the same on both versions and are core, not translation: calls and frames, exceptions (a new model on 3.11+), generators.
4. For 3.7: rather than a 3.7 front end that derives the declared-effect form from `ceval.c`, write that form directly in 3.12's DSL and generate from it (D26, next section). That way the generator's back half is the part that carries over.
5. 3.12's own globals guard (`LOAD_GLOBAL_MODULE`/`BUILTIN`) uses the dict *keys* version (`ma_keys->dk_version`) plus an entry index, not `ma_version_tag`. That's relevant to QUESTIONS 29.

## Bytecode slice: simple opcodes from the DSL (D26)

`tools/metac/gen_ceval.py` reads `tools/metac/bytecodes37.c` (3.7's opcodes in 3.12's DSL) with CPython's own parser (`tools/metac/cases_generator/`, vendored from 3.12.9) and writes `ni/Python/pycompiler_gen.h`, which `pycompiler.c` includes in its opcode switch. [run]

- **Result:** 39 of the 43 described opcodes are generated, and their hand-written cases are gone from `pycompiler.c` (165 lines):
  - `NOP`, `POP_TOP`, `ROT_TWO`, `ROT_THREE`, `DUP_TOP`, `DUP_TOP_TWO`;
  - `LOAD_CONST`, `LOAD_FAST`, `STORE_FAST`;
  - `UNARY_POSITIVE`/`NEGATIVE`/`INVERT`, every `BINARY_*` and `INPLACE_*` except the matrix ones, `STORE_SUBSCR`, `GET_ITER`.
- **Shape of a case:** the inputs are read with `NTOP` and left on the stack, then the body. Nothing is popped or released until the end, so an error exit is `break`, which leaves the inputs for the unwinding code as the hand-written cases did. `DECREF_INPUTS()` therefore becomes the releases at the end.
- **Vocabulary** (all in `translate()`; anything else is skipped with the reason):
  - `GETLOCAL`/`SETLOCAL(oparg, x)`, `GETITEM(consts, oparg)`, `Py_INCREF`, `Py_DECREF` of an input, `X = Py_NewRef(Y)`;
  - `DECREF_INPUTS()`, `ERROR_IF(cond, error | unbound_local_error)`;
  - `[int|PyObject *] X = F(args)` for a C-API `F`. `assert` and `STAT_INC` are dropped. The `#if ENABLE_SPECIALIZATION` part is dropped and its `#else` part kept.
- **C-API calls are paired by declaration, not by a table.** `F` must be declared in 3.7's `Include/abstract.h` or `object.h`, and `Psyco` + `F[2:]` must be declared in `ni/Objects/pabstract.h` with one more parameter (`po`). The returns must pair up: `PyObject *` with `vinfo_t *` (NULL means error), or `int` with `bool` (false means error).
- **Representation facts, hand-written in the generator:**
  - A local is unbound only if its value is a known NULL (`psyco_knowntobe(x, 0)`), the same rule as `DELETE_FAST`.
  - A constant from the code object is a new compile-time vinfo, which `Py_INCREF` turns into `vinfo_new(CompileTime_New(...))`.
  - `Py_None` as an argument is a `psyco_vi_None()` temporary, released after the call.
- **Restart rule, checked:** the DSL's rule (no INCREF before the last `ERROR_IF`) is extended to Psyco's. No irreversible statement (incref, `SETLOCAL`) may come before the last call or `ERROR_IF`, and no owned temporary may be live at an error exit.
- **Checks against 3.7**, run by `--check` and so by `tools/check.sh`:
  - `ceval.c` has the `TARGET`;
  - the `ceval.c` case calls every C-API function the description calls;
  - the declared stack effect equals `dis.stack_effect()`, run in python3.7. `NOP` is unknown to it.
- **Mutation test** [run]: five wrong descriptions were tried, and each one makes `--check` fail.
  - A wrong stack effect, a wrong function, and an opcode 3.7 doesn't have are reported as mismatches with 3.7.
  - An incref moved before the error exit, and an input that is never released, are refused by the generator ("restart rule", "neither released nor stored").
- **Compared with the hand-written cases:**
  - The generated cases make the same meta calls with the same operands in the same order. `STORE_SUBSCR` is `(container, sub, v)`, and `BINARY_POWER` passes `psyco_vi_None()`.
  - They release in the same order (top first).
  - `STORE_FAST` stores before it pops, where the hand-written case popped first; the effect is the same.
  - `NOP` lost an `#ifdef NOP` it didn't need. The unbound-local error is now one helper (`unbound_local_error`), shared with `DELETE_FAST`.
- **Tests:** `tests/cases/generated_opcodes.py` runs every generated opcode, and every error exit through the unwinding code. It found F28: Ni still raises Python 2's TypeError messages. The generated code isn't the cause; it calls the same metas.

**The same generator on 3.12's own `bytecodes.c`** (`gen_ceval.py --try`, 3.12.9, no change to the generator) [run]: 13 of 186 instructions translate.
- They are `NOP`, `LOAD_CLOSURE`, `LOAD_FAST_CHECK`, `LOAD_FAST`, `LOAD_CONST`, `STORE_FAST`, `POP_TOP`, `END_SEND`, `UNARY_NEGATIVE`, `UNARY_INVERT`, `BINARY_SUBSCR`, `STORE_SUBSCR` and `GET_ITER`.
- `BINARY_SUBSCR` and `STORE_SUBSCR` are the specialising versions; their adaptive-counter code is dropped.
- 58 of the rest declare arrays, conditional outputs or types in their stack effects (`args[oparg]`, `null if (oparg & 1)`). 55 have control flow, and 53 use statements outside the vocabulary.
- `KW_NAMES` was refused only after a rule was added: assigning a variable that isn't an output (3.12's `kwnames` is interpreter state).
- The next vocabulary to add follows from that: array inputs (calls, `BUILD_*`), conditional outputs, and a branch on a run-time truth value (below).

## Failures log

Record here each case where the current approach failed: the CPython file and function, what it couldn't do, and what we did.

| CPython function | Why the generator skips it | What we did |
|---|---|---|
| `longobject.c:long_bool` | `Py_SIZE(v) != 0` reads `ob_size`, which a virtual int doesn't have. The fix is a fact about the representation (the sign of `ob_size` is the sign of the value), not about this C. | Hand-written. Candidate rule: map `Py_SIZE(x)` compared with 0 to the value compared with 0. |
| `longobject.c:long_richcompare` | Not in the number table. `long_compare` is a digit loop with no one-digit fast path; reducing it to one digit is judgement. 3.12's `long_compare` has a compact fast path [inferred], so it may become generatable on 3.12. | Hand-written. |
| `long_mod`, `long_div` | Their fast paths (`fast_mod`, `fast_floor_div`) need exactly one digit (`== 1`), and the backend has no signed divide. | Residual, by the owner's scope (backend-supported operations only). |
| `long_abs` | Branches on `Py_SIZE(v) < 0` and calls other slots (`long_neg`, `long_long`). Needs the same `Py_SIZE` rule as `long_bool`, plus calls between translated slots. | Residual. |
| `long_long` (pos, int, index) | `PyLong_CheckExact(v) ? incref v : _PyLong_Copy(v)`: an identity pattern (P21), not a fast path. | Residual. |
| `long_pow`, `long_divmod`, shifts, `& ^ |`, `long_float`, `long_true_divide` | 3.7's C has no one-digit fast path to translate; they work on digit arrays. | Residual. |
| `floatobject.c:float_float` (pos, float) | `PyFloat_CheckExact(v) ? incref v : new float`: the identity pattern (P21) again, as for `long_long`. | Unary + stays hand-written, and it always makes a new float (KNOWN_BUGS F22). |
| `float_divmod`, `float_floor_div` | They return a tuple (`Py_BuildValue`), or take item 0 of `float_divmod`'s. Copy-through only builds a float. | Residual. Candidate: a virtual tuple of two copy-through results. |
| `float_pow` | Uses `int` locals and many early returns of objects. | Residual. |
| `float___trunc___impl` (nb_int) | Returns an int (`PyLong_FromDouble`), and the backend can't convert a double to a word. | Residual. |
| `floatobject.c:float_richcompare` | Not in the number table. Switches on the type of `w`, with an exact int/float comparison. | Hand-written (P26 whitelist: float/float only). |
| `bytecodes37.c:UNARY_NOT` | `if (err == 0) res = Py_True; else res = Py_False;` is a branch on a run-time value. Ni turns it into a condition code and a virtual bool (`integer_NON_NULL`, `PsycoBool_FromCondition`), and `PsycoObject_IsTrue` returns a vinfo, not an int. | Hand-written. Candidate rule: `int x = F(...)` where F's Psyco counterpart returns a vinfo, then `if (x == 0) A else B` becomes a condition code when A and B are constants. 3.12's `POP_JUMP_IF_*` need the same. |
| `BINARY_MATRIX_MULTIPLY`, `INPLACE_MATRIX_MULTIPLY` | No `PsycoNumber_MatrixMultiply` (until `93b201f`). | `pabstract.c` got both (abstract.c's `binary_op`/`binary_iop` on `nb_matrix_multiply`), and the generator now writes the cases: 41 of 43. |
| `bytecodes37.c:DELETE_SUBSCR` | No `PsycoObject_DelItem`. The hand-written case calls `PsycoObject_SetItem(po, o, key, NULL)`, which is a fact about Ni's API, not about the C. | Hand-written. Fix: give pabstract a `PsycoObject_DelItem`. |
