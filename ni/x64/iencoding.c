/*
 * Backend functions:
 *  - proxy stubs (psyco_call_code_builder);
 *  - C calls at run time (run_time_call: SysV registers and 16-byte alignment,
 *    then run_time_result_check applies the CfPyErr* convention);
 *  - C calls at compile time (compile_time_call: libffi, for CfPure folding);
 *  - Psyco-to-Psyco calls (psyco_call_psyco: the caller pushes a finfo slot and
 *    the arguments, the callee pops them, and the result is a new reference in
 *    RAX);
 *  - condition-code materialisation and the integer ALU emitters.
 */

#include "iencoding.h"
#include <ffi.h>
#include "../Python/frames.h"
#include "../Python/pycompiler.h"
#include "../cf_flags.h"
#include "../codegen.h"
#include "../codemanager.h"
#include "../dispatcher.h"
#include "../vcompiler.h"
#include "ipyencoding.h"

DEFINEVAR reg_t RegistersLoop[REG_TOTAL] = {
    /* RAX > */ REG_NONE,
    /* RCX > */ REG_NONE,
    /* RDX > */ REG_NONE,
    /* RBX > */ RBP_IS_RESERVED ? REG_LOOP_START : REG_X64_RBP,
    /* RSP > */ REG_NONE,
    /* RBP > */ RBP_IS_RESERVED ? REG_NONE : REG_LOOP_START,
    /* RSI > */ REG_NONE,
    /* RDI > */ REG_NONE,
    /* R8  > */ REG_NONE,
    /* R9  > */ REG_NONE,
    /* R10 > */ REG_NONE,
    /* R11 > */ REG_NONE,
    /* R12 > */ REG_NONE,
    /* R13 > */ REG_X64_R14,
    /* R14 > */ REG_X64_R15,
    /* R15 > */ REG_X64_RBX};

DEFINEFN void *
psyco_call_code_builder(PsycoObject *po, void *fn, int restore,
                        RunTimeSource extraarg, size_t block_size)
{
    code_t *block_start;
    int mark;

    BEGIN_CODE
    BEGIN_SHORT_JUMP(0);
    block_start = (code_t *)(((long)code + 7) & (~7)); /* alignment */
    code = block_start;
    code += block_size;
    END_SHORT_JUMP(0);
    TEMP_PUSH_R(REG_FUNCTIONS_RETURN, mark);
    /* RAX is saved: we can be here from run_time_result_check, before the
       result has a tracked location. Other registers need no saving (only
       callee-saved ones are allocated). The flags are not saved, whatever
       'restore' says: promotion spills them first (NEED_CC) and
       psyco_coding_pause asserts none is left (KNOWN_BUGS B8). */
    BEGIN_CALL(extraarg != SOURCE_DUMMY ? 2 : 1);
    if (extraarg != SOURCE_DUMMY) {
        CALL_SET_ARG_FROM_RT(extraarg, 1);
    }
    CALL_SET_ARG_IMMED(block_start, 0);
    END_CALL_I(fn);
    MOV_R_R(REG_TRANSIENT_1, REG_FUNCTIONS_RETURN);
    TEMP_POP_R(REG_FUNCTIONS_RETURN, mark); /* Restore RAX */
    JMP_R(REG_TRANSIENT_1);
    END_CODE
    return (void *)block_start;
}

static vinfo_t *
compile_time_check(PsycoObject *po, long result, int flags)
{
    switch (flags & CfPyErrMask) {
        case CfPyErrDontCheck:
            break;
        case CfPyErrIfNull:
            if (result == 0) {
                goto error;
            }
            break;
        case CfPyErrIfNonNull:
            if (result != 0) {
                goto error;
            }
            break;
        case CfPyErrIfNeg:
            if (result < 0) {
                goto error;
            }
            break;
        case CfPyErrIfMinus1:
            if (result == -1) {
                goto error;
            }
            break;
        case CfPyErrCheck:
            if (PyErr_Occurred()) {
                goto error;
            }
            break;
        case CfPyErrCheckMinus1:
            if (result == -1 && PyErr_Occurred()) {
                goto error;
            }
            break;
        case CfPyErrCheckNeg:
            if (result < 0 && PyErr_Occurred()) {
                goto error;
            }
            break;
        case CfPyErrNotImplemented:
            if ((PyObject *)result == Py_NotImplemented) {
                Py_DECREF(Py_NotImplemented);
                return psyco_vi_NotImplemented();
            }
            if (result == 0) {
                goto error;
            }
            break;
        case CfPyErrIterNext:
            abort(); /* How can an iterator be pure */
        case CfPyErrAlways:
            goto error;
        default:
            abort(); /* Unhandled */
    }
    return VINFO_OK;
error:
    psyco_virtualize_exception(po);
    return NULL;
}

