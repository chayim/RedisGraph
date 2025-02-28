/*
* Copyright 2018-2020 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#include "op_conditional_traverse.h"
#include "RG.h"
#include "shared/print_functions.h"
#include "../../query_ctx.h"

// default number of records to accumulate before traversing
#define BATCH_SIZE 16

/* Forward declarations. */
static OpResult CondTraverseInit(OpBase *opBase);
static Record CondTraverseConsume(OpBase *opBase);
static OpResult CondTraverseReset(OpBase *opBase);
static OpBase *CondTraverseClone(const ExecutionPlan *plan, const OpBase *opBase);
static void CondTraverseFree(OpBase *opBase);

static void CondTraverseToString(const OpBase *ctx, sds *buf) {
	TraversalToString(ctx, buf, ((const OpCondTraverse *)ctx)->ae);
}

static void _populate_filter_matrix(OpCondTraverse *op) {
	GrB_Matrix FM = RG_MATRIX_M(op->F);

	for(uint i = 0; i < op->record_count; i++) {
		Record r = op->records[i];
		/* Update filter matrix F, set row i at position srcId
		 * F[i, srcId] = true. */
		Node *n = Record_GetNode(r, op->srcNodeIdx);
		NodeID srcId = ENTITY_GET_ID(n);	
		GrB_Matrix_setElement_BOOL(FM, true, i, srcId);
	}

	GrB_Matrix_wait(FM, GrB_MATERIALIZE);
}

/* Evaluate algebraic expression:
 * prepends filter matrix as the left most operand
 * perform multiplications
 * set iterator over result matrix
 * removed filter matrix from original expression
 * clears filter matrix. */
void _traverse(OpCondTraverse *op) {
	// If op->F is null, this is the first time we are traversing.
	if(op->F == NULL) {
		/* Create both filter and result matrices.
		 * make sure M's format is SPARSE, required by the matrix iterator */
		size_t required_dim = Graph_RequiredMatrixDim(op->graph);
		RG_Matrix_new(&op->M, GrB_BOOL, op->record_cap, required_dim);
		RG_Matrix_new(&op->F, GrB_BOOL, op->record_cap, required_dim);

		// Prepend the filter matrix to algebraic expression as the leftmost operand.
		AlgebraicExpression_MultiplyToTheLeft(&op->ae, op->F);

		// Optimize the expression tree.
		AlgebraicExpression_Optimize(&op->ae);
	}

	// Populate filter matrix.
	_populate_filter_matrix(op);

	// Evaluate expression.
	AlgebraicExpression_Eval(op->ae, op->M);

	if(op->iter == NULL) GxB_MatrixTupleIter_new(&op->iter, RG_MATRIX_M(op->M));
	else GxB_MatrixTupleIter_reuse(op->iter, RG_MATRIX_M(op->M));

	// Clear filter matrix.
	RG_Matrix_clear(op->F);
}

OpBase *NewCondTraverseOp(const ExecutionPlan *plan, Graph *g, AlgebraicExpression *ae) {
	OpCondTraverse *op = rm_malloc(sizeof(OpCondTraverse));
	op->graph = g;
	op->ae = ae;
	op->r = NULL;
	op->iter = NULL;
	op->F = NULL;
	op->M = NULL;
	op->records = NULL;
	op->record_count = 0;
	op->edge_ctx = NULL;
	op->record_cap = BATCH_SIZE;

	// Set our Op operations
	OpBase_Init((OpBase *)op, OPType_CONDITIONAL_TRAVERSE, "Conditional Traverse", CondTraverseInit,
				CondTraverseConsume, CondTraverseReset, CondTraverseToString, CondTraverseClone, CondTraverseFree,
				false, plan);

	bool aware = OpBase_Aware((OpBase *)op, AlgebraicExpression_Src(ae), &op->srcNodeIdx);
	UNUSED(aware);
	ASSERT(aware == true);

	const char *dest = AlgebraicExpression_Dest(ae);
	op->destNodeIdx = OpBase_Modifies((OpBase *)op, dest);

	const char *edge = AlgebraicExpression_Edge(ae);
	if(edge) {
		/* This operation will populate an edge in the Record.
		 * Prepare all necessary information for collecting matching edges. */
		uint edge_idx = OpBase_Modifies((OpBase *)op, edge);
		QGEdge *e = QueryGraph_GetEdgeByAlias(plan->query_graph, edge);
		op->edge_ctx = EdgeTraverseCtx_New(ae, e, edge_idx);
	}

	return (OpBase *)op;
}

static OpResult CondTraverseInit(OpBase *opBase) {
	OpCondTraverse *op = (OpCondTraverse *)opBase;
	// Create 'records' with this Init function as 'record_cap'
	// might be set during optimization time (applyLimit)
	// If cap greater than BATCH_SIZE is specified,
	// use BATCH_SIZE as the value.
	if(op->record_cap > BATCH_SIZE) op->record_cap = BATCH_SIZE;
	op->records = rm_calloc(op->record_cap, sizeof(Record));

	return OP_OK;
}

