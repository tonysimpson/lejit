// CPython 3.7's simple opcodes, written in 3.12's instruction DSL
// (Python/bytecodes.c; Tools/cases_generator/interpreter_definition.md).
//
// Input to tools/metac/gen_ceval.py (doc/DECISIONS.md D26). On 3.12 the
// generator reads upstream's bytecodes.c instead, so only this file is
// specific to the 3.7 baseline. It is a description of 3.7's Python/ceval.c,
// not Ni code: each instruction says what ceval.c's TARGET case does, using
// 3.12's names where 3.12 has the same instruction (POP_TOP, LOAD_CONST,
// UNARY_NEGATIVE, STORE_SUBSCR...). gen_ceval.py checks every entry against
// 3.7: the TARGET exists in ceval.c, the C-API functions called here are
// called there, and the stack effect equals dis.stack_effect().
//
// Differences from ceval.c are noted where they occur. None changes what a
// program computes; they are fast paths that 3.12 also moved out of the
// generic instruction.

// BEGIN BYTECODES //

        inst(NOP, (--)) {
        }

        inst(POP_TOP, (value --)) {
            DECREF_INPUTS();
        }

        // 3.12: SWAP 2
        inst(ROT_TWO, (second, top -- top, second)) {
        }

        inst(ROT_THREE, (third, second, top -- top, third, second)) {
        }

        // 3.12: COPY 1
        inst(DUP_TOP, (top -- top, top2)) {
            top2 = Py_NewRef(top);
        }

        inst(DUP_TOP_TWO, (second, top -- second, top, second2, top2)) {
            second2 = Py_NewRef(second);
            top2 = Py_NewRef(top);
        }

        inst(LOAD_CONST, (-- value)) {
            value = GETITEM(consts, oparg);
            Py_INCREF(value);
        }

        // 3.12 splits this into LOAD_FAST (the compiler proved the local
        // bound) and LOAD_FAST_CHECK, which is this.
        inst(LOAD_FAST, (-- value)) {
            value = GETLOCAL(oparg);
            ERROR_IF(value == NULL, unbound_local_error);
            Py_INCREF(value);
        }

        inst(STORE_FAST, (value --)) {
            SETLOCAL(oparg, value);
        }

        inst(UNARY_POSITIVE, (value -- res)) {
            res = PyNumber_Positive(value);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(UNARY_NEGATIVE, (value -- res)) {
            res = PyNumber_Negative(value);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(UNARY_INVERT, (value -- res)) {
            res = PyNumber_Invert(value);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        // ceval.c tests for Py_True and Py_False before calling
        // PyObject_IsTrue; 3.12 has the same result without the tests.
        inst(UNARY_NOT, (value -- res)) {
            int err = PyObject_IsTrue(value);
            DECREF_INPUTS();
            ERROR_IF(err < 0, error);
            if (err == 0) {
                res = Py_True;
            }
            else {
                res = Py_False;
            }
        }

        inst(BINARY_POWER, (base, exp -- res)) {
            res = PyNumber_Power(base, exp, Py_None);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(BINARY_MULTIPLY, (left, right -- res)) {
            res = PyNumber_Multiply(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(BINARY_MATRIX_MULTIPLY, (left, right -- res)) {
            res = PyNumber_MatrixMultiply(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        // ceval.c formats str % args with PyUnicode_Format directly; the
        // result is the same as PyNumber_Remainder's.
        inst(BINARY_MODULO, (left, right -- res)) {
            res = PyNumber_Remainder(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        // ceval.c concatenates two strs in place when it can
        // (unicode_concatenate); 3.12 made that the specialisation
        // BINARY_OP_INPLACE_ADD_UNICODE.
        inst(BINARY_ADD, (left, right -- res)) {
            res = PyNumber_Add(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(BINARY_SUBTRACT, (left, right -- res)) {
            res = PyNumber_Subtract(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(BINARY_SUBSCR, (container, sub -- res)) {
            res = PyObject_GetItem(container, sub);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(BINARY_FLOOR_DIVIDE, (left, right -- res)) {
            res = PyNumber_FloorDivide(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(BINARY_TRUE_DIVIDE, (left, right -- res)) {
            res = PyNumber_TrueDivide(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(BINARY_LSHIFT, (left, right -- res)) {
            res = PyNumber_Lshift(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(BINARY_RSHIFT, (left, right -- res)) {
            res = PyNumber_Rshift(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(BINARY_AND, (left, right -- res)) {
            res = PyNumber_And(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(BINARY_XOR, (left, right -- res)) {
            res = PyNumber_Xor(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(BINARY_OR, (left, right -- res)) {
            res = PyNumber_Or(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(INPLACE_POWER, (base, exp -- res)) {
            res = PyNumber_InPlacePower(base, exp, Py_None);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(INPLACE_MULTIPLY, (left, right -- res)) {
            res = PyNumber_InPlaceMultiply(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(INPLACE_MATRIX_MULTIPLY, (left, right -- res)) {
            res = PyNumber_InPlaceMatrixMultiply(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(INPLACE_TRUE_DIVIDE, (left, right -- res)) {
            res = PyNumber_InPlaceTrueDivide(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(INPLACE_FLOOR_DIVIDE, (left, right -- res)) {
            res = PyNumber_InPlaceFloorDivide(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(INPLACE_MODULO, (left, right -- res)) {
            res = PyNumber_InPlaceRemainder(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        // in-place str concatenation as for BINARY_ADD
        inst(INPLACE_ADD, (left, right -- res)) {
            res = PyNumber_InPlaceAdd(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(INPLACE_SUBTRACT, (left, right -- res)) {
            res = PyNumber_InPlaceSubtract(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(INPLACE_LSHIFT, (left, right -- res)) {
            res = PyNumber_InPlaceLshift(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(INPLACE_RSHIFT, (left, right -- res)) {
            res = PyNumber_InPlaceRshift(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(INPLACE_AND, (left, right -- res)) {
            res = PyNumber_InPlaceAnd(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(INPLACE_XOR, (left, right -- res)) {
            res = PyNumber_InPlaceXor(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(INPLACE_OR, (left, right -- res)) {
            res = PyNumber_InPlaceOr(left, right);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        // ceval.c also PREDICTs FOR_ITER and CALL_FUNCTION (a dispatch
        // shortcut)
        inst(GET_ITER, (iterable -- iter)) {
            iter = PyObject_GetIter(iterable);
            DECREF_INPUTS();
            ERROR_IF(iter == NULL, error);
        }

        inst(STORE_SUBSCR, (v, container, sub --)) {
            /* container[sub] = v */
            int err = PyObject_SetItem(container, sub, v);
            DECREF_INPUTS();
            ERROR_IF(err, error);
        }

        inst(DELETE_SUBSCR, (container, sub --)) {
            /* del container[sub] */
            int err = PyObject_DelItem(container, sub);
            DECREF_INPUTS();
            ERROR_IF(err, error);
        }

// END BYTECODES //
