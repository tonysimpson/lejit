---
written: 2026-11-16
status: stage 1 started (`64f88e7` ints, `749985f` floats); bytecode stage started (gen_ceval.py, D26); its 3.7 input is `ceval.c` itself (D69), with array and conditional stack effects (D73)
---
# Meta-compiler: approach and tooling options

The aim is in `GOAL.md` (long-term aim) and `DECISIONS.md` D16–D20. The patterns it must produce are in `PATTERNS.md`.

**Rule: use the simplest thing that works.** Move to a heavier tool only when the current one demonstrably fails on real input. When that happens, record the failure here, with the CPython construct that caused it, before you switch.

## Versions (D67)

One Ni source serves every CPython version from 3.7 on; none is dropped.

- **Hand-written, version-free:** the core, the backend (with `llvm-mc` templates, D65), and the generator's rules: the policy table (what to promote, make virtual, guard), representation rules, effect rules. No `#if` on versions anywhere.
- **Generated, per build:** everything that mirrors CPython, from the source of the version being built for (its release tarball): bytecode cases, slots, `abstract.c`, builtins, the call path, layouts, virtual objects' constructors. Not checked in.
- **Bytecode input:** `bytecodes.c` through CPython's parser for 3.12+; for 3.7–3.11 a reader of `ceval.c`'s `TARGET` cases that produces the same form, so everything after it is shared.
- **Mechanisms:** where Ni itself must work differently (entry: frame hook or `PyFunction_SetVectorcall`; tracing; frames), a small detector looks in that version's source for the mechanism, and the matching snippet goes into the generated source (`36b5beb`, D75, "Mechanisms" below).
- **New features** go into the core or the rules, never into one version's output, so regenerating carries them to every version. Each version's tests run on every change.
- **Order:** generate everything possible on 3.7 first, compared with the hand translations, then 3.8, 3.9 and on.
- **At build time** (`9eae61d`, D68) [run]: `setup.py` runs `tools/metac/generate.py --python EXE --out DIR`, which finds the source for EXE's version (`NI_CPYTHON_SRC` or the cached, checksum-checked tarball, `cpython_src.py`) and runs each generator in its `GENERATORS` list. A generator is a module with `build(target)` returning `(header text, report lines)`, or raising `cslots.GenError` to stop the build; `target` is `cslots.Target(src, version, python, pyconfig)`. A new generator is one entry in that list. The headers go to the build directory, never into `ni/`.

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

`tools/metac/gen_long.py` reads 3.7's `Objects/longobject.c` and writes `plongobject_gen.h` (at first checked in, D24; generated at build time since `9eae61d`, D67). Plain token rewriting was enough. [run]

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

`tools/metac/gen_float.py` reads `Objects/floatobject.c` and writes `pfloatobject_gen.h`. The shared parts live in `tools/metac/cslots.py`. [run]

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

*History: until D69 the input was the hand-written `bytecodes37.c`; it is gone, and the next section reads the same opcodes from `ceval.c`. The rest of this section (shape, vocabulary, restart rule, 3.12 trial) is still how the back half works.*

`tools/metac/gen_ceval.py` reads `tools/metac/bytecodes37.c` (3.7's opcodes in 3.12's DSL) with CPython's own parser (`tools/metac/cases_generator/`, vendored from 3.12.9) and writes `pycompiler_gen.h`, which `pycompiler.c` includes in its opcode switch. [run]

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
- **Checks against 3.7**, run every time it generates, so in every build and twice in `tools/check.sh`; a mismatch fails the build:
  - `ceval.c` has the `TARGET`;
  - the `ceval.c` case calls every C-API function the description calls;
  - the declared stack effect equals `dis.stack_effect()`, run in the interpreter being built for. `NOP` is unknown to it.
- **Mutation test** [run]: five wrong descriptions were tried, and each one made the old `--check` fail.
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

## `ceval.c` reader (D69)

`tools/metac/ceval_reader.py` reads the `TARGET(op)` cases of the source's `Python/ceval.c` and gives `gen_ceval.py` the same `InstDef`s CPython's parser gives for a `bytecodes.c`: it writes each case out in that syntax (`inst(NAME, (ins -- outs)) { body }`) and parses the text with the vendored parser. `gen_ceval.definitions` picks the front end by what the source has: `Python/bytecodes.c` if present, else `ceval.c`; no version number. Plain token rules were enough (`ctokens.py`, which can now keep preprocessor lines as tokens so that a conditional inside a case is seen). `python3 tools/metac/ceval_reader.py SRC` prints what it reads. [run]

