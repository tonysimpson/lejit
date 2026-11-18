#undef _Py_atomic_load_relaxed
#undef _Py_atomic_store_relaxed
#undef _Py_atomic_load_explicit
#undef _Py_atomic_store_explicit
void *_Py_atomic_load_relaxed(void *);
void _Py_atomic_store_relaxed(void *, ...);
void *_Py_atomic_load_explicit(void *, int);
void _Py_atomic_store_explicit(void *, ...);
