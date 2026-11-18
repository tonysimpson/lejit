/* Entry by per-function vectorcall (D75): PyFunction_SetVectorcall on each
   function Ni compiles, a function watcher (PyFunction_AddWatcher) to undo
   it when __code__, __defaults__ and the like change, and
   _PyFunction_Vectorcall for anything Ni doesn't run (PORT312.md, the
   prototype in tools/proto312/). Needs ni_entry_point and a frameless
   NiCode_Run taking the argument vector. */
#error "Ni: entry by per-function vectorcall (PyFunction_SetVectorcall and function watchers) is not written yet (ni/mechanisms/entry/vectorcall.h; D75)"
