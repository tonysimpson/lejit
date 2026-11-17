---
written: 2026-11-17
status: running journal; newest entries last
---
# Pointbreak journal

Pain points and wishes from debugging Ni with pointbreak, for pointbreak's
own development (`~/work/pointbreak`, maintained separately: nothing here
changes it). The installed copy is pointbreak `04e59cd` (2019), in a venv
outside both repos (doc/BUILD.md, Debug). Each entry says what I was trying
to do, what got in the way, and what would have helped.

## 2026-11-17: segfault in `a85encode` (test_base64 under Ni)

Goal: find why compiled `base64.a85encode` jumps to a heap address after an
exception. Tool written: `tools/nianalyser/crash_report.py`.

What worked well:
- Hooking Ni's code generator (`ni_trace_begin_code`/`end_code` via
  nianalyser) gives an interval map of every emitted chunk with its Python
  location and compiler call stack. Annotating crash addresses with it is
  exactly the right idea.
- `single_step()` from a late breakpoint (the last `psyco_processor_run`
  before the crash) recorded the last 4000 instructions in about 7 s, and
  showed the faulting `RET 0x40` and the respawn before it. That found in
  minutes what gdb hadn't.

Pain points:
1. **Fatal signals.** I had to find out by reading `debugger.py` that a
   SIGSEGV arrives as a `STOP` event with a `signal` attribute (and that
   `next_event` returns it rather than raising). A documented event, or a
   callback (`db.on_signal(SIGSEGV, fn)`), would help. `siginfo` (`si_addr`,
   `si_code`, from `PTRACE_GETSIGINFO`) is missing: it tells "jumped to a bad
   address" from "read through a bad pointer".
2. **`address_to_description` only knows exact symbols.** Everything else is
   `????@addr`: return addresses inside `ni.so`, anything in anonymous
   mappings. Wanted: nearest symbol + offset, else mapping name, offset and
   permissions. I used the private `pointbreak.proc_maps._maps(db._pid)`;
   a public `db.mappings()` / `db.mapping_for(addr)` would do.
3. **`single_step()` returns a list of events** and the caller has to look
   for fatal signals and exit in it. A `step_until(predicate)` or a
   ring-buffer recorder (`db.record_steps(n)`) would make "what ran just
   before the crash" a one-liner; every crash tool will want it.
4. **Two runs to step from the right place.** The first run counts
   `psyco_processor_run` hits, the second steps from the last one. Fork-based
   checkpoints (rewind to the last entry) would remove the second run.
5. **Tracing cost.** nianalyser stops twice per emitted chunk (13,648 chunks
   here), a ptrace round trip each. Breakpoints that copy registers or memory
   into a buffer and continue, read later in one go, would make code-map
   tracing much cheaper.
6. **Watchpoints**: you mentioned pointbreak has them; the copy installed
   here (`04e59cd`) doesn't, and `NOTES.md` lists them as planned (ptrace
   debug registers; QEMU user mode for unlimited ones). The bug looked like
   a corrupted return path, where a watchpoint on the stack slot would have
   named the writer directly. To check: is there a newer pointbreak to
   install?
7. **Disassembly** is done by nianalyser with distorm3; a built-in
   `db.disassemble(addr, n)` with the symbolisation of item 2 would save
   every tool reimplementing it.
8. **Unwinding through generated code.** `backtrace()` stops at the first
   JIT frame. Your design (hook the code generator, record unwind info from
   a script) fits; what's missing is the other half: a way for a script to
   register "code range → unwind rule" (e.g. frame size, or "return address
   at rsp+N") that `backtrace()` then uses.

Outcome: fixed (F31, `e657847`). Cutting the crash down took some
sixty runs of hand-made variants; pointbreak was used for the two traces
that mattered. Further notes from the reduction:

9. **A struct reader was good.** `db.reference(addr, nianalyser.PsycoObject)`
   gave the compiler's state at a breakpoint, cheaply.
10. **Breakpoints in `ni.so` seemed to miss hits.** A script with
    breakpoints on `function_return`, `do_respawn` and
    `psyco_respawn_detected` logged three hits in a run that must have
    compiled many functions. I didn't find out why (set too late, after
    the library loaded? A rearm problem after the first hit?). A way to
    list the breakpoints and their hit counts would have settled it.
11. **The build mattered more than the tool.** The crash disappeared in an
    `NI_ALL_CHECKS=1` build, and a release build without `NI_TRACE` gave
    zero recorded chunks. `crash_report.py` now says so; nothing for
    pointbreak here.

## 2026-11-17: a failed guard landing in the wrong respawned code (A9 branch)

- **Trying to:** find why a failed `tp_version_tag` guard jumped into code already compiled for another respawn point.
- **Worked:** `crash_report.py --step-from` showed the proxy, then `JMP RDX` into the already-emitted exception chunk. That was enough to see that no new compile had happened.
- **In the way:** nothing could tell me *who last wrote* the guard's `Jcc` target (`change_cond_jump_target`) or the proxy's return value. I fell back to `fprintf` in the compiler.
- **Would have helped:** a write watchpoint on an address in emitted code, reporting the writer's backtrace. Also a cheap way to break when a C function returns, and print its return value (`do_respawn`'s code address).