static long
var_call(void *c_function, int flags, int arg_count, char argtags[],
         long args[])
{
    ffi_type *call_args[arg_count];
    ffi_type *rtype;
    void *call_values[arg_count];
    long result;
    ffi_cif cif;
    int i;
    for (i = 0; i < arg_count; i++) {
        switch (argtags[i]) {
            case 'l':
                call_args[i] = &ffi_type_slong;
                call_values[i] = &args[i];
                break;
            case 'D':
                call_args[i] = &ffi_type_double;
                call_values[i] = &args[i];
                break;
            default:
                printf("var_call: unsupported '%c'\n", argtags[i]);
                abort();
        }
    }
    if (flags & CfReturnUnsigned) {
        if ((flags & CfReturnTypeMask) == CfReturnTypeInt) {
            rtype = &ffi_type_uint;
        }
        else if ((flags & CfReturnTypeMask) == CfReturnTypeLong) {
            rtype = &ffi_type_ulong;
        }
        else {
            printf("var_call: unsupported unsigned return type\n");
            abort();
        }
    }
    else {
        if ((flags & CfReturnTypeMask) == CfReturnTypeInt) {
            rtype = &ffi_type_sint;
        }
        else if ((flags & CfReturnTypeMask) == CfReturnTypeLong) {
            rtype = &ffi_type_slong;
        }
        else if ((flags & CfReturnTypeMask) == CfReturnTypeDouble) {
            rtype = &ffi_type_double;
        }
        else if ((flags & CfReturnTypeMask) == CfReturnTypeNative) {
            rtype = &ffi_type_slong;
        }
        else if ((flags & CfReturnTypeMask) == CfReturnTypePtr) {
            rtype = &ffi_type_pointer;
        }
        else if ((flags & CfReturnTypeMask) == CfReturnTypeVoid) {
            rtype = &ffi_type_void;
        }
        else {
            printf("var_call: unsupported signed return type\n");
            abort();
        }
    }
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, arg_count, rtype, call_args) ==
        FFI_OK) {
        ffi_call(&cif, c_function, &result, call_values);
    }
    else {
        printf("var_call: ffi error\n");
        abort();
    }
    return result;
}

DEFINEFN vinfo_t *
compile_time_call(PsycoObject *po, void *c_function, int flags, int arg_count,
                  char argtags[], long args[])
{
    vinfo_t *vresult;
    long result;

    result = var_call(c_function, flags, arg_count, argtags, args);
    vresult = compile_time_check(po, result, flags);
    if (vresult != VINFO_OK) {
        /* error */
        return vresult;
    }
    if (flags & CfNoReturnValue) {
        if (flags & CfNewRef) {
            Py_DECREF((PyObject *)result);
        }
        return VINFO_OK;
    }
    else if (flags & CfNewRef) {
        return vinfo_new(CompileTime_NewSk(sk_new(result, SkFlagPyObj)));
    }
    else {
        return vinfo_new(CompileTime_New(result));
    }
}

