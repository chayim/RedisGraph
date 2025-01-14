//------------------------------------------------------------------------------
// GB_setElement: C(row,col) = scalar
//------------------------------------------------------------------------------

// SuiteSparse:GraphBLAS, Timothy A. Davis, (c) 2017-2021, All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

//------------------------------------------------------------------------------

// Sets the value of single scalar, C(row,col) = scalar, typecasting from the
// type of scalar to the type of C, as needed.  Not user-callable; does the
// work for all GrB_*_setElement* functions.

// If C(row,col) is already present in the matrix, its value is overwritten
// with the scalar.  Otherwise, if the mode determined by GrB_init is
// non-blocking, the tuple (i,j,scalar) is appended to a list of pending tuples
// to C.  GB_wait assembles these pending tuples.

// GrB_setElement is the same as GrB_*assign with an implied SECOND accum
// operator whose ztype, xtype, and ytype are the same as C, with I=i, J=1, a
// 1-by-1 dense matrix A (where nnz (A) == 1), no mask, mask not complemented,
// C_replace effectively false (its value is ignored), and A transpose
// effectively false (since transposing a scalar has no effect).

// Compare this function with GrB_*_extractElement_*

#include "GB_Pending.h"

#define GB_FREE_ALL ;

GrB_Info GB_setElement              // set a single entry, C(row,col) = scalar
(
    GrB_Matrix C,                   // matrix to modify
    void *scalar,                   // scalar to set
    const GrB_Index row,            // row index
    const GrB_Index col,            // column index
    const GB_Type_code scalar_code, // type of the scalar
    GB_Context Context
)
{

    //--------------------------------------------------------------------------
    // check inputs
    //--------------------------------------------------------------------------

    GrB_Info info ;
    ASSERT (C != NULL) ;
    GB_RETURN_IF_NULL (scalar) ;

    if (row >= GB_NROWS (C))
    { 
        GB_ERROR (GrB_INVALID_INDEX,
            "Row index " GBu " out of range; must be < " GBd,
            row, GB_NROWS (C)) ;
    }
    if (col >= GB_NCOLS (C))
    { 
        GB_ERROR (GrB_INVALID_INDEX,
            "Column index " GBu " out of range; must be < " GBd,
            col, GB_NCOLS (C)) ;
    }

    ASSERT (scalar_code <= GB_UDT_code) ;

    GrB_Type ctype = C->type ;
    GB_Type_code ccode = ctype->code ;

    // scalar_code and C must be compatible
    if (!GB_code_compatible (scalar_code, ccode))
    { 
        GB_ERROR (GrB_DOMAIN_MISMATCH,
            "Input scalar of type [%s]\n"
            "cannot be typecast to entry of type [%s]",
            GB_code_string (scalar_code), ctype->name) ;
    }

    // pending tuples and zombies are expected, and C might be jumbled too
    ASSERT (GB_JUMBLED_OK (C)) ;
    ASSERT (GB_PENDING_OK (C)) ;
    ASSERT (GB_ZOMBIES_OK (C)) ;

    //--------------------------------------------------------------------------
    // sort C if needed; do not assemble pending tuples or kill zombies yet
    //--------------------------------------------------------------------------

    if (C->jumbled)
    { 
        GB_OK (GB_wait (C, "C (setElement:jumbled)", Context)) ;
    }

    // zombies and pending tuples are still OK, but C is no longer jumbled
    ASSERT (!GB_JUMBLED (C)) ;
    ASSERT (GB_PENDING_OK (C)) ;
    ASSERT (GB_ZOMBIES_OK (C)) ;

    bool C_is_full = GB_IS_FULL (C) ;

    //--------------------------------------------------------------------------
    // check if C needs to convert to non-iso, or if C is a new iso matrix
    //--------------------------------------------------------------------------

    // stype is the type of this scalar
    GrB_Type stype = GB_code_type (scalar_code, ctype) ;
    size_t csize = ctype->size ;

    if (C->iso)
    {

        //----------------------------------------------------------------------
        // typecast the scalar and compare with the iso value of C
        //----------------------------------------------------------------------

        // s = (ctype) scalar
        bool convert_to_non_iso ;
        if (ctype != stype)
        { 
            // s = (ctype) scalar
            GB_void s [GB_VLA(csize)] ;
            GB_cast_scalar (s, ccode, scalar, scalar_code, csize) ;
            // compare s with the iso value of C
            convert_to_non_iso = (memcmp (C->x, s, csize) != 0) ;
        }
        else
        { 
            // compare the scalar with the iso value of C
            convert_to_non_iso = (memcmp (C->x, scalar, csize) != 0) ;
        }

        if (convert_to_non_iso)
        { 
            // The new entry differs from the iso value of C.  Assemble all
            // pending tuples and convert C to non-iso.  Zombies are OK.
            if (C->Pending != NULL)
            { 
                GB_OK (GB_wait (C, "C (setElement:to non-iso)", Context)) ;
            }
            GB_OK (GB_convert_any_to_non_iso (C, true, Context)) ;
        }

    }
    else if (GB_nnz (C) == 0 && !C_is_full && C->Pending == NULL)
    {

        //----------------------------------------------------------------------
        // C is empty: this is the first setElement, convert C to iso
        //----------------------------------------------------------------------

        // s = (ctype) scalar
        if (ctype != stype)
        { 
            // s = (ctype) scalar
            GB_void s [GB_VLA(csize)] ;
            GB_cast_scalar (s, ccode, scalar, scalar_code, csize) ;
            GB_OK (GB_convert_any_to_iso (C, s, Context)) ;
        }
        else
        { 
            GB_OK (GB_convert_any_to_iso (C, (GB_void *) scalar, Context)) ;
        }
    }

    //--------------------------------------------------------------------------
    // handle the CSR/CSC format
    //--------------------------------------------------------------------------

    int64_t i, j ;
    if (C->is_csc)
    { 
        // set entry with index i in vector j
        i = row ;
        j = col ;
    }
    else
    { 
        // set entry with index j in vector i
        i = col ;
        j = row ;
    }

    int64_t pleft ;
    bool found ;
    bool is_zombie ;
    bool C_is_bitmap = GB_IS_BITMAP (C) ;

    if (C_is_full || C_is_bitmap)
    { 

        //----------------------------------------------------------------------
        // C is bitmap or full
        //----------------------------------------------------------------------

        pleft = i + j * C->vlen ;
        found = true ;
        is_zombie = false ;

    }
    else
    {

        //----------------------------------------------------------------------
        // binary search in C->h for vector j, or O(1)-time lookup if sparse
        //----------------------------------------------------------------------

        int64_t pC_start, pC_end, pright = C->nvec - 1 ;
        pleft = 0 ;
        found = GB_lookup (C->h != NULL, C->h, C->p, C->vlen, &pleft,
            pright, j, &pC_start, &pC_end) ;

        //----------------------------------------------------------------------
        // binary search in kth vector for index i
        //----------------------------------------------------------------------

        if (found)
        { 
            // vector j has been found; now look for index i
            pleft = pC_start ;
            pright = pC_end - 1 ;

            // Time taken for this step is at most O(log(nnz(C(:,j))).
            const int64_t *restrict Ci = C->i ;
            GB_BINARY_SEARCH_ZOMBIE (i, Ci, pleft, pright, found, C->nzombies,
                is_zombie) ;
        }
    }

    //--------------------------------------------------------------------------
    // set the element
    //--------------------------------------------------------------------------

    if (found)
    {

        //----------------------------------------------------------------------
        // C (i,j) found
        //----------------------------------------------------------------------

        // if not zombie: action: ( =A ): copy A into C
        // else           action: ( undelete ): bring a zombie back to life

        if (!C->iso)
        { 
            // typecast or copy the scalar into C(i,j)
            void *cx = ((GB_void *) C->x) + (pleft*csize) ;
            GB_cast_scalar (cx, ccode, scalar, scalar_code, csize) ;
        }

        if (is_zombie)
        { 
            // bring the zombie back to life
            C->i [pleft] = i ;
            C->nzombies-- ;
        }
        else if (C_is_bitmap)
        { 
            // set the entry in the C bitmap
            int8_t cb = C->b [pleft] ;
            C->nvals += (cb == 0) ;
            C->b [pleft] = 1 ;
        }

        return (GrB_SUCCESS) ;

    }
    else
    {

        //----------------------------------------------------------------------
        // C (i,j) not found: add a pending tuple
        //----------------------------------------------------------------------

        // action: ( insert )

        // No typecasting can be done.  The new pending tuple must either be
        // the first pending tuple, or its type must match the prior pending
        // tuples.  See GB_subassign_methods.h for a complete description.

        //----------------------------------------------------------------------
        // check for wait
        //----------------------------------------------------------------------

        bool wait = false ;

        if (C->Pending == NULL)
        { 
            // the new pending tuple is the first one, so it will define
            // C->type-pending = stype.  No need to wait.
            wait = false ;
        }
        else
        {
            if (stype != C->Pending->type)
            { 
                // the scalar type (stype) must match the type of the
                // prior pending tuples.  If the type is different, prior
                // pending tuples must be assembled first.
                wait = true ;
            }
            else if (!GB_op_is_second (C->Pending->op, ctype))
            { 
                // prior op is not SECOND: setElement uses an implicit
                // SECOND_Ctype operator, which must match the operator of the
                // prior pending tuples.  If it doesn't match, prior pending
                // tuples must be assembled first.
                wait = true ;
            }
        }

        if (wait)
        { 

            //------------------------------------------------------------------
            // incompatible pending tuples: wait is required
            //------------------------------------------------------------------

            // Pending tuples exist.  Either the pending operator is not
            // SECOND_ctype (implicit or explicit), or the type of prior
            // pending tuples is not the same as the type of the scalar.  This
            // new tuple requires both conditions to hold.  All prior tuples
            // must be assembled before this new one can be added.
            GB_OK (GB_wait (C, "C (setElement:incompatible pending tuples)",
                Context)) ;

            // repeat the search since the C(i,j) entry may have been in
            // the list of pending tuples.  There are no longer any pending
            // tuples, so this recursion will only happen once.  The
            // pending operator will become the implicit SECOND_ctype,
            // and the type of the pending tuples will become ctype.
            return (GB_setElement (C, scalar, row, col, scalar_code, Context)) ;

        }
        else
        {

            //------------------------------------------------------------------
            // the new tuple is now compatible with prior tuples, if any
            //------------------------------------------------------------------

            ASSERT (GB_PENDING_OK (C)) ;
            ASSERT (GB_ZOMBIES_OK (C)) ;

            // C (i,j) must be added to the list of pending tuples.
            // If this is the first pending tuple, then the type of pending
            // tuples becomes the type of this scalar, and the pending operator
            // becomes NULL, which is the implicit SECOND_ctype operator.

            if (!GB_Pending_add (&(C->Pending), C->iso, (GB_void *) scalar,
                stype, NULL, i, j, C->vdim > 1, Context))
            { 
                // out of memory
                GB_phbix_free (C) ;
                return (GrB_OUT_OF_MEMORY) ;
            }

            ASSERT (GB_PENDING (C)) ;

            // if this was the first tuple, then the pending operator and
            // pending type have been defined
            ASSERT (GB_op_is_second (C->Pending->op, ctype)) ;
            ASSERT (C->Pending->type == stype) ;
            ASSERT (C->Pending->size == stype->size) ;

            // one more pending tuple; block if too many of them
            return (GB_block (C, Context)) ;
        }
    }
}

