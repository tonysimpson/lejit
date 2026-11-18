# Generator design

The design for the meta-compiler that generates Ni's compile-time CPython layer. It replaces today's `tools/metac/` (about 20,000 lines, grown one named rule at a time) with one small pipeline. Status: **proposed**, for the owner to mark up.

## What it does

For each CPython C function Ni mirrors, it writes the function's **generating extension**: the C that runs inside Ni's compiler and emits machine code for that function, given what is known at compile time. This is Psyco's meta function, derived from the C instead of written by hand. The theory is partial evaluation (Jones, Gomard, Sestoft, *Partial Evaluation and Automatic Program Generation*, 1993): a binding-time analysis, then specialisation.

Constraints, unchanged from GOAL and DECISIONS:
- **One Ni source for every CPython from 3.7 on** (D67). No `#if` on versions in Ni; the target version is only ever an input.
- **Generated at build time, not checked in** (D68).
- **The input is CPython's C source.** LLVM only as an assembler for templates (D65).
- **Breaks loudly.** A construct the generator doesn't understand is refused, with the C location and the reason. It never guesses.

## Pipeline

```
CPython source ──gcc -E + shim──▶ C ──pycparser──▶ AST ──types──▶ typed AST
      │                                                                  │
   rules (Python)                                    binding-time analysis
      │                                                                  │
      └──────────────────────▶ specialiser ──▶ C builder ──▶ *_gen.h + report
```

1. **Source.** As now: a tarball by version, checked against `cpython_sha256.txt`.
2. **Preprocess with `gcc -E`**, using the target's include paths and `pyconfig.h`, and a **shim header** included first. The shim declares each vocabulary name (`Py_INCREF`, `PyFloat_Check`, `Py_RETURN_NONE`, …) as a function, and `#undef`s CPython's macro of the same name after the headers are read, so those names survive as calls. Line markers are kept so every message can name the CPython file and line. `#if`, `#include` and every other macro are resolved by the real preprocessor, as CPython's own build does.
3. **Parse with pycparser.** **Type** the AST from its declarations: each expression's C type, each struct's fields, each typedef. Nothing else in the pipeline reads tokens or text.
4. **Binding-time analysis**, once, for every function, in one module (below).
5. **Specialise**: walk the annotated AST and decide, per construct, one of a small set of actions (below).
6. **Emit** through a small C builder: expressions and statements as objects, printed once, deterministically.
7. **Report**: per function, generated or refused, and why. `donecheck.py` reads this report, not the generators' internals.

There is **one translator**. A slot, a builtin, an `abstract.c` function, an opcode case and argument binding are all "a C body, its parameters and their binding times". Only the entry point differs:
- a function: its parameters;
- an opcode case: the value stack as an array parameter, `oparg` as a compile-time int, and `ceval.c`'s locals as the frame's;
- argument binding: the part of `_PyEval_EvalCodeWithName` between making the frame and running it.

## Binding times

Every C value gets one binding time:

| Time | Meaning | Ni form |
|---|---|---|
| `known` | Its value is known while compiling | a C value in the compiler |
| `virtual` | An object Ni has not allocated: its fields are vinfos | a VirtualTime vinfo |
| `runtime` | Only known when the code runs | a RunTime vinfo |

