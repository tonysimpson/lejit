---
written: 2026-11-16
status: stage 1 started (`64f88e7` ints, `749985f` floats); 3.12 bytecode desk read done
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
4. For the 3.7 slice next: the useful lesson is to give the 3.7 generator a symbolic stack, so that it derives a declared-effect form `(inputs -- outputs)` first and then emits the body. That intermediate form is what 3.12 provides, so the second half of a 3.7 generator should carry over.
5. 3.12's own globals guard (`LOAD_GLOBAL_MODULE`/`BUILTIN`) uses the dict *keys* version (`ma_keys->dk_version`) plus an entry index, not `ma_version_tag`. That's relevant to QUESTIONS 29.

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
