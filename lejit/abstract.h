#ifndef LE_ABSTRACT_H
#define LE_ABSTRACT_H
#include "compiler.h"
#include "encoder.h"

LeValue*
_Le_CheckFunctionResult(LeCompilerObject* c, LeValue *callable, 
                        LeValue *result, const char *where);


#endif /* LE_ABSTRACT_H */
