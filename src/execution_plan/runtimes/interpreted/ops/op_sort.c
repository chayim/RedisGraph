/*
* Copyright 2018-2021 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#include "op_sort.h"
#include "op_project.h"
#include "op_aggregate.h"
#include "../../../../util/arr.h"
#include "../../../../util/qsort.h"
#include "../../../../util/rmalloc.h"
#include "../../../../query_ctx.h"

/* Forward declarations. */
static RT_OpResult SortInit(RT_OpBase *opBase);
static Record SortConsume(RT_OpBase *opBase);
static RT_OpResult SortReset(RT_OpBase *opBase);
static RT_OpBase *SortClone(const RT_ExecutionPlan *plan, const RT_OpBase *opBase);
static void SortFree(RT_OpBase *opBase);

// Heapsort function to compare two records on a subset of fields.
// Return value similar to strcmp.
static int _record_compare(Record a, Record b, const RT_OpSort *op) {
	uint comparison_count = array_len(op->record_offsets);
	for(uint i = 0; i < comparison_count; i++) {
		SIValue aVal = Record_Get(a, op->record_offsets[i]);
		SIValue bVal = Record_Get(b, op->record_offsets[i]);
		int rel = SIValue_Compare(aVal, bVal, NULL);
		if(rel == 0) continue;      // Elements are equal; try next ORDER BY element.
		rel *= op->directions[i];   // Flip value for descending order.
		return rel;
	}
	return 0;
}

// Quicksort function to compare two records on a subset of fields.
// Returns true if a is less than b.
static bool _record_islt(Record a, Record b, const RT_OpSort *op) {
	return _record_compare(a, b, op) >
		   0; // Return true if the current left element is less than the right.
}

// Compares two heap record nodes.
static int _heap_elem_compare(const void *A, const void *B, const void *udata) {
	RT_OpSort *op = (RT_OpSort *)udata;
	Record aRec = (Record)A;
	Record bRec = (Record)B;
	return _record_compare(aRec, bRec, op);
}

static void _accumulate(RT_OpSort *op, Record r) {
	if(op->limit == UNLIMITED) {
		/* Not using a heap and there's room for record. */
		array_append(op->buffer, r);
		return;
	}

	if(Heap_count(op->heap) < op->limit) {
		Heap_offer(&op->heap, r);
	} else {
		// No room in the heap, see if we need to replace
		// a heap stored record with the current record.
		if(_heap_elem_compare(Heap_peek(op->heap), r, op) > 0) {
			Record replaced = Heap_poll(op->heap);
			RT_OpBase_DeleteRecord(replaced);
			Heap_offer(&op->heap, r);
		} else {
			RT_OpBase_DeleteRecord(r);
		}
	}
}

static inline Record _handoff(RT_OpSort *op) {
	if(array_len(op->buffer) > 0) return array_pop(op->buffer);
	return NULL;
}

RT_OpBase *RT_NewSortOp(const RT_ExecutionPlan *plan, AR_ExpNode **exps, int *directions) {
	RT_OpSort *op = rm_malloc(sizeof(RT_OpSort));
	op->heap = NULL;
	op->skip = 0;
	op->limit = UNLIMITED;
	op->buffer = NULL;
	op->directions = directions;
	op->exps = exps;

	// Set our Op operations
	RT_OpBase_Init((RT_OpBase *)op, OPType_SORT, SortInit, SortConsume, SortReset, SortClone,
				SortFree, false, plan);

	uint comparison_count = array_len(exps);
	op->record_offsets = array_new(uint, comparison_count);
	for(uint i = 0; i < comparison_count; i ++) {
		int record_idx;
		bool aware = RT_OpBase_Aware((RT_OpBase *)op, exps[i]->resolved_name, &record_idx);
		ASSERT(aware);
		array_append(op->record_offsets, record_idx);
	}

	return (RT_OpBase *)op;
}

