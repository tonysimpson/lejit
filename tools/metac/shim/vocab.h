/* The vocabulary shim (doc/GENERATOR_DESIGN.md "Front end"). front.py
   includes it again after every #include of the .c file being read, and
   after the file's own #define or #undef of a name here, so that each name
   is a call to its marker __ni_vocab_<name> wherever the file uses it.
   front.py reads the names from the #undef lines. The atomics expand to
   GNU statement expressions; the decrefs would lose their argument's type
   to a cast (effects.py). */
#ifndef NI_VOCAB_MARKERS
#define NI_VOCAB_MARKERS
void *__ni_vocab__Py_atomic_load();
void *__ni_vocab__Py_atomic_load_relaxed();
void *__ni_vocab__Py_atomic_load_explicit();
void __ni_vocab__Py_atomic_store();
void __ni_vocab__Py_atomic_store_relaxed();
void __ni_vocab__Py_atomic_store_explicit();
void __ni_vocab__Py_atomic_signal_fence();
void __ni_vocab__Py_atomic_thread_fence();
void __ni_vocab_Py_DECREF();
void __ni_vocab_Py_XDECREF();
void __ni_vocab_Py_CLEAR();
void __ni_vocab_Py_SETREF();
void __ni_vocab_Py_XSETREF();
#endif

#undef _Py_atomic_load
#undef _Py_atomic_load_relaxed
#undef _Py_atomic_load_explicit
#undef _Py_atomic_store
#undef _Py_atomic_store_relaxed
#undef _Py_atomic_store_explicit
#undef _Py_atomic_signal_fence
#undef _Py_atomic_thread_fence
#undef Py_DECREF
#undef Py_XDECREF
#undef Py_CLEAR
#undef Py_SETREF
#undef Py_XSETREF
#define _Py_atomic_load(...) __ni_vocab__Py_atomic_load(__VA_ARGS__)
#define _Py_atomic_load_relaxed(...) __ni_vocab__Py_atomic_load_relaxed(__VA_ARGS__)
#define _Py_atomic_load_explicit(...) __ni_vocab__Py_atomic_load_explicit(__VA_ARGS__)
#define _Py_atomic_store(...) __ni_vocab__Py_atomic_store(__VA_ARGS__)
#define _Py_atomic_store_relaxed(...) __ni_vocab__Py_atomic_store_relaxed(__VA_ARGS__)
#define _Py_atomic_store_explicit(...) __ni_vocab__Py_atomic_store_explicit(__VA_ARGS__)
#define _Py_atomic_signal_fence(...) __ni_vocab__Py_atomic_signal_fence(__VA_ARGS__)
#define _Py_atomic_thread_fence(...) __ni_vocab__Py_atomic_thread_fence(__VA_ARGS__)
#define Py_DECREF(...) __ni_vocab_Py_DECREF(__VA_ARGS__)
#define Py_XDECREF(...) __ni_vocab_Py_XDECREF(__VA_ARGS__)
#define Py_CLEAR(...) __ni_vocab_Py_CLEAR(__VA_ARGS__)
#define Py_SETREF(...) __ni_vocab_Py_SETREF(__VA_ARGS__)
#define Py_XSETREF(...) __ni_vocab_Py_XSETREF(__VA_ARGS__)
