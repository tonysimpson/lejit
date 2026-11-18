/* A GC object is tracked while its PyGC_Head's _gc_next (16 bytes before
   it) isn't 0. A free-threaded build (Py_GIL_DISABLED) tests a bit
   of ob_gc_bits instead, which this would have to refuse. */
#error "Ni: the inline store's GC test on _gc_next is not written yet (ni/mechanisms/store_gc/gc_next.h)"
