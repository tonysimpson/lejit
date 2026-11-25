#ifndef LE_MECHANISMS_OSR_TOOL_H
#define LE_MECHANISMS_OSR_TOOL_H
/* sys.monitoring's tool id for optimizers, as CPython names it
   (OPTIMIZER_ID), with which LeJIT counts backward jumps (osr.c); and its
   bit in each event's byte of a code's tools, which isn't a tool watching
   the code */
#define LE_OSR_TOOL 5
#define LE_OSR_TOOL_BITS (0x0101010101010101ULL << LE_OSR_TOOL)
#endif