**How a case is read** (D73 extended D69's rules to stack effects that depend on oparg, `99d9b26`). A symbolic run over the case's statements:
- **Positions.** A stack position is a linear form in oparg (`Lin`), counted from the stack pointer on entry: `-oparg - 1`, `2 * oparg`. An expression in oparg that isn't linear (`oparg & 0xFF`, `oparg >> 8`, `oparg == 3`) is one atom of the form; `int` locals set from such expressions (`totalargs`, `have_fmt_spec`) stand for their value, and `#define`d integer constants (`FVS_MASK`) are read from `Include/*.h` and `ceval.c`. Two positions are ordered by their difference, at sixteen opargs (1 to 9, 255 to 258, 511, 513, 65535) if it isn't a constant; a difference whose sign changes is refused.
- **Stack.** `POP`, `TOP`, `SECOND`, `THIRD`, `FOURTH`, `PEEK(n)` read input slots, assigned to a variable (which then names the slot) or inside an expression (then a reference, named at the end); `PUSH`, `SET_TOP`…`SET_FOURTH`, `SET_VALUE(n, x)` write slots; `STACKADJ`, `STACK_GROW`, `STACK_SHRINK`, `stack_pointer += n` move the pointer; `Py_DECREF(POP())` pops and releases, `(void)POP()` pops. At the end every position touched cuts the stack into intervals. Inputs are the intervals below the entry pointer: named by the variable read from them, `values[n]` if a loop or a pointer reached them, `args[n]` if a helper popped them, `unused[n]` if nothing touched them (they must then be left in place); outputs are the intervals below the final pointer, named by the variable written there, or by the input left in place (`list, unused[oparg - 1], v -- list, unused[oparg - 1]`, as 3.12 writes `LIST_APPEND`). An interval of size `(C)`, a 0/1 atom, is a conditional one, `step if (oparg == 3)`. Refused: reading a slot it wrote or popped, two names for one input, overwriting or dropping an input it didn't read, pushing a slot never set, an array cut in two.
- **Loops.** `while (--oparg >= 0)` or `while (oparg--)` whose body pops k values per turn (and does nothing else to the stack) reads an array of k·oparg inputs; it is written out as `for (int i = oparg; --i >= 0; ) { … values[k * i + j] … }`, and oparg may not be used after it (C counts it down). `for (i = N; i > 0; i--)` whose body reads `PEEK(a * i + b)` for a values of b in a row reads an array of a·N inputs, element `a * N - a * i + (bmax - b)` (`BUILD_MAP`'s pairs: `values[2 * oparg - 2 * i]`, `values[2 * oparg - 2 * i + 1]`). A loop whose body releases what it pops releases the array; one that releases only some of it is refused.
- **Pointers.** `stack_pointer - n` handed to a call reads the n slots below the pointer (an array, written as its name: `_PyUnicode_JoinArray(empty, values, oparg)`); `stack_pointer + n` handed to a call and then `STACKADJ(n)` are n outputs the call wrote (`unused[n]`). Any `stack_pointer` left in the text is rewritten from the entry pointer, as 3.12's bodies see it (`stack_pointer + oparg - 1` in `UNPACK_SEQUENCE`).
- **Helpers.** A local copy of the stack pointer (`sp = stack_pointer`) handed by address to a function of `ceval.c` (`call_function(&sp, …)`) moves by what that function's body pops: the shape read is `PyObject **B = (*P) - E;` then `while ((*P) > B) { w = EXT_POP(*P); Py_DECREF(w); }`, and no other change to `*P`; E (`oparg + 1`) is taken in the call's arguments. The slots it pops are consumed and released. `stack_pointer = sp` moves the pointer to the copy.
- **Branches.** On a compile-time condition (in oparg) whose then-arm starts `x = POP()` and whose else-arm is empty or `x = NULL`, or `x = C ? POP() : NULL`: a conditional input `x if (C)`, the rest of the arm under `if (C)`. A branch on `opcode` is folded (in a case entered by its own `TARGET`, opcode is that case) when the test decides it. Any other branch that touches the stack, jumps, or releases or rebinds a variable naming an input is run on both arms as separate paths; each path is checked on its own (FORMAT_VALUE has four), and where they meet they must agree on the stack. In the arm of `if (x == NULL)` x counts as released. A branch that does none of that is passed through as text.
- **Error exits.** `if (C) goto error;`, or a block ending in `goto error` whose statements are releases of inputs and at most one known error statement, becomes `ERROR_IF(C, label)` as before (D69); any other block becomes `if (C) { statements; ERROR_IF(true, error); }` (3.12's own form), the input releases left out. `if (C) A else { …; goto error; }` is the exit on `!(C)`, then `A` (`UNPACK_*`). Every exit is checked once the intervals are known, on every path to it: each input is either in its slot or released or stored, never both, and any other slot written holds the value `C` says is NULL.
- **Generic path.** `if (T) A else B`, where `T` only tests types (`Py*_Check`, `Py*_CheckExact`) and, with them, an exact type's size against oparg (`PyTuple_GET_SIZE(seq) == oparg`, `UNPACK_SEQUENCE`'s two fast paths), keeps `B`. This is the one judgement rule: CPython's exact-type fast paths (`unicode_concatenate` in `BINARY_ADD`/`INPLACE_ADD`, `PyUnicode_Format` in `BINARY_MODULO`, the tuple and list unpacking loops) give the same result as the generic call, and Psyco's counterpart of the generic call dispatches on the type itself. It is what `bytecodes37.c` wrote by hand, and what 3.12 did by moving them to specialisations.
- **Duplicates.** An output that is the same variable as another gets a new name (`top2`), and the `Py_INCREF` that made the extra reference becomes `top2 = Py_NewRef(top)`, as `bytecodes.c` writes a copy. The number of increfs must match.
- **Tidying.** `T x;` then `x = E;` becomes `T x = E;` at the case's top level; `T *a, *b;` is split; inputs and outputs lose their declarations (the DSL declares them). `DISPATCH`/`FAST_DISPATCH` must be the last statement of the main path; `PREDICT` is dropped. `PREDICTED` labels sit outside the cases.

**Refused, by kind** (the first construct that stops the reader; 3.7.17, 119 cases):

| Kind | Count | Opcodes, and the construct |
|---|---|---|
| core | 18 | Block stack (`POP_BLOCK`, `POP_EXCEPT`, `SETUP_FINALLY`, `SETUP_WITH`, `SETUP_ASYNC_WITH`), frame state `f->` (`LOAD_GLOBAL`, `LOAD_NAME`, `STORE_NAME`, `MAKE_FUNCTION`, `WITH_CLEANUP_START`…), `tstate` (`FOR_ITER`) |
| control flow | 9 | A dispatch in a branch on a run-time value (`UNARY_NOT`, `POP_JUMP_IF_*`, `JUMP_IF_*_OR_POP`, `DELETE_FAST`, `DELETE_DEREF`), `goto error` after its own cleanup (`GET_ANEXT`, `WITH_CLEANUP_FINISH`) |
| shared code | 8 | `goto fast_block_end` or `why = WHY_*` (`RETURN_VALUE`, `RAISE_VARARGS`, `END_FINALLY`, `BREAK_LOOP`, `CONTINUE_LOOP`), `goto fast_yield` (`YIELD_VALUE`, `YIELD_FROM`), `goto dispatch_opcode` (`EXTENDED_ARG`) |
| stack | 5 | `PUSH(NULL)` (`LOAD_METHOD`, `GET_AITER`), arms that leave different stacks (`GET_YIELD_FROM_ITER`), an error exit with a pushed value on the stack (`BEFORE_ASYNC_WITH`), an error exit with an input neither on the stack nor released (`CALL_FUNCTION_EX`: CPython 3.7 leaks `kwargs` when `PyDict_New` fails; failures log) |
| falls through | 4 | `SETUP_LOOP`, `SETUP_EXCEPT` into `SETUP_FINALLY`; `BUILD_TUPLE_UNPACK_WITH_CALL`, `BUILD_TUPLE_UNPACK` into `BUILD_LIST_UNPACK` |
| preprocessor | 1 | `JUMP_ABSOLUTE` (`#if FAST_LOOPS`) |

No case is refused for oparg any more. Of the 18 D69 refused for it, 17 are read: `LIST_APPEND`, `SET_ADD`, `MAP_ADD`, `UNPACK_SEQUENCE`, `UNPACK_EX`, `BUILD_STRING`, `BUILD_TUPLE`, `BUILD_LIST`, `BUILD_SET`, `BUILD_MAP`, `BUILD_CONST_KEY_MAP`, `BUILD_LIST_UNPACK`, `BUILD_SET_UNPACK`, `BUILD_MAP_UNPACK`, `BUILD_MAP_UNPACK_WITH_CALL`, `BUILD_SLICE`, `FORMAT_VALUE`; `CALL_FUNCTION_EX` is refused for CPython's leak. The three calls (`CALL_FUNCTION`, `CALL_FUNCTION_KW`, `CALL_METHOD`; `(args[oparg + 1] -- res)`, `(args[oparg + 1], names -- res)`, `(meth, args[oparg + 1] -- res)`) and `PRINT_EXPR`, `LOAD_DEREF`, `GET_AWAITABLE` (general error blocks) are read too. Their effects, as 3.12 writes them where it has the opcode: `BUILD_TUPLE (values[oparg] -- tup)`, `BUILD_MAP (values[2 * oparg] -- map)`, `BUILD_CONST_KEY_MAP (values[oparg], keys -- map)`, `BUILD_SLICE (start, stop, step if (oparg == 3) -- slice)`, `FORMAT_VALUE (value, fmt_spec if ((oparg & FVS_MASK) == FVS_HAVE_SPEC) -- result)`, `UNPACK_EX (seq -- unused[(oparg & 0xFF) + (oparg >> 8) + 1])`, `MAP_ADD (map, unused[oparg - 1], value, key -- map, unused[oparg - 1])`, `BUILD_MAP_UNPACK_WITH_CALL (func, unused, values[oparg] -- func, unused, sum)`.

**Read: 74; generated: 49** (`99d9b26`). Five more than D69: `LIST_APPEND`, `MAP_ADD`, `BUILD_TUPLE`, `BUILD_LIST`, `BUILD_SLICE`, whose hand-written cases are gone from `pycompiler.c`. Additions to the back half, each general:
- **Effects.** Array, conditional and `unused` inputs, sized by the same `Lin` forms; the bottom inputs an instruction leaves as they are (same name and size in inputs and outputs) are read where they are (`NTOP(oparg + 2)`) and neither popped nor pushed. An array input is `STACK_POINTER() - (n)`, a conditional one `(C) ? NTOP(d) : NULL`. Releases are still top first: a scalar's after the pops above it, an array's as `POP_DECREF` in a loop, a conditional one's `vinfo_xdecref`.
- **Counterparts** are looked for in every `Include/*.h` and every `ni/Objects/p*.h` (was `abstract.h`, `object.h`, `pabstract.h`, `pobject.h`): `PyList_Append` → `PsycoList_Append`, `PyDict_SetItem` → `PsycoDict_SetItem`. A counterpart without `po` that returns a `vinfo_t *` builds a virtual object and can't raise, so the `ERROR_IF` on its result is dropped (`PySlice_New` → `PsycoSlice_New`).
- **PATTERNS E, a container filled from the stack.** `X = PyT_New(n)`, its `ERROR_IF`, and the loop that moves every item of an input array of size n into it (`PyT_SET_ITEM(X, i, values[i])`) become `X = PsycoT_New([po,] n, values)` when `PsycoT_New` takes `(int, vinfo_t **)` after `po`; the array is then released, because Ni's constructors take new references to their items. No per-opcode table: `PyTuple_New` → `PsycoTuple_New` (virtual, can't fail), `PyList_New` → `PsycoList_New` (with `po`, can fail).
- **Calls** (a copy of the stack pointer handed to a helper) are skipped as core.

**Same calls in the same order** as the hand-written cases they replace [read], with the same operands: `PsycoList_Append(po, list = NTOP(oparg + 1), v = NTOP(1))`, `PsycoDict_SetItem(po, map = NTOP(oparg + 2), key = NTOP(1), value = NTOP(2))`, `PsycoTuple_New(oparg, STACK_POINTER() - oparg)`, `PsycoList_New(po, oparg, STACK_POINTER() - oparg)` with its NULL test, `PsycoSlice_New(start, stop, step or NULL)`; then the same releases in the same order. `tools/samecode.py 4a88d79 0f5af55`: 83 units the same, 0 differ, 4,075 of 4,075 code objects the same [run].

**Compared with the `bytecodes37.c` output** (each case's text, with variables renamed by order of appearance) [run]: 30 cases identical, 10 differ only in variable names (ceval.c's `dividend`/`divisor`, `sum`, `diff`, `quotient`, `mod`), and `DUP_TOP_TWO` increfs `top` before `second`, as ceval.c does (no failure point between them, same effect). The cases come in `ceval.c`'s order, and the list of what isn't generated is now every opcode, by kind. The three attribute cases make the same calls in the same order as the hand-written ones they replace.

**Checks, in every generation:** every effect read must equal `dis.stack_effect` run in the target interpreter, at each of the opargs 0, 1, 2, 3, 4, 5, 7, 8, 255, 256, 257, 258 and 513 (the sizes and conditions evaluated at each; D73); a name that isn't an opcode is a mismatch. A mismatch is a `GenError`, so the build stops. The old checks (the `TARGET` exists, `ceval.c` calls the functions the description calls) are true by construction now and were dropped.

**Mutation test** [run]: three wrong readers (a `STACKADJ` that doesn't move, a `PUSH` that doesn't move, an output dropped from two-input cases) each stop generation with `dis.stack_effect` mismatches. Six edits to a copy of `ceval.c`: `STORE_FAST` with `TOP()` for `POP()` is a mismatch (effect 0, dis −1); `STACKADJ(-2)` in `STORE_SUBSCR`, `BINARY_POWER` without `SET_TOP`, `BINARY_ADD` reading `SECOND()`, `BINARY_SUBSCR` not releasing `sub`, and `LOAD_FAST` increfing before its error exit are each refused (the error-exit check, or the restart rule), so the opcode drops out of the output and is left to the hand-written code, never generated wrong.

**Mutation test for D73** [run] (a script run by hand, as for D69): four wrong readers and eight edits to a copy of `ceval.c`, each against the five new cases' text. A conditional input taken as always there, and `call_function` popping one more, stop generation with `dis.stack_effect` mismatches; a count-down loop popping one fewer per turn (`BUILD_MAP`) cuts its array in two, and `PEEK(2*i)` read as `PEEK(i)` reads the stack unevenly: both refused. `BUILD_SLICE` testing `oparg == 2` is a mismatch; `MAP_ADD` with `STACKADJ(-1)` can't order its positions; `BUILD_TUPLE` counting `--oparg > 0` is an unknown loop; `BUILD_TUPLE` setting item 0 doesn't match the fill loop (skipped); `BUILD_LIST` popping two per item is refused; `LIST_APPEND` without `Py_DECREF(v)`, `BUILD_SLICE` without `Py_XDECREF(step)` and `BUILD_LIST`'s error exit moved after its pops fail the error-exit check. In the back half, `MAP_ADD`'s output renamed (so nothing is kept in place) is skipped. None generated a different case. Not caught by any check: a passthrough input moved by one slot (`PEEK(oparg + 1)` in `LIST_APPEND`) leaves the effect the same; only the behaviour tests would see it (risk, STATUS).
## Policy table (D71)

The generator's judgement is data: `tools/metac/policy_table.py`, checked by `tools/metac/policy.py`, which `generate.py` runs before any generator, so a mismatch fails the build. `python3 tools/metac/policy.py --src SRC` runs the checks alone; `--shape KEY…` prints a definition's current shape. [run]

**Keys and shapes.** Every key names a definition in CPython's source, found by `tools/metac/csource.py`: `path:name` for a function, a macro, a struct (by tag or typedef) or an initialised variable; `Python/ceval.c:TARGET(OP)` for an opcode case. An entry carries `shape`, the first 12 hex digits of a sha256 of the definition's tokens (comments and spacing don't count). A shape that differs means the C changed: the check names the key and both shapes, and someone reads the entry again against the new C and adds the new shape. `shape` may be a tuple, one per source checked, so no version number is written anywhere (D67). Ni's side is named the same way (`ni/Objects/pabstract.c:PsycoNumber_Add`, `ni/Python/pycompiler.c:case LOAD_GLOBAL`, or a generator `tools/metac/gen_ceval.py:translate`) and must exist.

**Sections.**

| Section | What it holds | Size |
|---|---|---|
| `ENTRIES` | One per CPython definition a hand translation mirrors (INVENTORY's translated and mixed rows, dead code left out): its shape, the Ni code that carries it today, and policy items `(kind, value[, note])` | 250 entries, 93 with items, 186 items |
| `RULES` | What is one rule, not per-function entries, with its decisions and where Ni applies it | 11 |
| `LIMITS` | Numbers in Ni's code (`MEGAMORPHIC_MAX` 5, `MP_BLOCK_CAP` 6, `MAX_MP_DISTANCE` 170, `INLINE_MAXIMUM_WEIGHT` 6, `VLIST_LENGTH_MAX` 3, `CT_TUPLE_LOAD_SIZE_LIMIT` 15, `FAST_MAX` 5, `MAX_CALL_FREES` 64, `STORE_CACHE_MAX_REFRESHES` 64) | 9 |
| `LIGHT_OPCODES` | Opcodes that don't count towards the inlining weight | 22 |
| `TYPES` | Per type struct: its type object, mutable and immutable fields, the virtual form's weights (P15, step 6) | 12 |
| `CANNOT_BLOCK` | D53's list, as the CPython functions behind it | 38 |
| `NO_DEL_TYPES`, `SAFE_DESCRIPTORS` | The type whitelists of F55's decref rule and of attribute specialisation | 6, 7 |
| `SEEDS`, `EXTERNAL`, `ASSUMPTIONS` | The effect rules' starting points (below) | 25, 28, 6 |

**Vocabulary** (`policy.VOCABULARY`; every item's kind and value must be in it):
- `promote`: `type`, `verify_type`, `static_type`, `value`, `constant`, `known_only`. `mega`: `type`, `value`, `residual`.
- `guard`: `version_tag`, `store_guard`, `dict_version`, `first_seen`, `memory`, `identity`, `constant_class`, `no_fold_heap`, `generic_for_heap`.
- `virtual`: `result`, `argument`, `fast_path`, `concat`, `fuse`, `args_tuple`.
- `shortcut` (in this C) and `extra` (no C behind it in this source: the generator must emit it from the table, or same code shows a loss): `one_digit`, `const_fold`, `type_whitelist`, `inline_index`, `inline_read`, `no_tuple_call`, `known_length`, `identity`, `power_of_two`, `small_presized`, `word_compare`, `skip_check`, `specialise_only`, `compile_time_unpack`, `direct_meta`.
- `likely`: `true`, `false` (the note names the condition). `restype`: a type object of the source.
- `ref`: `need_reference`, `held`, `no_steal`, `incref`, `keep_alive`. `forget`: `before_branch`, `direct_call`, `handler`, `store`.
- `limit`: `nested_weight`, `max_args`, `depth`, `max_frees`. `other`: `compile_time_hash`, `ends_block`, `untraced_only`, `trace_fallback`, `mutable_fields`.

Items by kind: shortcut 41, virtual 31, likely 28, restype 24, promote 17, ref 11, other 9, guard 8, extra 5, forget 4, limit 4, mega 4.

**Rules instead of entries** (`RULES`): `slot_guard` (D54: every slot read on a possibly-heap type through `Psyco_TypeSlot`, resuming CPython's algorithm where it changed; replaces 22 guard items and the core rows' slot macros), `restart` (PATTERNS rule 1, F24, F25), `error_unlikely` (P05: 7 items), `effects` (computed, below: 48 items in translated and mixed rows, 72 in all), `forget` (D59, D62, D63, F55), `megamorphic` (P04, D43, D66), `eval_breaker` (D45, D53), `type_test_generic_path` (D69: the reader's one judgement rule, now named here), `gc_ignored`, `allocator_pure`, `virtual_types`. INVENTORY's DRIFT items (51) are not policy: they are differences a faithful translation loses.

**Checks against Ni's code.** Where the table copies something Ni still hard-codes, the two must agree until a generator emits the code from the table: `LIMITS` against the `#define`s; `LIGHT_OPCODES` against `mergepoints.c:IS_LIGHT_INSTR`, and each against `Include/opcode.h`; `CANNOT_BLOCK` against `psyfunc.c:cannot_block_init` (methods and builtins resolved through the source's `PyMethodDef` tables, clinic headers included); `NO_DEL_TYPES` against both copies (`pobject.c:collect_undeletable_vars`, `ipyencoding.h:psyco_decref_rt`); `SAFE_DESCRIPTORS` against `pobject.c:c_descriptor`/`safe_descriptor`; `TYPES` fields against the struct, weights against the `INIT_SVIRTUAL` calls.

**What moved.** `gen_float.py`'s `LIBM` (the C functions copied code may call) is now `EXTERNAL`'s pure functions, and `ERRORS` the `ASSUMPTIONS` marked `raises`. Generated headers are byte-identical before and after (`plongobject_gen.h`, `pfloatobject_gen.h`, `pycompiler_gen.h`) [run]. `gen_long`'s and `gen_ceval`'s tables (`CTORS`, `INT_TYPES`, `OPS`, `LABELS`, `DROPPED`, `CONSTANTS`, `CODE_TUPLES`) say how to write the output, not what to choose, so they stay.

**Mutation test** [run]: a copy of the source with one token changed in `abstract.c:PyNumber_Add` fails generation: `Objects/abstract.c:PyNumber_Add: shape 6cb97b65d5ef, the entry was written for 773b92704612`, and no header is written. An unknown key, an unknown value, a Ni name that doesn't exist, a wrong limit and a field not in its struct each fail, by name.

## Effect rules (D71)

`tools/metac/effects.py --src SRC [-v] [--why NAME…]` (a tool, not run by the build). It builds a call graph over the source's `Include`, `Objects`, `Python`, `Modules` and `Parser`, 19,137 nodes, functions and macros alike, plus Ni's own helpers read the same way, and finds for each node [run]:
- **python** (may run Python): a call through a function pointer (`->f(`, `(*f)(`, a parameter), or a decref of anything but a singleton or a local declared with a `NO_DEL_TYPES` struct; else a callee that may. 6,803 nodes.
- **blocks** (may release the GIL): `Py_BEGIN_ALLOW_THREADS`, `PyEval_SaveThread`. 4,474.
- **pure**: no write through a pointer or to a file-scope variable (refcounts don't count), no read of a mutable file-scope variable (type objects and `PyExc_*` taken as constant), no python, every callee pure. An allocator is impure; `allocs` records who reaches one (rule `allocator_pure`).
- **raises**: a call through a pointer, `PyErr_SetObject`/`PyErr_Format(V)`, or an assumption `raises`. A decref doesn't raise (an error in `__del__` is printed).

The walk stops at `SEEDS` (decref, gc, blocks, sets_error, alloc, fatal: `Py_FatalError` and `Py_UNREACHABLE` end the process) and at `ASSUMPTIONS`, which say what Ni accepts without proof: `PyErr_SetString` (no Python, no blocking, raises: a built-in class and an ASCII message), `PyDict_GetItem` and `_PyDict_GetItem_KnownHash` (F34: an exact str key), `PyErr_Restore` (an old exception's dealloc runs no Python), `function_call` (no blocking: D53, the body tests `eval_breaker`), `PyNumber_Index` (D53's str methods). A header's macro shadows a function of the same name (the default configuration's `_Py_ForgetReference` is a macro).

**Compared with every hand-written flag** [run]. The 225 residual call sites in the built sources (`psyco_generic_call`, `Psyco_META1`…`4`): 13 have flags built at run time (unproven), 157 resolve to a C function (by name, through Ni's compat macros or a one-call header macro, or through a type's slot initialisers such as `PyList_Type.tp_as_sequence->sq_concat`), 55 call a pointer. *Agree* means the flag says what the analysis finds; *cautious*, that Ni assumes less than is proven (safe, but a lost optimisation); *disagree*, that Ni assumes more.

| Flag | Agree | Cautious | Disagree | Other |
|---|---|---|---|---|
| `CfNoPython`/`CfPure` (no Python), 212 sites | 182 | 28 | 2 | |
| `CfPure`, 212 | 197 | 6 | 2 | 7 allocators (rule `allocator_pure`) |
| error convention (checks what it may raise), 157 | 143 | 6 | 8 | |
| `CfNewRef` against `refcounts.dat`, 33 | 33 | | | |
| `CfNoBlock` or D53's list, 157 | 74 | 82 | 1 | |
| D53's list itself, 38 | 37 | | 1 | |

A site whose result the caller tests itself (`runtime_NON_NULL_f(po, psyco_generic_call(...))`) counts as checked. The 82 cautious `CfNoBlock` sites are the measure of what computing D53's list would gain: calls after which no `eval_breaker` test is needed.

**The 13 disagreements, classified:**
- **Bugs in Ni's flags:** `float_richcompare` (error, `pfloatobject.c:pfloat_richcompare`) is KNOWN_BUGS F104 already; `pycompiler.c:cimpl_vt_traceback` (NoError, but leaves a MemoryError set when it returns None) is new, F105.
- **Limits of the analysis** (failures log): `float_richcompare`'s no-Python (a decref of an int temporary declared `PyObject *`) and pure (it reads `_PyLong_One`, a variable set once at start-up); the pointer `fn` at `pfloatobject.c:pfloat_richcompare` (one of the `cimpl_fp_*_fp` compare helpers, chosen in a switch); `cimpl_unicode_eq` (`PyUnicode_READY` of a legacy string); `PyErr_ExceptionMatches`, `PyErr_GivenExceptionMatches`, `cimpl_generator_stop` (`PyTuple_Size`'s bad-call error, under a `PyTuple_Check`); `cimpl_push_exc_info` (`PyErr_NormalizeException` replaces the exception, as `ceval.c` lets it); `cimpl_rt_traceback` (a failure replaces the exception being unwound, by design); `PyFunction_NewWithQualName` blocks and is in D53's list (only through a UTF-8 error handler for an ASCII name).

## Mechanisms (D75)

Where Ni's own mechanism must differ between versions (D67 (4)), `tools/metac/mechanisms.py` asks the target's source what it offers and picks a snippet; `generate.py` writes it into the build directory. GENERATION.md step 9. [run] unless marked.

**The machinery.**
- **Detectors** are small functions in `mechanisms.py`, each answering one question from the source with the definition it read. Header questions search every `Include/**/*.h`, so a definition moving between `pystate.h`, `cpython/pystate.h` and `internal/` doesn't change the answer. Structs are found by tag or typedef name, fields by the last name in each declaration (both `#if` branches count).
- **`VARIANTS`** (same file) is the one place answers become snippets: per mechanism, its detectors in order and a table from the tuple of answers to a variant, the file `ni/mechanisms/<mechanism>/<variant>.h`. Answers not in the table stop generation (and so the build) with the answers found; `generate.py` then removes the old headers.
- **Output:** `mechanism_entry_gen.h`, `mechanism_tracing_gen.h`, `mechanism_frames_gen.h`, each with the answers and their evidence in a comment, an include guard, `#line` back to the snippet file (compiler errors point there), and the snippet.
- **Stubs:** a variant not written yet is one `#error "... is not written yet (file; D75)"`, so a build that selects it stops in the compiler with that message.
- `python3 tools/metac/mechanisms.py SRC...` prints the answers for source trees; `python3 tests/mechanisms.py` checks the table below and that unmapped answers fail generation (a copy of 3.7's source with a `Python/instrumentation.c` added gives `tracing_flag=tstate, monitoring=yes`).

**Detector answers** (`tests/mechanisms.py`, tarballs from python.org, hashes from pyenv's python-build definitions and matched by the downloads):

| Question | 3.7.17 | 3.8.20 | 3.11.11 | 3.12.9 | 3.13.2 |
|---|---|---|---|---|---|
| `eval_hook_frame`: what the PEP 523 hook is given | `frame_object` | `frame_object` | `interpreter_frame` | `interpreter_frame` | `interpreter_frame` |
| `function_vectorcall`: `PyFunction_SetVectorcall` declared | no | no | no | yes | yes |
| `function_watchers`: `PyFunction_AddWatcher` declared | no | no | no | yes | yes |
| `calls_bypass_vectorcall`: specialised calls skip a replaced vectorcall | none (no specialiser) | none | **yes** (`_PyFunction_GetVersionForCurrentState` versions any function) | no (it refuses a non-default vectorcall) | no (`PyFunction_SetVectorcall` clears the version) |
| **entry** | `frame_eval_hook` | `frame_eval_hook` | `frame_eval_hook_interp` (stub) | `vectorcall` (stub) | `vectorcall` (stub) |
| `tracing_flag`: where `use_tracing` is | `_ts` | `_ts` | `_PyCFrame` | none | none |
| `monitoring`: `Python/instrumentation.c` (PEP 669) | no | no | no | yes | yes |
| **tracing** | `tstate_use_tracing` | `tstate_use_tracing` | `cframe_use_tracing` (stub) | `monitoring` (stub) | `monitoring` (stub) |
| `frame_locals`: where arguments and locals are | `_frame.f_localsplus` | `_frame.f_localsplus` | `_PyInterpreterFrame.localsplus` | same | same |
| `current_frame`: the innermost frame | `_ts.frame` | `_ts.frame` | `_PyCFrame.current_frame` | `_PyCFrame.current_frame` | `_ts.current_frame` |
| **frames** | `frame_object` | `frame_object` | `interpreter_frame_cframe` (stub) | `interpreter_frame_cframe` (stub) | `interpreter_frame` (stub) |

On 3.11 the hook stays usable although vectorcall isn't: the specialiser refuses Python calls while a hook is set (`specialize.c:specialize_py_call`, `SPEC_FAIL_CALL_PEP_523`), and `CALL` inlines a Python call only when `eval_frame == NULL` (`ceval.c`). [read] 3.8 needs no new snippet: its answers are 3.7's.

**What moved into snippets on 3.7** (`36b5beb`; `tools/samecode.py 2d2d3d5 36b5beb`: 83 units same, 0 differ):
- **Entry** (`frame_eval_hook.h`): `NiEval_EvalFrame` and its installation (`ni_entry_install`), reading `f_code`, `f_globals`, `f_locals`, `f_builtins` from the frame. The rest of the old hook is the core's `eval_hook.c:ni_entry_point(co, globals, locals, builtins)` (generator, tracing and module checks, merge points, first-call compile, globals and builtins check), returning the code to run or NULL; `NiCode_Run` runs it on a `ni_frame_t`.
- **Tracing** (`tstate_use_tracing.h`): `ni_tracing_now(tstate)`, the hook's test, and `ni_tracing_test(po, vts)`, the same test emitted by `psyfunc.c:tracing_on` (`psyco_int32_nonzero_in` at `offsetof(PyThreadState, use_tracing)`). Loading the thread state stays core (`load_tstate`).
- **Frames** (`frame_object.h`): `ni_frame_t` (here `PyFrameObject`) and accessors for the frame chain and entry: `ni_frame_current`, `ni_frame_set_current`, `ni_frame_back`, `ni_frame_object`, `ni_frame_code`, `ni_frame_globals`, `ni_frame_args`, `ni_frame_set_executing`. Used by `NiCode_Run`, `frames.c` (`pvisitframes`, `psyco_build_pyframe`, `psyco_running_real_frame`) and `pycompiler.c:cimpl_new_frame`. On 3.7 every one is a field access; the snippet exists because on 3.11+ the chain is a different structure (interpreter frames, frame objects made on demand), not different offsets.

**Not covered** [read]: fields of frame *objects* (`f_lasti`, `f_lineno`, emulated frames' `f_back`, `PyFrame_New`) stay in `frames.c` and `pycompiler.c`; `pyruntime.c`'s `_PyRuntime` addresses (the thread state, `eval_breaker`) and `ni_eval_breaker_at_entry` are 3.7 internals outside the three mechanisms, and will need their own detector or generated layout; so will the exception state (PORT312.md). An `NI_ALL_STATIC=1` build fails in `psyco.h` (`staticforward`, KNOWN_BUGS C55), so the one-unit build wasn't checked; the generated headers have include guards for it.

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
| `ceval.c:UNARY_NOT` (was `bytecodes37.c`) | `if (err == 0) { …SET_TOP(Py_True); DISPATCH(); } else if (err > 0) …`: a branch on a run-time value, with a dispatch in each arm. Ni turns it into a condition code and a virtual bool (`integer_NON_NULL`, `PsycoBool_FromCondition`), and `PsycoObject_IsTrue` returns a vinfo, not an int. | Hand-written. Candidate rule: `int x = F(...)` where F's Psyco counterpart returns a vinfo, then `if (x == 0) A else B` becomes a condition code when A and B are constants. 3.12's `POP_JUMP_IF_*` need the same. |
| `BINARY_MATRIX_MULTIPLY`, `INPLACE_MATRIX_MULTIPLY` | No `PsycoNumber_MatrixMultiply` (until `93b201f`). | `pabstract.c` got both (abstract.c's `binary_op`/`binary_iop` on `nb_matrix_multiply`), and the generator now writes the cases: 41 of 43. |
| `ceval.c:DELETE_SUBSCR` | Read; no `PsycoObject_DelItem`. The hand-written case calls `PsycoObject_SetItem(po, o, key, NULL)`, which is a fact about Ni's API, not about the C. | Hand-written. Fix: give pabstract a `PsycoObject_DelItem`. |
| `ceval.c:BINARY_ADD`, `INPLACE_ADD`, `BINARY_MODULO` | A fast path under an exact-type test (`unicode_concatenate`, `PyUnicode_Format`). Not a failure, but the reader's one judgement rule. | The reader keeps the generic `else` branch (D69). If a later version's fast path gives a different result from the generic call, the rule is wrong there. It is now named in the policy table (`RULES` `type_test_generic_path`, D71); an exception for one case would be an entry keyed by its `TARGET`. |
| `ceval.c:LOAD_CLOSURE`, `STORE_DEREF` | Read; `freevars[oparg]` and `PyCell_GET`/`PyCell_SET` aren't in the back half's vocabulary. | Hand-written. Candidate: `freevars[oparg]` as a representation rule (Ni's cells), as `GETLOCAL` is. |
| `ceval.c:COMPARE_OP` | Read; `cmp_outcome(oparg, left, right)` is a static function of `ceval.c` that switches on `oparg`. | Hand-written (condition codes). Needs `cmp_outcome` translated, with the switch on a compile-time `oparg` folded. |
| `ceval.c:IMPORT_NAME`, `IMPORT_FROM` | Read; `import_name(f, …)` and `import_from` are `ceval.c` helpers, and `import_name` takes the frame. | Hand-written. |
| `ceval.c:JUMP_FORWARD` | Read; `JUMPBY(oparg)` is a jump, not in the vocabulary. | Hand-written. Jumps are a small vocabulary for step 8. |
| `ceval.c:PRINT_EXPR`, `LOAD_DEREF`, `BUILD_CONST_KEY_MAP` | An error exit that sets its own exception (`PyErr_SetString`, `format_exc_unbound`). The reader knew one error statement, the one 3.12 names as a label. | Read since D73, as `if (C) { …; ERROR_IF(true, error); }`; the back half has no `if`, so they stay hand-written. Candidate: such a block whose statements only set an exception, emitted on the `break` path. |
| `ceval.c:SET_ADD`, `BUILD_SET` | Read (D73); `PySet_Add` and `PySet_New` have no Psyco counterpart. The hand-written cases call them at run time (`psyco_generic_call(po, PySet_Add, CfCommonIntZeroOk, …)`), which needs the effect flags of GENERATION step 3, not a translation. | Hand-written. Candidate: a C-API call with no counterpart becomes a residual call with the policy table's flags. |
| `ceval.c:BUILD_STRING` | Read (D73); `PyUnicode_New` and `_PyUnicode_JoinArray` have no counterpart. The hand-written case calls `cimpl_build_string` with up to four parts as words, or a virtual tuple. | Hand-written. Needs the residual-call rule, and the four-word form is judgement (policy). |
| `ceval.c:UNPACK_SEQUENCE`, `UNPACK_EX` | Read (D73): `(seq -- unused[oparg])`, the outputs written by `ceval.c`'s `unpack_iterable` through `stack_pointer + oparg - 1`. The back half can't push an array it didn't name, and `unpack_iterable` is a `ceval.c` helper. The hand-written `UNPACK_SEQUENCE` also unpacks a virtual tuple of the right length with no code (P22), which the reader's generic-path rule drops from the C. | Hand-written. Needs `unpack_iterable` translated (as `cimpl_unpack_iterable` is by hand) and the virtual-tuple shortcut as a policy entry. |
| `ceval.c:BUILD_MAP`, `BUILD_CONST_KEY_MAP` | Read (D73). A loop with a call and an error exit in each turn (`PyDict_SetItem`, then `Py_DECREF(map); goto error`): the back half translates no loop but the fill loop. `_PyDict_NewPresized`'s counterpart `PsycoDict_NewPresized` isn't found by `'Psyco' + fn[2:]` (the leading underscore). `BUILD_CONST_KEY_MAP` takes its keys from a compile-time tuple (`PsycoDict_SetItemC`), which is a fact about Ni's representation. | Hand-written; the hand-written cases make the calls the C makes. Candidate: counted loops over an input array, an error exit inside one releasing the locals it names. |
| `ceval.c:BUILD_LIST_UNPACK`, `BUILD_SET_UNPACK`, `BUILD_MAP_UNPACK`, `BUILD_MAP_UNPACK_WITH_CALL` | Read (D73); loops of `_PyList_Extend`, `_PySet_Update`, `PyDict_Update`, `_PyDict_MergeEx` with error exits that format messages. The hand-written cases call `cimpl_build_unpack`, `cimpl_build_set_unpack` and `cimpl_build_map_unpack` with a virtual tuple of the parts, and build a virtual tuple directly when every part is one (judgement, P22). | Hand-written. |
| `ceval.c:FORMAT_VALUE` | Read (D73): `(value, fmt_spec if ((oparg & FVS_MASK) == FVS_HAVE_SPEC) -- result)`. A `switch` on oparg choosing a function pointer, and a branch on `PyUnicode_CheckExact(value) && fmt_spec == NULL` that isn't only a type test. The hand-written case calls `cimpl_format_value` and keeps an exact str as itself. | Hand-written. Needs the switch on compile-time oparg folded (as `COMPARE_OP` needs). |
| `ceval.c:CALL_FUNCTION_EX` | Refused (D73): with `oparg & 1`, `kwargs = POP()` and then `if (d == NULL) goto error;` when `PyDict_New` fails leaves `kwargs` neither on the stack nor released: a reference leak in CPython 3.7.17's own code, which the error-exit check finds. The effect (`-1 - (oparg & 1)`) would otherwise read. | Refused; calls are core anyway. |
| `ceval.c:CALL_FUNCTION`, `CALL_FUNCTION_KW`, `CALL_METHOD` | Read (D73), their effects from `call_function`'s body. Core by design (GENERATION step 7, D58): the call path, inlining and closures are Ni's own. | Skipped as core by the back half. |
| `ceval.c:BEFORE_ASYNC_WITH` | An error exit with a value it pushed still on the stack. | Refused (coroutines, core in any case). |
| effects: `floatobject.c:float_richcompare` (no Python) | A decref of `ww`, an int made in the function, but declared `PyObject *`: the analysis knows a local's type only from its declaration. | Accepted (F104 is the real error). Candidate: a local assigned only from `PyLong_*` constructors has their type. |
| effects: `float_richcompare` (pure) | Reads `_PyLong_One`, a file-scope variable set once at start-up; the analysis takes every non-`const` one as mutable. | Accepted. Candidate: an `ASSUMPTIONS` list of variables constant after start-up. |
| effects: the pointer `fn` in `pfloatobject.c:pfloat_richcompare` | A call through a local function pointer chosen in a switch from Ni's pure compare helpers. | Unresolved; the flags are right. Candidate: resolve a pointer to every function assigned to it. |
| effects: `cimpl_unicode_eq` (`_PyUnicode_EQ`) | `PyUnicode_READY` can raise and writes the string, but only for a legacy (not ready) string, which Python code never makes in 3.7. | Accepted; the analysis is path-insensitive. |
| effects: `PyErr_ExceptionMatches`, `PyErr_GivenExceptionMatches`, `cimpl_generator_stop` | `PyTuple_Size`'s bad-call error, on a path guarded by `PyTuple_Check`. | Accepted (path-insensitive). |
| effects: `cimpl_push_exc_info` | `PyErr_NormalizeException` may replace the exception; it returns nothing, and `ceval.c` ignores the failure too. | Accepted: there is nothing to check. |
| effects: `cimpl_rt_traceback` | `PyFrame_New` may fail; the helper then leaves the new error in place of the one being unwound, by design (its comment). | Accepted. |
| effects: `funcobject.c:PyFunction_NewWithQualName` (blocks, in D53's list) | `PyUnicode_InternFromString` reaches the UTF-8 decoder's error handler, which can import the codec registry; the name is ASCII, so it never does. | Accepted (path-insensitive). |
| effects: 13 call sites | Flags built at run time (`cf` variables, `CF_UNCHECKED`). | Unproven. |
