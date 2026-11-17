---
written: 2026-11-17
status: design note; the hook change (`21ae5a3`) and option B are done
---
# Generators: design note

Generator, coroutine and async-generator code is never compiled (`mergepoints.c:_build_merge_points` rejects `CO_GENERATOR | CO_COROUTINE`; history B7). Upstream Psyco never compiled them either. [read] Compiled code *consumes* generators as ordinary run-time iterators: `FOR_ITER` on a generator is a call to `tp_iternext`. [inferred]

## Where the time goes today

- **The hook on every resume.** `gen_send_ex` calls `PyEval_EvalFrameEx` on each `next()`, so every resume passes through `eval_hook.c:NiEval_EvalFrame`. Before `21ae5a3` the hook looked up the code's merge points first. It now leaves at once for generator flags. Result [run], release build:
  - pyperformance generators 0.88× → 0.99×, coroutines 0.96× → 1.01×;
  - bench.py generators 0.88× → 1.1×;
  - a microbenchmark (2 million items): genexpr under `sum()` 0.85× → 1.0×; a compiled `for` loop over a plain generator 1.27×.
  
  Nothing more is available here: the remaining cost is PEP 523's extra indirect call.
- **bm_generators** is a recursive tree walk (`yield from self.left`). Each value passes through one `YIELD_FROM` per tree level, all inside CPython's `gen_send_ex`. Compiling the bodies would save little, because the bodies do almost nothing. [read] (`tools/pyperformance/bm_generators.py`), [inferred]
- **Generator expressions consumed by builtins** (`sum(x*x for x in xs)`, `set(...)`, `any`, `all`, `tuple`, `str.join`) are common in real code (nqueens, mdp). Each item costs a resume and a yield in the interpreter, plus a call from the builtin. [read], [inferred]

## Options

### A. Resumable compiled bodies over the CPython frame

Compile a generator's code with one entry per resume point, and keep the generator's state in its CPython frame at every yield. This follows 3.7's own protocol [read] (`ceval.c` 902–911, `YIELD_VALUE`, `YIELD_FROM`; `genobject.c:gen_send_ex`):

- **Resume.** The frame arrives with `f_lasti` at the last `YIELD_*`, the value stack up to `f_stacktop` (the sent value already pushed by `gen_send_ex`), and the block stack in `f_blockstack`/`f_iblock`. The entry is specialised on `f_lasti`, a small set known from the bytecode. The block stack at a given offset is static, so it is a compile-time fact of that entry.
- **Ownership.** At entry the compiled code takes over the frame's locals and stack items as owned run-time values, and sets `f_stacktop = NULL` as ceval does ("running"). At a yield it writes every live local and stack item back with its reference, sets `f_stacktop`, `f_lasti` and `f_iblock`, and returns the value. There is never more than one owner, and the interpreter can take over at any yield, so `throw()`, `close()`, tracing and `gi_frame` inspection keep working. A `throwflag` entry simply goes to the interpreter.
- **`YIELD_FROM`.** Do what ceval does: send into the delegate (`_PyGen_Send` or `tp_iternext`). If it yields, write the state back with `f_lasti` on the instruction before, so the next resume runs `YIELD_FROM` again.
- **Return.** Release everything and leave `f_stacktop` NULL. `gen_send_ex` turns the result into `StopIteration`.

Cost per yield: write-back of the live state, boxing any virtual values, plus the glue of `psyco_processor_run` on each resume. So **A pays only when a body does real work between yields**. bm_generators doesn't. Work: about 1–2 days, mostly `PsycoObject_FromCode` with a start offset and frame-sourced vlocals, and the yield write-back in `pycompiler.c`. [inferred]

**On 3.12** the vectorcall entry (PORT312.md) never sees resumes: `gen_send_ex` goes straight to `_PyEval_EvalFrame`. A needs the PEP 523 hook there just for generator frames, and there the hook receives an internal `_PyInterpreterFrame`. This is the main argument against investing in A on 3.7 first. [read] (PORT312.md, Entering compiled code), [inferred]