static vinfo_t *
run_time_result_check(PsycoObject *po, int flags)
{
    condition_code_t error_cc;
    int mark; /* TEMP_PUSH_R */
#define CMP_RETURN_MINUS_1()                                 \
    do {                                                     \
        BEGIN_CODE                                           \
        if ((flags & CfReturnTypeMask) == CfReturnTypeInt) { \
            CMP_I8_DR(-1, REG_X64_RAX);                      \
        }                                                    \
        else {                                               \
            CMP_I8_R(-1, REG_X64_RAX);                       \
        }                                                    \
        END_CODE                                             \
    } while (0)
#define TEST_RETURN()                                        \
    do {                                                     \
        BEGIN_CODE                                           \
        if ((flags & CfReturnTypeMask) == CfReturnTypeInt) { \
            TEST_DR_DR(REG_X64_RAX, REG_X64_RAX);            \
        }                                                    \
        else {                                               \
            TEST_R_R(REG_X64_RAX, REG_X64_RAX);              \
        }                                                    \
        END_CODE                                             \
    } while (0)
    switch (flags & CfPyErrMask) {
        case CfPyErrDontCheck:
            break;
        case CfPyErrIfNull: /* a return == 0 (or NULL) means an error */
            TEST_RETURN();
            error_cc = CC_E;
            goto test_condition;
        case CfPyErrIfNonNull: /* a return != 0 means an error */
            TEST_RETURN();
            error_cc = CC_NE;
            goto test_condition;
        case CfPyErrIfNeg: /* a return < 0 means an error */
            TEST_RETURN();
            error_cc = CC_S;
            goto test_condition;
        case CfPyErrIfMinus1: /* only -1 means an error */
            CMP_RETURN_MINUS_1();
            error_cc = CC_E;
            goto test_condition;
        case CfPyErrCheck: /* always check with PyErr_Occurred() */
            error_cc = integer_NON_NULL(po, psyco_PyErr_Occurred(po));
            goto test_condition;
        case CfPyErrCheckMinus1: /* use PyErr_Occurred() if return is -1 */
            CMP_RETURN_MINUS_1();
            error_cc = CC_E;
            goto test_condition_and_pyerr_occured;
        case CfPyErrCheckNeg: /* use PyErr_Occurred() if return is < 0 */
            TEST_RETURN();
            error_cc = CC_S;
            goto test_condition_and_pyerr_occured;
        case CfPyErrNotImplemented: /* test for a Py_NotImplemented result */
            BEGIN_CODE
            CMP_I_R((long)Py_NotImplemented, REG_FUNCTIONS_RETURN);
            END_CODE
            if (runtime_condition_f(po, CC_E)) {
                BEGIN_CODE
                DEC_OB_REFCNT(REG_FUNCTIONS_RETURN);
                END_CODE
                return psyco_vi_NotImplemented();
            }
            TEST_RETURN();
            error_cc = CC_E;
            goto test_condition;
        case CfPyErrIterNext: /* specially for tp_iternext slots */
            TEST_RETURN();
            error_cc = CC_E;
            if (!runtime_condition_f(po, error_cc)) {
                break; /* PyObject* not NULL so OK */
            }
            error_cc = integer_NON_NULL(po, psyco_PyErr_Occurred(po));
            if (error_cc == CC_ERROR || runtime_condition_f(po, error_cc)) {
                goto error;
            }
            /* NULL result with no error set; it is the end of the
               iteration. Raise a pseudo PyErr_StopIteration. */
            PycException_SetVInfo(po, PyExc_StopIteration, psyco_vi_None());
            return NULL;
        case CfPyErrAlways: /* always set an exception */
            goto error;
        default:
            abort(); /* unhandled */
    }
    return VINFO_OK;
test_condition_and_pyerr_occured:
    if (!runtime_condition_f(po, error_cc)) {
        return VINFO_OK;
    }
    /* PyErr_Occurred() while the result is still only in RAX: save RAX
       around the call to PyThreadState_Get. Nothing between the PUSH and
       the POP may go through the register allocator, which can spill with
       a PUSH of its own, so that the POP restores the wrong value and the
       spilled vinfo is left above the top of the stack (KNOWN_BUGS B14).
       curexc_type is read into the scratch register instead. */
    BEGIN_CODE
    NEED_CC(); /* before the PUSH: it may take a register */
    /* counted: the call's alignment and RSP-relative addressing use it */
    TEMP_PUSH_R(REG_FUNCTIONS_RETURN, mark);
    BEGIN_CALL(0);
    END_CALL_I(PyThreadState_Get);
    MOV_R_O(REG_TRANSIENT_1, REG_FUNCTIONS_RETURN,
            offsetof(PyThreadState, curexc_type));
    TEMP_POP_R(REG_FUNCTIONS_RETURN, mark);
    TEST_R_R(REG_TRANSIENT_1, REG_TRANSIENT_1);
    END_CODE
    error_cc = CC_NE;
test_condition:
    if (!runtime_condition_f(po, error_cc)) {
        return VINFO_OK;
    }
error:
    PycException_Raise(po, vinfo_new(VirtualTime_New(&ERtPython)), NULL);
    return NULL;
}
#undef FORGET_REF

