//------------------------------------------------------------------------------
// GxB_SelectOp_wait: wait for a user-defined GxB_SelectOp to complete
//------------------------------------------------------------------------------

// SuiteSparse:GraphBLAS, Timothy A. Davis, (c) 2017-2021, All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

//------------------------------------------------------------------------------

// In SuiteSparse:GraphBLAS, a user-defined GxB_SelectOp has no pending
// operations to wait for.  All this method does is verify that the op is
// properly initialized, and then it does an OpenMP flush.

#include "GB.h"

GrB_Info GxB_SelectOp_wait   // no work, just check if the GxB_SelectOp is valid
(
    #if (GxB_IMPLEMENTATION_MAJOR <= 5)
    GxB_SelectOp *op
    #else
    GxB_SelectOp op,
    GrB_WaitMode waitmode
    #endif
)
{ 

    //--------------------------------------------------------------------------
    // check inputs
    //--------------------------------------------------------------------------

    #if (GxB_IMPLEMENTATION_MAJOR <= 5)
    GB_WHERE1 ("GxB_SelectOp_wait (&op)") ;
    GB_RETURN_IF_NULL (op) ;
    GB_RETURN_IF_NULL_OR_FAULTY (*op) ;
    #else
    GB_WHERE1 ("GxB_SelectOp_wait (op, waitmode)") ;
    GB_RETURN_IF_NULL_OR_FAULTY (op) ;
    #endif

    //--------------------------------------------------------------------------
    // return result
    //--------------------------------------------------------------------------

    #pragma omp flush
    return (GrB_SUCCESS) ;
}