/* Each call to CondTraverseConsume emits a Record containing the
 * traversal's endpoints and, if required, an edge.
 * Returns NULL once all traversals have been performed. */
static Record CondTraverseConsume(OpBase *opBase) {
	OpCondTraverse *op = (OpCondTraverse *)opBase;
	OpBase *child = op->op.children[0];

	/* If we're required to update an edge and have one queued, we can return early.
	 * Otherwise, try to get a new pair of source and destination nodes. */
	if(op->r         != NULL  &&
	   op->edge_ctx  != NULL  &&
	   EdgeTraverseCtx_SetEdge(op->edge_ctx, op->r)) {
		return OpBase_CloneRecord(op->r);
	}

	bool depleted = true;
	NodeID src_id = INVALID_ENTITY_ID;
	NodeID dest_id = INVALID_ENTITY_ID;

	while(true) {
		if(op->iter) GxB_MatrixTupleIter_next(op->iter, &src_id, &dest_id,
				NULL, &depleted);

		// Managed to get a tuple, break.
		if(!depleted) break;

		/* Run out of tuples, try to get new data.
		 * Free old records. */
		op->r = NULL;
		for(uint i = 0; i < op->record_count; i++) OpBase_DeleteRecord(op->records[i]);

		// Ask child operations for data.
		for(op->record_count = 0; op->record_count < op->record_cap; op->record_count++) {
			Record childRecord = OpBase_Consume(child);
			// If the Record is NULL, the child has been depleted.
			if(!childRecord) break;
			if(!Record_GetNode(childRecord, op->srcNodeIdx)) {
				/* The child Record may not contain the source node in scenarios like
				 * a failed OPTIONAL MATCH. In this case, delete the Record and try again. */
				OpBase_DeleteRecord(childRecord);
				op->record_count--;
				continue;
			}

			// Store received record.
			Record_PersistScalars(childRecord);
			op->records[op->record_count] = childRecord;
		}

		// No data.
		if(op->record_count == 0) return NULL;

		_traverse(op);
	}

	/* Get node from current column. */
	op->r = op->records[src_id];
	// Populate the destination node and add it to the Record.
	Node destNode = GE_NEW_NODE();
	Graph_GetNode(op->graph, dest_id, &destNode);
	Record_AddNode(op->r, op->destNodeIdx, destNode);

	if(op->edge_ctx) {
		Node *srcNode = Record_GetNode(op->r, op->srcNodeIdx);
		// Collect all appropriate edges connecting the current pair of endpoints.
		EdgeTraverseCtx_CollectEdges(op->edge_ctx, ENTITY_GET_ID(srcNode), ENTITY_GET_ID(&destNode));
		// We're guaranteed to have at least one edge.
		EdgeTraverseCtx_SetEdge(op->edge_ctx, op->r);
	}

	return OpBase_CloneRecord(op->r);
}

static OpResult CondTraverseReset(OpBase *ctx) {
	OpCondTraverse *op = (OpCondTraverse *)ctx;

	// Do not explicitly free op->r, as the same pointer is also held
	// in the op->records array and as such will be freed there.
	op->r = NULL;
	for(uint i = 0; i < op->record_count; i++) OpBase_DeleteRecord(op->records[i]);
	op->record_count = 0;

	if(op->edge_ctx) EdgeTraverseCtx_Reset(op->edge_ctx);

	if(op->iter) {
		GxB_MatrixTupleIter_free(&op->iter);
	}
	if(op->F != NULL) RG_Matrix_clear(op->F);
	return OP_OK;
}

static inline OpBase *CondTraverseClone(const ExecutionPlan *plan, const OpBase *opBase) {
	ASSERT(opBase->type == OPType_CONDITIONAL_TRAVERSE);
	OpCondTraverse *op = (OpCondTraverse *)opBase;
	return NewCondTraverseOp(plan, QueryCtx_GetGraph(), AlgebraicExpression_Clone(op->ae));
}

/* Frees CondTraverse */
static void CondTraverseFree(OpBase *ctx) {
	OpCondTraverse *op = (OpCondTraverse *)ctx;

	if(op->iter) {
		GxB_MatrixTupleIter_free(&op->iter);
	}

	if(op->F != NULL) {
		RG_Matrix_free(&op->F);
		op->F = NULL;
	}

	if(op->M != NULL) {
		RG_Matrix_free(&op->M);
		op->M = NULL;
	}

	if(op->ae) {
		AlgebraicExpression_Free(op->ae);
		op->ae = NULL;
	}

	if(op->edge_ctx) {
		EdgeTraverseCtx_Free(op->edge_ctx);
		op->edge_ctx = NULL;
	}

	if(op->records) {
		for(uint i = 0; i < op->record_count; i++) OpBase_DeleteRecord(op->records[i]);
		rm_free(op->records);
		op->records = NULL;
	}
}