static RT_OpResult SortInit(RT_OpBase *opBase) {
	RT_OpSort *op = (RT_OpSort *)opBase;
	// If there is LIMIT value, l, set in the current clause,
	// the operation must return the top l records with respect to
	// the sorting criteria. In order to do so, it must collect the l records,
	// but if there is a SKIP value, s, set, it must collect l+s records,
	// sort them and return the top l.
	if(op->limit != UNLIMITED) {
		op->limit += op->skip;
		// If a limit is specified, use heapsort to poll the top N.
		op->heap = Heap_new(_heap_elem_compare, op);
	} else {
		// If all records are being sorted, use quicksort.
		op->buffer = array_new(Record, 32);
	}

	return OP_OK;
}

/* `op` is an actual variable in the caller function. Using it in a
 * macro like this is rather ugly, but the macro passed to QSORT must
 * accept only 2 arguments. */
#define RECORD_SORT(a, b) (_record_islt((*a), (*b), op))

static Record SortConsume(RT_OpBase *opBase) {
	RT_OpSort *op = (RT_OpSort *)opBase;
	Record r = _handoff(op);
	if(r) return r;

	// If we're here, we don't have any records to return
	// try to get records.
	RT_OpBase *child = op->op.children[0];
	bool newData = false;
	while((r = RT_OpBase_Consume(child))) {
		_accumulate(op, r);
		newData = true;
	}
	if(!newData) return NULL;

	if(op->buffer) {
		QSORT(Record, op->buffer, array_len(op->buffer), RECORD_SORT);
	} else {
		// Heap, responses need to be reversed.
		int records_count = Heap_count(op->heap);
		op->buffer = array_new(Record, records_count);

		/* Pop items from heap */
		while(records_count > 0) {
			r = Heap_poll(op->heap);
			array_append(op->buffer, r);
			records_count--;
		}
	}

	// Pass ordered records downward.
	return _handoff(op);
}

/* Restart iterator */
static RT_OpResult SortReset(RT_OpBase *ctx) {
	RT_OpSort *op = (RT_OpSort *)ctx;
	uint recordCount;

	if(op->heap) {
		recordCount = Heap_count(op->heap);
		for(uint i = 0; i < recordCount; i++) {
			Record r = (Record)Heap_poll(op->heap);
			RT_OpBase_DeleteRecord(r);
		}
	}

	if(op->buffer) {
		recordCount = array_len(op->buffer);
		for(uint i = 0; i < recordCount; i++) {
			Record r = array_pop(op->buffer);
			RT_OpBase_DeleteRecord(r);
		}
	}

	return OP_OK;
}

static RT_OpBase *SortClone(const RT_ExecutionPlan *plan, const RT_OpBase *opBase) {
	ASSERT(opBase->type == OPType_SORT);
	RT_OpSort *op = (RT_OpSort *)opBase;
	int *directions;
	AR_ExpNode **exps;
	array_clone(directions, op->directions);
	array_clone_with_cb(exps, op->exps, AR_EXP_Clone);
	return RT_NewSortOp(plan, exps, directions);
}

/* Frees Sort */
static void SortFree(RT_OpBase *ctx) {
	RT_OpSort *op = (RT_OpSort *)ctx;

	if(op->heap) {
		uint recordCount = Heap_count(op->heap);
		for(uint i = 0; i < recordCount; i++) {
			Record r = (Record)Heap_poll(op->heap);
			RT_OpBase_DeleteRecord(r);
		}
		Heap_free(op->heap);
		op->heap = NULL;
	}

	if(op->buffer) {
		uint recordCount = array_len(op->buffer);
		for(uint i = 0; i < recordCount; i++) {
			Record r = array_pop(op->buffer);
			RT_OpBase_DeleteRecord(r);
		}
		array_free(op->buffer);
		op->buffer = NULL;
	}

	if(op->record_offsets) {
		array_free(op->record_offsets);
		op->record_offsets = NULL;
	}

	if(op->directions) {
		array_free(op->directions);
		op->directions = NULL;
	}

	if(op->exps) {
		uint exps_count = array_len(op->exps);
		for(uint i = 0; i < exps_count; i++) AR_EXP_Free(op->exps[i]);
		array_free(op->exps);
		op->exps = NULL;
	}
}
