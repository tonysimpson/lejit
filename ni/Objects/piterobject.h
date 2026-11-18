 /***************************************************************/
/***            Psyco equivalent of iterobject.h               ***/
 /***************************************************************/

#ifndef _PSY_ITEROBJECT_H
#define _PSY_ITEROBJECT_H


#include "pobject.h"
#include "pabstract.h"


/* seqiterobject, private to Objects/iterobject.c, is copied from the
   source into layouts_gen.h */
#define SEQITER_it_index  FMUT(CPY_FIELD(seqiterobject, it_index, OB_type))
#define SEQITER_it_seq    CPY_FIELD(seqiterobject, it_seq, SEQITER_it_index)
#define iSEQITER_IT_INDEX FIELD_INDEX(SEQITER_it_index)
#define iSEQITER_IT_SEQ   FIELD_INDEX(SEQITER_it_seq)
#define SEQITER_TOTAL     FIELDS_TOTAL(SEQITER_it_seq)


/*********************************************************************/
 /* Virtual sequence iterators. Created if needed by PySeqIter_New(). */
EXTERNVAR source_virtual_t psyco_computed_seqiter;

/* !! consumes a ref on 'seq' */
EXTERNFN vinfo_t* PsycoSeqIter_NEW(PsycoObject* po, vinfo_t* seq);

PSY_INLINE vinfo_t* PsycoSeqIter_New(PsycoObject* po, vinfo_t* seq)
{
	vinfo_incref(seq);
	return PsycoSeqIter_NEW(po, seq);
}

#endif /* _PSY_ITEROBJECT_H */