/* run-time vinfo_t creation */
PSY_INLINE vinfo_t *
new_rtvinfo(PsycoObject *po, reg_t reg, bool ref, bool nonneg)
{
    vinfo_t *vi = vinfo_new(RunTime_New(reg, ref, nonneg));
    REG_NUMBER(po, reg) = vi;
    return vi;
}

static vinfo_t *
run_time_result(PsycoObject *po, int flags)
{
    reg_t rg;
    BEGIN_CODE
    NEED_FREE_REG(rg);
    if ((flags & CfReturnTypeMask) == CfReturnTypeInt) {
        if (flags & CfReturnUnsigned) {
            MOVZX_QR_DR(
                rg,
                REG_X64_RAX); /* mov unsigned (zero extended) from EAX to rg */
        }
        else {
            MOVSX_QR_DR(
                rg,
                REG_X64_RAX); /* mov signed (sign extended) from EAX to rg */
        }
    }
    else if ((flags & CfReturnTypeMask) == CfReturnTypeDouble) {
        MOVQ_QR_XMM(rg, REG_X64_XMM0);
    }
    else {
        MOV_R_R(rg, REG_X64_RAX);
    }
    END_CODE
    return new_rtvinfo(po, rg, flags & CfNewRef, flags & CfNotNegative);
}

DEFINEFN vinfo_t *
run_time_call(PsycoObject *po, void *c_function, int flags, int arg_count,
              char argtags[], long args[], int total_output_array_size)
{
    int stackbase, i, double_arg_count, double_arg_i, int_arg_i;
    vinfo_t *vresult;
    BEGIN_CODE
    NEED_CC();
    stackbase = po->stack_depth;
    po->stack_depth += total_output_array_size;
    STACK_CORRECTION(total_output_array_size);
    for (i = 0, double_arg_count = 0; i < arg_count; i++) {
        if (argtags[i] == 'd' || argtags[i] == 'D') {
            double_arg_count++;
        }
    }
    extra_assert(double_arg_count <= arg_count);
    extra_assert(double_arg_count <= 8); /* xmm0-xmm7 */
    BEGIN_CALL(arg_count - double_arg_count);
    /* SysV numbers integer and floating-point arguments separately: an
       integer argument's register is chosen by its position among the
       integer arguments only (int_arg_i), not by its overall position */
    for (i = arg_count, double_arg_i = double_arg_count - 1,
        int_arg_i = arg_count - double_arg_count;
         i--;) {
        if (argtags[i] != 'd' && argtags[i] != 'D')
            int_arg_i--;
        switch (argtags[i]) {
            case 'v':
                CALL_SET_ARG_FROM_RT(((vinfo_t *)args[i])->source, int_arg_i);
                break;
            case 'r':
                CALL_SET_ARG_FROM_STACK_REF(args[i], int_arg_i);
                break;
            case 'a':
            case 'A': {
                vinfo_array_t *array = (vinfo_array_t *)args[i];
                bool with_reference = (argtags[i] == 'A');
                int j = array->count;
                while (j > 0) {
                    stackbase += sizeof(long);
                    array->items[--j] = vinfo_new(
                        RunTime_NewStack(stackbase, with_reference, false));
                }
                CALL_SET_ARG_FROM_STACK_REF(array->items[0]->source, int_arg_i);
                break;
            }
            case 'd':
                CALL_SET_ARG_FROM_RT_DOUBLE(((vinfo_t *)args[i])->source,
                                            double_arg_i);
                double_arg_i--;
                break;
            case 'D':
                CALL_SET_ARG_IMMED_DOUBLE(args[i], double_arg_i);
                double_arg_i--;
                break;
            case 'l':
                CALL_SET_ARG_IMMED(args[i], int_arg_i);
                break;
            default:
                printf("var_call: unsupported '%c'\n", argtags[i]);
                abort();
        }
    }
    END_CALL_I(c_function);
    END_CODE
    vresult = run_time_result_check(po, flags);
    if (vresult != VINFO_OK) {
        return vresult;
    }
    if (flags & CfNoReturnValue) {
        if (flags & CfNewRef) {
            BEGIN_CODE
            DEC_OB_REFCNT(REG_X64_RAX);
            END_CODE
        }
        return VINFO_OK;
    }
    return run_time_result(po, flags);
}

