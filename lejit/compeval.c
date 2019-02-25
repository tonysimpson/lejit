#include "compiler.h"


#define UNBOUNDLOCAL_ERROR_MSG \
    "local variable '%.200s' referenced before assignment"


/* This is the compiler, its similar to Python's PyEval_EvalFrameEx but
   rather than running bytecodes it is compiling bytecode into machine
   code and executing it. 
   
   In LeJIT compilation and execution are woven together. Compilation often
   requires the code to run upto a defined point when compilation will 
   continue.
   */
LeEntryPointObject* LeCompEval_Compile(LeCompilerState *s)
{
    LeEntryPointObject *entry_point = NULL;
    PyFrameObject *f;

    /* Copied from ceval.c unless stated. 
       LLTRACE is removed.
       USE_COMPUTED_GOTOS is not supported yet.

       TODO: how do we detect if the c compiler can support 
       computed gotos without using autoconf? */

    LeValue **stack_pointer;  /* Next free slot in value stack */
    const _Py_CODEUNIT *next_instr;
    int opcode;        /* Current opcode */
    int oparg;         /* Current opcode argument, if any */
    LeValue **fastlocals, **freevars;
    PyObject *retval = NULL;            /* Return value */
    PyThreadState *tstate = _PyThreadState_GET();
    PyCodeObject *co;

    const _Py_CODEUNIT *first_instr;
    LeValue **names;
    LeValue **consts;


#define TARGET(op) op
#define DISPATCH() continue
#define FAST_DISPATCH goto fast_next_opcode

#define GETITEM(v, i) PyTuple_GET_ITEM((PyTupleObject *)(v), (i))
#define INSTR_OFFSET()  \
    (sizeof(_Py_CODEUNIT) * (int)(next_instr - first_instr))
#define NEXTOPARG()  do { \
        _Py_CODEUNIT word = *next_instr; \
        opcode = _Py_OPCODE(word); \
        oparg = _Py_OPARG(word); \
        next_instr++; \
    } while (0)
/* Both the jumps differ from ceval as they need to find and join
   the jump detinations in compiled code */
#define JUMPTO(x)       (next_instr = first_instr + (x) / sizeof(_Py_CODEUNIT))
#define JUMPBY(x)       (next_instr += (x) / sizeof(_Py_CODEUNIT))
#define PREDICT(op) \
    do{ \
        _Py_CODEUNIT word = *next_instr; \
        opcode = _Py_OPCODE(word); \
        if (opcode == op){ \
            oparg = _Py_OPARG(word); \
            next_instr++; \
            goto PRED_##op; \
        } \
    } while(0)

#define PREDICTED(op) PRED_##op:

/* Stack manipulation macros */
#define STACK_LEVEL()     ((int)(stack_pointer - c->c_valuestack))
#define EMPTY()           (STACK_LEVEL() == 0)
#define TOP()             (stack_pointer[-1])
#define SECOND()          (stack_pointer[-2])
#define THIRD()           (stack_pointer[-3])
#define FOURTH()          (stack_pointer[-4])
#define PEEK(n)           (stack_pointer[-(n)])
#define SET_TOP(v)        (stack_pointer[-1] = (v))
#define SET_SECOND(v)     (stack_pointer[-2] = (v))
#define SET_THIRD(v)      (stack_pointer[-3] = (v))
#define SET_FOURTH(v)     (stack_pointer[-4] = (v))
#define SET_VALUE(n, v)   (stack_pointer[-(n)] = (v))
#define PUSH(v)           (*stack_pointer++ = (v))
#define POP()             (*--stack_pointer)
#define STACK_GROW(n)     (stack_pointer += n)
#define STACK_SHRINK(n)   (stack_pointer -= n)
#define EXT_POP(STACK_POINTER) (*--(STACK_POINTER))

/* Local variable macros */
#define GETLOCAL(i)       (fastlocals[i])

#define SETLOCAL(i, value)      do { LeValue *tmp = GETLOCAL(i); \
                                     GETLOCAL(i) = value; \
                                     Le_XDECREF(s, tmp); } while (0)

#define UNWIND_BLOCK(b) \
    while (STACK_LEVEL() > (b)->b_level) { \
        PyObject *v = POP(); \
        Py_XDECREF(v); \
    }

