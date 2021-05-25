
// Decide branch direction for GPU use for the dot-product MxM
extern "C" 
{
  #include "GB_mxm.h"
}
#include "GB_cuda.h"

bool GB_AxB_dot3_cuda_branch 
(
    const GrB_Matrix M,             // mask matrix
    const bool Mask_struct,         // if true, use the only structure of M
    const GrB_Matrix A,             // input matrix
    const GrB_Matrix B,             // input matrix
    const GrB_Semiring semiring,    // semiring that defines C=A*B
    const bool flipxy,              // if true, do z=fmult(b,a) vs fmult(a,b)
    GB_Context Context
)
{
        // very rough estimate of the work to do
        double adeg = ((double) GB_NNZ (A)) / ((double) GB_IMAX (1, A->nvec)) ;
        double bdeg = ((double) GB_NNZ (B)) / ((double) GB_IMAX (1, B->nvec)) ;
        double work = GB_NNZ (M) * GB_IMIN (adeg, bdeg) ;

        // TODO if A or B are not accessed (first, 2nd, or pair ops)
        // then the type if A can be user-defined here, for CUDA.

        int ngpus_to_use = GB_ngpus_to_use (work) ;
        GBURBLE (" work:%g gpus:%d ", work, ngpus_to_use) ;
        if (ngpus_to_use > 0 && semiring->semiring_is_builtin
            && (A->type->code != GB_UDT_code)
            && (B->type->code != GB_UDT_code))
        {
            return true;
        }
        else
        { 
            return false;
        }

}