The rules:
- **Inputs.** Parameters take their times from the entry: the opcode's stack values are run-time or virtual, `oparg` is known, `op` in `tp_richcompare` is known.
- **Expressions.** An operation is `known` when its inputs are `known` and it is pure (from the effect analysis). Otherwise it is `runtime`.
- **Control.** A test on a `known` value picks its branch. A test on a `runtime` value is a run-time branch, both arms specialised.
- **Types.** Reading the type of a run-time parameter promotes it (Ni's `Psyco_NeedType`), so the type is `known` from there on. For any other run-time value with no known type, CPython's code runs from that point.
- **Loops.** A loop whose bound is `known` is unrolled; otherwise it runs at run time (D78).
- **Fixed point.** The analysis iterates to a fixed point over gotos and loops. Liveness is computed on the same graph.
- **Ownership.** Reference ownership (new, borrowed, stolen) is part of each value's annotation, from the vocabulary's signatures, so increfs and decrefs come out right without a rule each.

Most of today's named rules are cases of this table: `if_known`, `bounds_test`, `nested_scalar_call`, `type_test_generic`, switch folding, `fall_through`, goto liveness and the return-type rules.

## The specialiser's actions

Each construct gets exactly one of these, each in one place in the code:

| Action | When | Emits |
|---|---|---|
| fold | all inputs `known`, operation pure | nothing at run time; a C value |
| map | the call is in the vocabulary or a type's `make` | the Ni function the rule names |
| residual call | a run-time call with effect flags | `psyco_generic_call(...)` with the call's flags |
| scalar helper | a maximal run of statements on C scalars only | one pure C helper, called as a meta (D87) |
| inline | a static helper of the same file | its body, specialised in place |
| resume | anything else that can't be specialised | a run-time call of CPython's own code from this point |
| refuse | a construct with no rule | nothing; reported with location and reason |

`resume` is what makes the generator safe: when in doubt, the run-time code is CPython's own, so behaviour is CPython's. Specialising more is a performance question, never a correctness one.

## Rules (Python, with C inside)

Three files, plain Python. Every C string is parsed by the same pycparser and checked on load. Every Ni name must exist in `ni/`. Every CPython name and condition must exist in the target's source.

**`vocabulary.py`**: what a CPython idiom means to Ni, as the Ni function that does it, with its ownership signature.

```python
VOCABULARY = {
    'Py_INCREF(x)':             'psyco_incref_v(x)',
    'Py_DECREF(x)':             'psyco_decref_v(x)',
    'Py_TYPE(x)':               'Psyco_NeedType(x)',
    'Py_RETURN_NOTIMPLEMENTED': 'return psyco_vi_NotImplemented()',
}
```

**`types.py`**: how Ni represents each CPython type, as Ni's own functions. `when` is CPython's own test, as it appears in the source.

```python
TYPES = [
    Type('PyLongObject',
         value='PsycoLong_MediumValue(x)', when='-1 <= Py_SIZE(x) && Py_SIZE(x) <= 1',
         make={'PyLong_FromLong(n)': 'PsycoLong_FROM_WORD(n)'}),
    Type('PyFloatObject',
         value='PsycoFloat_AS_DOUBLE(x)',
         make={'PyFloat_FromDouble(d)': 'PsycoFloat_FROM_DOUBLE(d)'}),
]
```

**`policy.py`**: the judgement calls, the only places where the generator does something other than follow the C. Each entry names a CPython function and keeps its shape hash, so a change in the C stops generation until someone re-reads the entry.

```python
POLICY = {
    'float_richcompare': [specialise('w', 'PyFloat_Type')],
    'tuplerepeat':       [specialise('n', known=True)],
    'PyObject_Hash':     [likely(False, 'tp->tp_hash == NULL')],
}
```

`specialise`, `likely`, `fold` and so on are a handful of small dataclasses, checked when the file loads. A note for people is a Python comment, never a string the tool reads.

## Other generators

These stay separate, because they produce different things, but they share the front end, the rules loader and the C builder:
- **Layouts** (D76): `static_assert`s on struct offsets and sizes, from the typed AST.
- **Mechanisms** (D75): one detector per mechanism over the typed AST, choosing a hand-written snippet in `ni/mechanisms/`.
- **Effects** (D71): a call-graph pass over the typed AST, giving each function's flags (`CfPure`, `CfNoPython`, `CfNoBlock`). It feeds the binding-time analysis.
- **Virtual types** (D80): field descriptors and constructors, from `types.py` and the structs.

## Errors and the done check

- **A refusal** names the CPython file and line, the construct, and the missing rule. The report lists every function as generated, resumed in part (with where), or refused.
- **The done check** compares the report with Ni's hand-written mirrors. A hand-written function needs an entry in `hand_table.py` (core, with why), or a refusal in the report.

## Testing

- **Per pass, on small C snippets with expected output**: the preprocessor shim, typing, the binding-time table, each specialiser action. Seconds to run, and each rule's behaviour is visible.
- **End to end**, as now: generation deterministic, `tests/run.py`, `cpython_tests.py`, `samecode.py` and release instructions.

## Migration

1. **Build the new pipeline beside the old.** The old generator is the oracle.
2. **Move one family at a time:** abstract, numbers, containers, builtins, opcodes, binding. Each must give the same headers, or the same machine code by `samecode.py`, with every difference given a reason.
3. **Delete each family's old code as it moves.** When all have moved, `gen_abstract.py`, `gen_ceval.py`, `cparse.py`, `conditions.py`, `pointers.py`, `scalars.py` and the macro expander are gone.
4. **Then extend.** `float_richcompare` and the other `type_whitelist` functions should come out of one `specialise` policy line each.

## Open decisions (owner)

1. **pycparser plus `gcc -E`, or libclang?** libclang gives types and macro locations for free, but is LLVM, which D65 limits to assembly. Proposed: pycparser.
2. **Is a typed, annotated AST inside the generator a "compiler IR"** in GOAL's sense? Proposed reading: no. GOAL rules out an IR as the input or as a stage of Ni's code generation; this is the generator's own view of CPython's C, and Ni's output is unchanged.
3. **Shim maintenance.** The vocabulary names in the shim are the generator's contract with CPython's headers. When a new CPython changes a macro into a static inline function, the shim must still capture the name. The detector should fail loudly when a vocabulary name doesn't survive preprocessing.
