
> **Note (later the same day):** the templated header comments this brief asked for were removed at the owner's request and replaced by specific comments on the complex files only (`doc/DECISIONS.md` D9). The review findings below still stand.

# Brief for file reviewers

Repo: /home/tsimpson/work/ni. "Ni" is a fork of Armin Rigo's Psyco JIT (last upstream: git commit 9bd5834, 2010, Python 2.x, i386), partially ported to x86-64 and CPython 3.7. Sources are under ni/. Use British English.

## The goal (why we are reviewing)
Keep Psyco's design (a specialising JIT: values tracked as run-time / compile-time / virtual-time, promotion, merge points and unification, virtual objects materialised only on escape) but make it maintainable by **generating** the hand-written "compile-time CPython" layer with a meta-compiler that reads CPython's own C source. So for every file we want to separate:
- **core**: specialiser internals that must be designed and tidied by hand;
- **translated**: hand-translations of CPython C functions into compile-time form (what a meta-compiler should produce);
- **backend**: x86-64 machine-code emission;
- **glue** / **tooling**.

Another future AI will pick this work up with no guidance except what's in the repo, so be precise, anchor to function names (not line numbers), and mark each observation as **[read]** (seen in code) or **[inferred]**.

Other agents are separately mining git history for removed features — don't do history archaeology beyond a quick `git log --oneline -- <file>` if it helps understand a file.

## Vocabulary (seed — confirm or correct from the code)
- `vinfo_t`: a compiler-time descriptor of a value; `Source` tags it RunTime (register/stack), CompileTime (`source_known_t`, a known constant) or VirtualTime (`source_virtual_t`: not yet built, with a `compute` function to materialise it). `vinfo_array_t` holds sub-fields (e.g. ob_type, ob_size, items).
- `PsycoObject *po`: the compiler state for the code being emitted.
- Promotion: forcing a run-time value to compile-time by emitting a dispatch on its actual value at run time (`Psyco_NeedType`, `psyco_finish_promotion`).
- Residual call: `psyco_generic_call(po, c_fn, Cf* flags, "argfmt", ...)` emits a call to the real C function; `Cf*` flags (cf_flags.h) describe effects/return conventions.
- Meta functions: `Psyco_DefineMeta(c_fn, psyco_fn)` registers a compile-time implementation for a C function / type slot; `Psyco_META1/2/3` dispatch to it if registered, else residual call.
- Field descriptors: `FMUT`, `FPYREF`, `DEF_FIELD`, `psyco_get_field`, `psyco_put_field` describe C struct fields so loads/stores can be emitted or folded.

## Seed translation patterns (provisional names — use these names; propose new ones as `NEW: <name>`)
- `promote-type`: `Psyco_NeedType`/`Psyco_FastType` instead of `Py_TYPE(o)`.
- `verify-type`: `Psyco_VerifyType` tri-state (true/false/error-or-promotion) instead of `PyX_Check`.
- `runtime-branch`: `runtime_condition_t/f`, `integer_cmp` → `condition_code_t` instead of a C `if` on a run-time value.
- `residual-call`: fall back to `psyco_generic_call` of the real C function.
- `meta-dispatch`: `Psyco_META*` through a type slot instead of calling the slot.
- `virtual-object`: build a VirtualTime object with `compute_*` / `direct_compute_*` instead of allocating.
- `field-access`: `psyco_get_field`/`psyco_put_field` with descriptors instead of `o->field`.
- `ct-fold`: if all inputs are compile-time, compute the result in C at compile time.
- `exception-set`: `PycException_*` instead of `PyErr_*`.
- `refcount`: `vinfo_incref/decref` ownership discipline mirrored from Py_INCREF/DECREF.

## What to do for each assigned file
1. Read the whole file (and its header).
2. **Add a header comment** at the very top of each .c and .h file (before the first line; keep any existing comments below it). Exact format:
```
/*
 * <path relative to repo root, e.g. ni/Objects/ptupleobject.c>
 * Role:    <one line>
 * Layer:   <core | translated | backend | glue | tooling> (mixed: list, e.g. "translated + core")
 * Mirrors: <CPython file(s) and key functions it corresponds to, or "none">
 * Notes:   <0-2 lines: the single most important thing to know, e.g. Py2 leftovers, broken parts>
 * See:     doc/MAP.md, doc/PATTERNS.md
 */
```
   Change nothing else in the sources.
3. Write your findings to the report file named in your task, with, per file:
   - **Map row**: `| path | layer | CPython counterpart | role (one line) |`
   - **Sections**: table of the file's function groups → classification (core/translated/backend/glue/obsolete) → one-line note.
   - **Patterns observed**: pattern name → example `file:function` → a 1-3 line CPython-vs-Psyco illustration where useful. Say whether each use is *mechanical* (a meta-compiler could do it from the C source alone) or *judgement* (hand-tuned: heuristics, size limits, choices of what to virtualise/fold, extra knowledge not in the C).
   - **Leftovers**: Py2-isms, 32-bit assumptions (`long` vs `Py_ssize_t`, `int` sizes), dead code, `#if 0`, TODO/XXX.
   - **Glossary terms**: any term/macro a newcomer needs defined, with the file:symbol where it's defined.
   - **Questions**: anything unclear.
   Dense bullet points; no padding.
4. Do not commit. Do not modify files outside your assignment (other agents are editing other files in parallel).
5. Final reply: ≤150 words — headline observations, any new pattern names proposed.