#define UNWIND_EXCEPT_HANDLER(b) \
    do { \
        PyObject *type, *value, *traceback; \
        _PyErr_StackItem *exc_info; \
        assert(STACK_LEVEL() >= (b)->b_level + 3); \
        while (STACK_LEVEL() > (b)->b_level + 3) { \
            value = POP(); \
            Py_XDECREF(value); \
        } \
        exc_info = tstate->exc_info; \
        type = exc_info->exc_type; \
        value = exc_info->exc_value; \
        traceback = exc_info->exc_traceback; \
        exc_info->exc_type = POP(); \
        exc_info->exc_value = POP(); \
        exc_info->exc_traceback = POP(); \
        Py_XDECREF(type); \
        Py_XDECREF(value); \
        Py_XDECREF(traceback); \
    } while(0)


    if ((entry_point = le_compiler_merge_or_modify_or_continue(s)) != NULL) {
        return entry_point;
    }
    else {
        entry_point = le_compiler_entry_point_new(s);
    }
    /* Start of code */
    /* push frame */
#if 0
    /* TODO: what do? */
    if (Py_EnterRecursiveCall(""))
        return NULL;
#endif
    
    f = le_compiler_fake_frame_make(s);
    tstate->frame = f;
    co = c->c_code;
    names = c->c_names;
    consts = c->c_consts;
    fastlocals = c->c_localsplus;
    freevars = c->c_localsplus + co->co_nlocals;

    first_instr = (_Py_CODEUNIT *) PyBytes_AS_STRING(co->co_code);

    next_instr = first_instr;
    if (c->c_lasti >= 0) {
        next_instr += c->c_lasti / sizeof(_Py_CODEUNIT) + 1;
    }
    stack_pointer = c->c_stacktop;
    assert(stack_pointer != NULL);
    c->c_stacktop = NULL;       /* remains NULL unless compilation suspends */
    f->f_executing = 1;


main_loop:
    for(;;) {
    fast_next_opcode:
        c->c_lasti = f->f_lasti = INSTR_OFFSET();
        NEXTOPARG();
    dispatch_opcode:
        switch (opcode) {

        case TARGET(NOP): {
            FAST_DISPATCH();
        }

        case TARGET(LOAD_FAST): {
            LeValue *value = GETLOCAL(oparg);
            if (value == NULL) {
                format_exc_check_arg(PyExc_UnboundLocalError,
                                     UNBOUNDLOCAL_ERROR_MSG,
                                     PyTuple_GetItem(co->co_varnames, oparg));
                goto error;
            }
            Le_INCREF(value);
            PUSH(value);
            FAST_DISPATCH();
        }

        case TARGET(LOAD_CONST): {
            PREDICTED(LOAD_CONST);
            LeValue *value = GETITEM(consts, oparg);
            Le_INCREF(value);
            PUSH(value);
            FAST_DISPATCH();
        }

        case TARGET(STORE_FAST): {
            PREDICTED(STORE_FAST);
            LeValue *value = POP();
            SETLOCAL(oparg, value);
            FAST_DISPATCH();
        }

        case TARGET(RETURN_VALUE): {
            retval = POP();
            assert(f->f_iblock == 0);
            goto return_or_yield;
        }


#if USE_COMPUTED_GOTOS
        _unknown_opcode:
#endif
        default:
            le_compiler_call_direct(s, LE_CALL_RETURNS_NO_VALUE, fprintf, "isii", stderr, "XXX lineno: %d, opcode: %d\n", PyFrame_GetLineNumber(f), opcode);
            PyErr_SetString(PyExc_SystemError, "unknown opcode");
            goto error;
        } /* switch */

error:
        if (!PyErr_Occurred() || )
            PyErr_SetString(PyExc_SystemError,
                            "error return without exception set");
        LeErr_TransferExceptionAndCreateTraceBack(s);

exception_unwind:
        /* TODO: add block stack unwind */
        break;
    } /* main loop */

    /* Pop remaining stack entries. */
    while (!EMPTY()) {
        LeValue *o = POP();
        Le_XDECREF(s, o);
    }

    assert(retval == NULL);
    assert(PyErr_Occurred());
return_or_yield:
exit_eval_frame:
    /* Py_LeaveRecursiveCall(); TODO: what do? */
    f->f_executing = 0;
    /* Remove the fake frame */
    tstate->frame = f->f_back;
    Py_XDECREF(f);
    /* Compile return */
    le_compiler_return(s, _Le_CheckFunctionResult(s, NULL, retval, "PyEval_EvalFrameEx"));
    return entry_point;
}