DEFINEFN
vinfo_t *
psyco_call_psyco(PsycoObject *po, CodeBufferObject *codebuf,
                 Source argsources[], int argcount,
                 struct stack_frame_info_s *finfo)
{
    /* this is a simplified version of psyco_generic_call() which
        assumes Psyco's calling convention instead of the C's. */
    int i;
    bool ccflags;
    int initial_stack_depth;
    int stack_correction;
    vinfo_t *vresult;

    BEGIN_CODE
    /* cannot use NEED_CC(): it might clobber one of the registers
       mentioned in argsources */
    ccflags = HAS_CCREG(po);
    if (ccflags)
        PUSH_CC();

    /* PUSH RTs with reg set */
    for (i = 0; i < REG_TOTAL; i++) {
        vinfo_t *content = REG_NUMBER(po, i);
        if (content != NULL) {
            PUSH_R(i);
            psyco_inc_stackdepth(po);
        }
    }

    /* add 16 byte aligned stack */
    do {
        stack_correction =
            (16 - (((argcount * sizeof(long)) + STACK_DEPTH_SINCE_CALL() +
                    INITIAL_STACK_DEPTH) %
                   16)) %
            16;
        STACK_CORRECTION(stack_correction);
        po->stack_depth += stack_correction;
    } while (0);

    PUSH_I(-1); /* finfo */
    psyco_inc_stackdepth(po);

    ABOUT_TO_CALL_SUBFUNCTION(finfo);
    /* offset in stack to this this finfo */
    finfo_last(finfo)->link_stack_depth =
        po->stack_depth - INITIAL_STACK_DEPTH;
    initial_stack_depth = po->stack_depth;
    /* We assume that compiler register state is not changed by the call macros
     */
    /* note that we are not using C calling convention here */
    for (i = 0; i < argcount; i++) {
        Source source = argsources[i];
        if (RSOURCE_REG_IS_NONE(source)) {
            extra_assert(getstack(source) != RUNTIME_STACK_NONE);
            PUSH_O(REG_X64_RSP, STACK_POS_OFFSET(RSOURCE_STACK(source)));
        }
        else {
            PUSH_R(getreg(source));
        }
        psyco_inc_stackdepth(po);
    }
    po->stack_depth += stack_correction;

    CALL_I(codebuf->codestart);
    /* psyco callees remove args :| */
    po->stack_depth = initial_stack_depth;
    RETURNED_FROM_SUBFUNCTION();
    ADD_R_I8(REG_X64_RSP, sizeof(long));
    psyco_dec_stackdepth(po);
    /* remove stack correction */
    STACK_CORRECTION(-stack_correction);
    po->stack_depth -= stack_correction;
    /* POP RTs with reg set */
    for (i = REG_TOTAL - 1; i >= 0; i--) {
        vinfo_t *content = REG_NUMBER(po, i);
        if (content != NULL) {
            POP_R(i);
            psyco_dec_stackdepth(po);
        }
    }

    if (ccflags)
        POP_CC();

    END_CODE
    vresult = run_time_result_check(po, CfCommonNewRefPyObject);
    if (vresult != VINFO_OK) {
        return vresult;
    }
    return run_time_result(po, CfCommonNewRefPyObject);
}

/* internal, see NEED_CC() */
EXTERNFN condition_code_t
cc_from_vsource(Source source); /* in codegen.c */

DEFINEFN
code_t *
psyco_compute_cc(PsycoObject *po, code_t *code, reg_t reserved)
{
    int i;
    vinfo_t *v;
    condition_code_t cc;
    reg_t rg;
    for (i = 0; i < 2; i++) {
        v = po->ccregs[i];
        if (v == NULL)
            continue;
        cc = cc_from_vsource(v->source);

        NEED_FREE_BYTE_REG(rg, reserved, REG_NONE);
        LOAD_REG_FROM_CONDITION(rg, cc);

        v->source = RunTime_New(rg, false, true);
        REG_NUMBER(po, rg) = v;
        po->ccregs[i] = NULL;
    }
    return code;
}

/*****************************************************************/
/***   Emit common instructions                                ***/