### B. Inline a generator expression into a builtin consumer

At `CALL_FUNCTION` in compiled code, when the callee is a known reduction (`sum`, `any`, `all`, `min`, `max`, `list`, `tuple`, `set`, `sorted`, `str.join`) and the single argument comes straight from `MAKE_FUNCTION` and an immediate call of a genexpr code object, compile the genexpr's bytecode inline:

- its `YIELD_VALUE` becomes the reduction step (add, test-and-exit, append), and its `RETURN_VALUE` becomes the result;
- the generator object, its frame and every resume disappear;
- the loop runs in registers, like a compiled listcomp, with virtual ints and the virtual range.

This is the same shape as inlining a listcomp's code (history A4: the machinery exists but rarely fires). It works unchanged on 3.12, because it happens at a call site in compiled code. Limits:
- only a genexpr consumed at once by a known builtin;
- the builtin must still be the real one (cached LOAD_GLOBAL guards it, D25);
- a genexpr that escapes (stored, passed on, partly consumed) keeps today's path;
- exceptions inside it need the right traceback entry (`<genexpr>`), which frames.c's emulation can give.

**Done** (2026-11-17), as a separate compiled callee rather than inlined, which needs no change to inlining:
- MAKE_FUNCTION plus the call of a genexpr gives a *pending generator* (`pfuncobject.c:PsycoGenexpr_New`): a virtual object whose compute makes the real call. Delaying it is safe because creating a generator runs no code.
- `sum`, `any`, `all` (`pbltinmodule.c` metas) and `list`, `tuple`, `set` (`ptype_call`) run a pending one through `psyfunc.c:psyco_call_genexpr`. That compiles the genexpr's code with two hidden slots after its free variables: the mode, a compile-time value so that versions never mix at merge points, and the accumulator. YIELD_VALUE does the step (append, add, `+`, or test and return), RETURN_VALUE returns the result (`pycheader.h` GENEXPR_*).
- **Only a temporary is fused** (vinfo refcount ≤ 2: the stack and the argument tuple; a dead local is already released). A generator that something can still use must stay one object: after a partial run (`any`) or an exception it keeps its position, inner loops included. Anything else, such as `next(g)`, passing it on, or a live local, makes the real generator.
- PEP 479: StopIteration leaving the fused code becomes RuntimeError, chained as `gen_send_ex` does (`pycompiler.c:cimpl_generator_stop`).
- Tests: `tests/cases/genexprs.py`.
- Results [run], release build: 40,000 calls over 50 items, `sum` 4.3×, `any` 2.9×, `tuple` 2.6×, `set` 2.6× against CPython. pyperformance comprehensions 1.40× → 1.81×, hexiom 1.67× → 1.98×, mdp 1.11× → 1.20×; bench.py generators 1.1× → 2.1×. nqueens is unchanged (1.00×): its genexprs are inside generator functions, which still run interpreted (option A).
- Not done: `min`/`max` (their key and default arguments), `str.join`, `sorted`, `dict`.

### C. Fuse a generator with a compiled `for` loop

`for x in gen(...)` in compiled code, with `gen` a known generator function: compile the consumer's loop body at each `YIELD_VALUE` of the inlined generator. This is the general form of B. It needs the compiler to interleave two code objects' bytecode (two block stacks, two `next_instr`), which Psyco's single-code `pyc_data_t` doesn't support. Large, and close to the tracing/PyPy territory GOAL.md rules out. Not proposed.

## Recommendation

1. Done: the hook exit (`21ae5a3`).
2. Done: **B**.
3. **A** only if a benchmark shows generator bodies doing real work between yields. nqueens does (`permutations` and `n_queens` are generator functions holding the genexprs B would fuse). Decide it together with the 3.12 entry design (QUESTIONS 33).
