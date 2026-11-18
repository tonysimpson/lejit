/* Entry by the PEP 523 hook given a _PyInterpreterFrame, where specialised
   calls would bypass a replaced vectorcall (D75). The hook must clear and
   pop the frame it evaluates, with functions an extension can't call
   (PORT312.md "Entering compiled code"). */
#error "Ni: entry through a frame-evaluation hook given a _PyInterpreterFrame is not written yet (ni/mechanisms/entry/frame_eval_hook_interp.h; D75)"