DEFINEFN
vinfo_t *
bininstrgrp(PsycoObject *po, int group, bool ovf, bool nonneg, vinfo_t *v1,
            vinfo_t *v2)
{
    reg_t rg;
    BEGIN_CODE
    NEED_CC();
    COPY_IN_REG(v1, rg);                      /* MOV rg, (v1) */
    COMMON_INSTR_FROM(group, rg, v2->source); /* XXX rg, (v2) */
    END_CODE
    if (ovf && runtime_condition_f(po, CC_O))
        return NULL; /* if overflow */
    return new_rtvinfo(po, rg, false, nonneg);
}

DEFINEFN
vinfo_t *
bint_add_i(PsycoObject *po, vinfo_t *rt1, long value2, bool unsafe)
{
    reg_t rg, dst;
    extra_assert(is_runtime(rt1->source));
    BEGIN_CODE
    if (!FITS_IN_32BITS(value2))
        NEED_CC(); /* the ADD below clobbers the flags (the LEA doesn't);
                      first, as it may take a free register */
    NEED_FREE_REG(dst);
    rg = getreg(rt1->source);
    if (rg == REG_NONE) {
        rg = dst;
        LOAD_REG_FROM(rt1->source, rg);
    }
    if ((value2 < -2147483648) || (value2 > 2147483647)) {
        MOV_R_I(REG_TRANSIENT_1, value2);
        ADD_R_R(REG_TRANSIENT_1, rg);
        MOV_R_R(dst, REG_TRANSIENT_1);
    }
    else {
        LOAD_REG_FROM_REG_PLUS_IMMED(dst, rg, value2);
    }
    END_CODE
    return new_rtvinfo(po, dst, false,
                       unsafe && value2 >= 0 && is_rtnonneg(rt1->source));
}

#define GENERIC_SHIFT_BY(rtmacro, nonneg)                \
    {                                                    \
        reg_t rg;                                        \
        extra_assert(0 < counter && counter < LONG_BIT); \
        BEGIN_CODE                                       \
        NEED_CC();                                       \
        COPY_IN_REG(v1, rg);                             \
        rtmacro(rg, counter);                            \
        END_CODE                                         \
        return new_rtvinfo(po, rg, false, nonneg);       \
    }

DEFINEFN
vinfo_t *
bininstrshift(PsycoObject *po, int group, bool nonneg, vinfo_t *v1,
              vinfo_t *v2)
{
    reg_t rg;
    BEGIN_CODE
    if (RSOURCE_REG(v2->source) != SHIFT_COUNTER) {
        NEED_REGISTER(SHIFT_COUNTER);
        LOAD_REG_FROM(v2->source, SHIFT_COUNTER);
    }
    NEED_CC_REG(SHIFT_COUNTER);
    DELAY_USE_OF(SHIFT_COUNTER);
    COPY_IN_REG(v1, rg);
    SHIFT_GENERICCL(rg, group); /* SHx rg, CL */
    END_CODE
    return new_rtvinfo(po, rg, false, nonneg);
}

DEFINEFN
vinfo_t *
bint_lshift_i(PsycoObject *po, vinfo_t *v1,
              int counter) GENERIC_SHIFT_BY(SHIFT_LEFT_BY, false)

    DEFINEFN vinfo_t *bint_rshift_i(PsycoObject *po, vinfo_t *v1, int counter)
        GENERIC_SHIFT_BY(SHIFT_SIGNED_RIGHT_BY, is_nonneg(v1->source))

            DEFINEFN vinfo_t *bint_urshift_i(PsycoObject *po, vinfo_t *v1,
                                             int counter)
                GENERIC_SHIFT_BY(SHIFT_RIGHT_BY, true)

                    DEFINEFN vinfo_t *bint_mul_i(PsycoObject *po, vinfo_t *v1,
                                                 long value2, bool ovf)
{
    reg_t rg;
    BEGIN_CODE
    NEED_CC();
    NEED_FREE_REG(rg);
    IMUL_IMMED_FROM_RT(v1->source, value2, rg);
    END_CODE
    if (ovf && runtime_condition_f(po, CC_O))
        return NULL;
    return new_rtvinfo(po, rg, false,
                       ovf && value2 >= 0 && is_rtnonneg(v1->source));
}

DEFINEFN
vinfo_t *
bininstrmul(PsycoObject *po, bool ovf, bool nonneg, vinfo_t *v1, vinfo_t *v2)
{
    reg_t rg;
    BEGIN_CODE
    NEED_CC();
    COPY_IN_REG(v1, rg);              /* MOV rg, (v1) */
    IMUL_REG_FROM_RT(v2->source, rg); /* IMUL rg, (v2) */
    END_CODE
    if (ovf && runtime_condition_f(po, CC_O))
        return NULL; /* if overflow */
    return new_rtvinfo(po, rg, false, nonneg);
}

DEFINEFN
vinfo_t *
unaryinstrgrp(PsycoObject *po, int group, bool ovf, bool nonneg, vinfo_t *v1)
{
    reg_t rg;
    BEGIN_CODE
    NEED_CC();
    COPY_IN_REG(v1, rg);           /* MOV rg, (v1) */
    UNARY_INSTR_ON_REG(group, rg); /* XXX rg       */
    END_CODE
    if (ovf && runtime_condition_f(po, CC_O))
        return NULL; /* if overflow */
    return new_rtvinfo(po, rg, false, nonneg);
}

/* XXX assumes RAX is free to trash */
DEFINEFN
vinfo_t *
unaryinstrabs(PsycoObject *po, bool ovf, bool nonneg, vinfo_t *v1)
{
    reg_t rg;
    BEGIN_CODE
    NEED_CC();
    if (getreg(v1->source) == REG_NONE) {
        LOAD_REG_FROM_EBP_BASE(REG_X64_RAX, RUNTIME_STACK(v1));
    }
    else {
        MOV_R_R(REG_X64_RAX, getreg(v1->source));
    }
    NEED_FREE_REG_COND(rg, (rg != REG_X64_RAX && rg != REG_X64_RDX &&
                            rg != getreg(v1->source)));
    if (REG_NUMBER(po, REG_X64_RDX) != NULL) {
        MOV_R_R(rg, REG_X64_RDX);
    }
    /* ABS RAX */
    CQO();
    ADD_R_R(REG_X64_RAX, REG_X64_RDX);
    XOR_R_R(REG_X64_RAX, REG_X64_RDX);
    /*     */
    if (REG_NUMBER(po, REG_X64_RDX) != NULL) {
        MOV_R_R(REG_X64_RDX, rg);
    }
    MOV_R_R(rg, REG_X64_RAX);
    END_CODE
    /* if number is negative we overflowed */
    if (ovf && runtime_condition_f(po, CC_S))
        return NULL;
    return new_rtvinfo(po, rg, false, nonneg);
}

static const condition_code_t direct_results[16] = {
    /*****   signed comparison      **/
    /* Py_LT: */ CC_L,
    /* Py_LE: */ CC_LE,
    /* Py_EQ: */ CC_E,
    /* Py_NE: */ CC_NE,
    /* Py_GT: */ CC_G,
    /* Py_GE: */ CC_GE,
    /* (6)    */ CC_ERROR,
    /* (7)    */ CC_ERROR,
    /*****  unsigned comparison     **/
    /* Py_LT: */ CC_uL,
    /* Py_LE: */ CC_uLE,
    /* Py_EQ: */ CC_E,
    /* Py_NE: */ CC_NE,
    /* Py_GT: */ CC_uG,
    /* Py_GE: */ CC_uGE,
    /* (14)   */ CC_ERROR,
    /* (15)   */ CC_ERROR};

DEFINEFN
condition_code_t
bint_cmp_i(PsycoObject *po, int base_py_op, vinfo_t *rt1, long immed2)
{
    BEGIN_CODE
    NEED_CC();
    COMPARE_IMMED_FROM_RT(rt1->source, immed2);
    END_CODE
    return direct_results[base_py_op];
}

DEFINEFN
condition_code_t
bininstrcmp(PsycoObject *po, int base_py_op, vinfo_t *v1, vinfo_t *v2)
{
    BEGIN_CODE
    NEED_CC();
    RTVINFO_IN_REG(v1); /* CMP v1, v2 */
    COMMON_INSTR_FROM_RT(7, getreg(v1->source), v2->source);
    END_CODE
    return direct_results[base_py_op];
}

DEFINEFN
vinfo_t *
bininstrcond(PsycoObject *po, condition_code_t cc, long immed_true,
             long immed_false)
{
    reg_t rg;
    BEGIN_CODE
    NEED_FREE_REG(rg);
    LOAD_REG_FROM_IMMED(rg, immed_true);
    BEGIN_SHORT_COND_JUMP(1, cc);
    LOAD_REG_FROM_IMMED(rg, immed_false);
    END_SHORT_JUMP(1);
    END_CODE
    return new_rtvinfo(po, rg, false, immed_true >= 0 && immed_false >= 0);
}

DEFINEFN
vinfo_t *
make_runtime_copy(PsycoObject *po, vinfo_t *v)
{
    reg_t rg;
    if (!compute_vinfo(v, po))
        return NULL;
    BEGIN_CODE
    NEED_FREE_REG(rg);
    LOAD_REG_FROM(v->source, rg);
    END_CODE
    return new_rtvinfo(po, rg, false, is_nonneg(v->source));
}

#if 0
DEFINEFN   -- unused --
vinfo_t* integer_and_i(PsycoObject* po, vinfo_t* v1, long value2)
     GENERIC_BINARY_INSTR_2(4, a & b,    /* AND */
			    value2>=0 || is_rtnonneg(v1->source))
#define GENERIC_BINARY_INSTR_2(group, c_code, nonneg)                   \
    {                                                                   \
        if (!compute_vinfo(v1, po))                                     \
            return NULL;                                                \
        if (is_compiletime(v1->source)) {                               \
            long a = CompileTime_Get(v1->source)->value;                \
            long b = value2;                                            \
            long c = (c_code);                                          \
            return vinfo_new(CompileTime_New(c));                       \
        }                                                               \
        else {                                                          \
            reg_t rg;                                                   \
            BEGIN_CODE                                                  \
            NEED_CC();                                                  \
            COPY_IN_REG(v1, rg);                   /* MOV rg, (v1) */   \
            COMMON_INSTR_IMMED(group, rg, value2); /* XXX rg, value2 */ \
            END_CODE                                                    \
            return new_rtvinfo(po, rg, false, nonneg);                  \
        }                                                               \
    }
#endif

#if 0
DEFINEFN      (not used)
vinfo_t* integer_seqindex(PsycoObject* po, vinfo_t* vi, vinfo_t* vn, bool ovf)
{
  NonVirtualSource vns, vis;
  vns = vinfo_compute(vn, po);
  if (vns == SOURCE_ERROR) return NULL;
  vis = vinfo_compute(vi, po);
  if (vis == SOURCE_ERROR) return NULL;
  
  if (!is_compiletime(vis))
    {
      reg_t rg, tmprg;
      BEGIN_CODE
      NEED_CC_SRC(vis);
      NEED_FREE_REG(rg);
      LOAD_REG_FROM_RT(vis, rg);
      DELAY_USE_OF(rg);
      NEED_FREE_REG(tmprg);

      /* Increase 'rg' by 'vns' unless it is already in the range(0, vns). */
         /* CMP i, n */
      vns = vn->source;  /* reload, could have been moved by NEED_FREE_REG */
      COMMON_INSTR_FROM(7, rg, vns);
         /* SBB t, t */
      COMMON_INSTR_FROM_RT(3, tmprg, RunTime_New(tmprg, false...));
         /* AND t, n */
      COMMON_INSTR_FROM(4, tmprg, vns);
         /* SUB i, t */
      COMMON_INSTR_FROM_RT(5, rg, RunTime_New(tmprg, false...));
         /* ADD i, n */
      COMMON_INSTR_FROM(0, rg, vns);
      END_CODE

      if (ovf && runtime_condition_f(po, CC_NB))  /* if out of range */
        return NULL;
      return new_rtvinfo(po, rg, false...);
    }
  else
    {
      long index = CompileTime_Get(vis)->value;
      long reqlength;
      if (index >= 0)
        reqlength = index;  /* index is known, length must be greater than it */
      else
        reqlength = ~index;  /* idem for negative index */
      if (ovf)
        {
          /* test for out of range index -- more precisely, test that the
             length is not large enough for the known index */
          condition_code_t cc = integer_cmp_i(po, vn, reqlength, Py_LE);
          if (cc == CC_ERROR || runtime_condition_f(po, cc))
            return NULL;
        }
      if (index >= 0)
        {
          vinfo_incref(vi);
          return vi;
        }
      else
        return integer_add_i(po, vn, index...);
    }
}
#endif /* 0 */
