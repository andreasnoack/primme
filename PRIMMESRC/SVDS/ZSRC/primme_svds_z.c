/*******************************************************************************
 *   PRIMME PReconditioned Iterative MultiMethod Eigensolver
 *   Copyright (C) 2015 College of William & Mary,
 *   James R. McCombs, Eloy Romero Alcalde, Andreas Stathopoulos, Lingfei Wu
 *
 *   This file is part of PRIMME.
 *
 *   PRIMME is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   PRIMME is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *******************************************************************************
 * File: primme_svds.c
 *
 * Purpose - front end to svd problems. 
 *
 ******************************************************************************/
 
#include <stdlib.h>   /* free, qsort */
#include <stdio.h>  
#include <string.h>  
#include <math.h>  
#include <assert.h>  
#include "primme_svds.h"
#include "const.h"
#include "wtime.h"
#include "primme_svds_private_z.h"
#include "primme_svds_interface.h"
#include "numerical_z.h"

/*******************************************************************************
 * Subroutine zprimme_svds - This routine is a front end used to perform 
 *    error checking on the input parameters, perform validation, 
 *    and make the call to main_iter. 
 *
 *    Calling zprimme_svds with all svals, svecs, resNorms set to NULL
 *    returns the int and real memory required in the following primme fields:
 *            int primme->intWorkSize : bytes of int workspace needed
 *       long int primme->realWorkSize: bytes of real workspace needed
 * 
 * INPUT/OUTPUT ARRAYS AND PARAMETERS
 * ----------------------------------
 * evals  Contains the converged Ritz values upon return.   Should be of size 
 *        primme->numEvals.
 * 
 * evecs  The local portions of the converged Ritz vectors.  The dimension of
 *        the array is at least primme->nLocal*primme->numEvals
 *
 * resNorms  The residual norms of the converged Ritz vectors.  Should be of 
 *           size primme->numEvals
 *  
 * primme  Structure containing various solver parameters and statistics
 *         See readme.txt for INPUT/OUTPUT variables in primme
 *
 * Return Value
 * ------------
 *  0 - Success
 * -1 - Failure to allocate workspace
 * -2 - Malloc failure in allocating a permutation integer array
 * -3 - main_iter encountered a problem
 * -4 ...-32 - Invalid input (parameters or primme struct) returned 
 *             by check_input()
 * -100...-199 - PRIMME error code from first stage
 * -200...-299 - PRIMME error code from second stage
 *
 ******************************************************************************/

int zprimme_svds(double *svals, Complex_Z *svecs, double *resNorms, 
      primme_svds_params *primme_svds) {

   int ret, allocatedTargetShifts;
   Complex_Z *svecs0;

   /* ------------------ */
   /* Set some defaults  */
   /* ------------------ */
   primme_svds_set_defaults(primme_svds);

   /* -------------------------------------------------------------- */
   /* If needed, we are ready to estimate required memory and return */
   /* -------------------------------------------------------------- */
    if (svals == NULL && svecs == NULL && resNorms == NULL)
       return allocate_workspace_svds(primme_svds, FALSE);

   /* ----------------------------------------------------------- */
   /* Primme_svds_initialize must be called by users unless users */  
   /* specify all parameters in primme_svds structure. Check if   */
   /* primme_svds inputs are good for bounds, correct values etc. */
   /* ----------------------------------------------------------- */
   ret = primme_svds_check_input(svals, svecs, resNorms, primme_svds); 
   if (ret != 0) {
      return(ret);
   }

   /* ----------------------------------------------------------------------- */
   /* Compute AND allocate memory requirements for main_iter and subordinates */
   /* ----------------------------------------------------------------------- */
   ret = allocate_workspace_svds(primme_svds, TRUE);
   if (ret != 0) {
      return ALLOCATE_WORKSPACE_FAILURE;
   }

   /* Execute stage 1 */
   svecs0 = copy_last_params_from_svds(primme_svds, 0, NULL, svecs, NULL,
         &allocatedTargetShifts);
   ret = zprimme(svals, svecs0, resNorms, &primme_svds->primme); 
   copy_last_params_to_svds(primme_svds, 0, svals, svecs, resNorms,
         allocatedTargetShifts);

   if(ret != 0) {
      return ret - 100;
   }
   if (primme_svds->methodStage2 == primme_svds_op_none) {
      return 0;
   }

   /* Execute stage 2 */
   svecs0 = copy_last_params_from_svds(primme_svds, 1, svals, svecs, resNorms,
         &allocatedTargetShifts);
   ret = zprimme(svals, svecs0, resNorms, &primme_svds->primmeStage2);
   copy_last_params_to_svds(primme_svds, 1, svals, svecs, resNorms,
         allocatedTargetShifts);

   if(ret != 0) {
      return ret - 200;
   }
   return 0;
}

static int comp_double(const void *a, const void *b)
{
   return *(double*)a <= *(double*)b ? -1 : 1;
}

static Complex_Z* copy_last_params_from_svds(primme_svds_params *primme_svds, int stage,
      double *svals, Complex_Z *svecs, double *rnorms, int *allocatedTargetShifts) {

   primme_params *primme;
   primme_svds_operator method;
   Complex_Z *aux, *out_svecs = svecs;
   int n, nMax, i, cut;
   const double machEps = Num_dlamch_primme("E");

   primme = stage == 0 ? &primme_svds->primme : &primme_svds->primmeStage2;
   method = stage == 0 ? primme_svds->method : primme_svds->methodStage2;

   *allocatedTargetShifts = 0;

   if (method == primme_svds_op_none) {
      primme->maxMatvecs = 1;
      return NULL;
   }

   if (!primme->matrixMatvec) {
      primme->matrixMatvec = matrixMatvecSVDS;
      primme->matrix = primme_svds;
   }
   if (!primme->applyPreconditioner) {
      primme->applyPreconditioner = applyPreconditionerSVDS;
      primme->preconditioner = primme_svds;
   }

   if (primme_svds->aNorm > 0.0) {
      switch(method) {
      case primme_svds_op_AtA:
      case primme_svds_op_AAt:
         primme->aNorm = primme_svds->aNorm*primme_svds->aNorm;
         break;
      case primme_svds_op_augmented:
         primme->aNorm = primme_svds->aNorm;
         break;
      case primme_svds_op_none:
         break;
      }
   }

   /* Set properly initial vectors. Now svecs = [Uc U0 Vc V0], where          */
   /* Uc, m x numOrthoConst, left constrain vectors;                          */
   /* U0, m x initSize, left initial vectors;                                 */
   /* Vc, n x numOrthoConst, right constrain vectors;                         */
   /* V0, n x numOrthoConst, right initial vectors.                           */

   primme->initSize = primme_svds->initSize;
   primme->numOrthoConst = primme_svds->numOrthoConst;
   n = primme_svds->initSize + primme_svds->numOrthoConst;
   nMax = max(primme_svds->initSize, primme_svds->numSvals) + primme_svds->numOrthoConst;
   switch(method) {
   case primme_svds_op_AtA:
   case primme_svds_op_AAt:
      /* Move Vc V0 to the rightmost position in svecs (aux).
         If using AA', only move Vc */
      aux = &svecs[nMax*primme_svds->mLocal];
      Num_copy_matrix_zprimme(&svecs[primme_svds->mLocal*n], primme_svds->nLocal,
         method == primme_svds_op_AtA ? n : primme_svds->numOrthoConst,
         primme_svds->nLocal, aux, primme_svds->nLocal);
      if (method == primme_svds_op_AtA) out_svecs = aux;
      break;
   case primme_svds_op_augmented:
      /* Shuffle svecs so that svecs = [V; U] */
      assert(primme->nLocal == primme_svds->mLocal+primme_svds->nLocal);
      aux = (Complex_Z *)primme_calloc(primme->nLocal*n, sizeof(Complex_Z), "aux");
      Num_zcopy_zprimme(primme->nLocal*n, svecs, 1, aux, 1);
      Num_copy_matrix_zprimme(&aux[primme_svds->mLocal*n], primme_svds->nLocal,
         n, primme_svds->nLocal, svecs, primme->nLocal);
      Num_copy_matrix_zprimme(aux, primme_svds->mLocal, n, primme_svds->mLocal,
         &svecs[primme_svds->nLocal], primme->nLocal);
      free(aux);
      break;
   case primme_svds_op_none:
      break;
   }
   primme->iseed[0] = primme_svds->iseed[0];
   primme->iseed[1] = primme_svds->iseed[1];
   primme->iseed[2] = primme_svds->iseed[2];
   primme->iseed[3] = primme_svds->iseed[3];
   primme->maxMatvecs = primme_svds->maxMatvecs;

   primme->intWork = primme_svds->intWork;
   primme->intWorkSize = primme_svds->intWorkSize;
   /* If matrixMatvecSVDS is used, it needs extra space to compute A*A' or A'*A */
   if ((primme->matrixMatvec == matrixMatvecSVDS) &&
       (method == primme_svds_op_AtA || method == primme_svds_op_AAt)) {
      cut = primme->maxBlockSize * (method == primme_svds_op_AtA ?
                     primme_svds->mLocal : primme_svds->nLocal);
   }
   else {
      cut = 0;
   }
   primme->realWork = (Complex_Z*)primme_svds->realWork + cut;
   primme->realWorkSize = primme_svds->realWorkSize - cut*sizeof(Complex_Z);
 
   if ((stage == 0 && primme_svds->numTargetShifts > 0) ||
       (stage == 1 && primme->targetShifts == NULL &&
         primme_svds->target == primme_svds_closest_abs)) {
      primme->targetShifts = primme_svds->targetShifts;
      primme->numTargetShifts = primme_svds->numTargetShifts;
      if (stage == 0 &&
            (method == primme_svds_op_AtA || method == primme_svds_op_AAt)) {
         for (i=0; i<primme->numTargetShifts; i++) {
            primme->targetShifts[i] *= primme->targetShifts[i];
         }
      }
   }
   else if (stage == 1 && primme->targetShifts == NULL &&
            primme_svds->target == primme_svds_smallest) {

      assert(method == primme_svds_op_augmented);
      *allocatedTargetShifts = 1;
      primme->targetShifts = (double *)primme_calloc(
         primme_svds->numSvals, sizeof(double), "targetShifts");

      /* primme was configured to find the closest but greater values than */
      /* some shift. The eigensolver is not able to distinguish eigenvalues*/
      /* separated by less than machEps*|A|. The augmented matrix has      */
      /* |m-n| eigenpairs with value zero that don't correspond to         */
      /* singular triplets of A. To avoid to return incorrect triplets set */
      /* shifts not smaller than machEps*|A|.                              */

      for (i=0; i<primme_svds->initSize; i++) {
         primme->targetShifts[i] = max(svals[i]-rnorms[i], primme_svds->aNorm*machEps);
      }
      for ( ; i<primme_svds->numSvals; i++) {
         primme->targetShifts[i] = primme_svds->aNorm*machEps;
      }

      /* Sort the shifts in ascending order */

      qsort(primme->targetShifts, primme_svds->numSvals, sizeof(double),
            comp_double);
      primme->numTargetShifts = primme_svds->numSvals;

   }
   else if (method == primme_svds_op_augmented &&
         primme_svds->target == primme_svds_smallest &&
         primme->targetShifts == NULL) {

      primme->targetShifts = (double *)primme_calloc( 1, sizeof(double),
            "targetShifts");
      *allocatedTargetShifts = 1;
      primme->targetShifts[0] = 0.0;
      primme->numTargetShifts = 1;

   }

   /* Set an initial guess [x; A'x] or [Ax; x] if there is no initial guess   */
   /* and augmented matrix will be used                                       */

   if (method == primme_svds_op_augmented && primme->initSize <= 0) {
      int ONE = 1, TWO = 2, NOTRANS = 0, TRANS = 1;
      double dtmp[2], dtmpo[2], *dtmp0 = dtmpo;
      Complex_Z ztmp, tzero = {+0.0e+00,+0.0e00};
      if (primme_svds->m >= primme_svds->n) {
         Num_larnv_zprimme(2, primme->iseed, primme_svds->mLocal,
               &svecs[primme_svds->nLocal]);
         primme_svds->matrixMatvec(&svecs[primme_svds->nLocal],
               &primme_svds->mLocal, svecs, &primme_svds->nLocal, &ONE, &TRANS,
               primme_svds);
      }
      else {
         Num_larnv_zprimme(2, primme->iseed, primme_svds->nLocal, svecs);
         primme_svds->matrixMatvec(svecs, &primme_svds->nLocal,
               &svecs[primme_svds->nLocal], &primme_svds->mLocal, &ONE,
               &NOTRANS, primme_svds);
      }
      ztmp = Num_dot_zprimme(primme_svds->nLocal, svecs, 1, svecs, 1);
      dtmp[0] = *(double*)&ztmp;
      ztmp = Num_dot_zprimme(primme_svds->mLocal,
            &svecs[primme_svds->nLocal], 1, &svecs[primme_svds->nLocal], 1);
      dtmp[1] = *(double*)&ztmp;
      if (primme_svds->globalSumDouble) {
         primme_svds->globalSumDouble(dtmp, dtmp0, &TWO, primme_svds);
      }
      else dtmp0 = dtmp;
      ztmp = tzero;
      *(double*)&ztmp = 1.0/sqrt(dtmp0[0]);
      Num_scal_zprimme(primme_svds->nLocal, ztmp, svecs, 1);
      *(double*)&ztmp = 1.0/sqrt(dtmp0[1]);
      Num_scal_zprimme(primme_svds->mLocal, ztmp,
            &svecs[primme_svds->nLocal], 1);
      primme->initSize = 1;
      primme->initBasisMode = primme_init_user;
   }

   return out_svecs;
}

/******************************************************************************
 * Function allocate_workspace_svds - This function computes the amount of
 *    integer and real workspace needed by the solver and possibly allocates
 *    the space 
 *
 * Input: 
 *   allocate  If false, no allocation occurs, but the amounts of int and real 
 *                       workspaces in BYTES are returned in the primme fields 
 *                       primme.intWorkSize, and primme.realWorkSize 
 *             If  true, and if the user-provided space is not sufficient,
 *                       allocation is also performed.
 *
 * Output
 *  primme.intWorkSize   Size of integer space allocated in bytes
 *  primme.realWorkSize  Size of real space allocated in bytes (LONG INT)
 *  *(primme.intWork)    Pointer to the integer space allocated
 *  *(primme.realWork)   Pointer to the real space allocated
 *   
 * 
 * Return value
 * ------------
 * int -  0 if (allocate == true) and the given workspaces are large enough or
 *             have been allocated successfully
 *       -1 if (allocate == true) and memory allocation has failed
 *        1 if (allocate==false) 
 *
 ******************************************************************************/

static int allocate_workspace_svds(primme_svds_params *primme_svds, int allocate) {
   primme_params primme;
   int intWorkSize=0;         /* Size of int work space */
   long int realWorkSize=0;   /* Size of real work space */

   /* Require workspace for 1st stage */
   if (primme_svds->method != primme_svds_op_none) {
      primme = primme_svds->primme;
      zprimme(NULL, NULL, NULL, &primme);
      intWorkSize = primme.intWorkSize;
      realWorkSize = primme.realWorkSize;
      /* If matrixMatvecSVDS is used, it needs extra space to compute A*A' or A'*A */
      if ((primme.matrixMatvec == NULL || primme.matrixMatvec == matrixMatvecSVDS) &&
          (primme_svds->method == primme_svds_op_AtA || primme_svds->method == primme_svds_op_AAt))
         realWorkSize += primme.maxBlockSize * sizeof(Complex_Z) *
                           (primme_svds->method == primme_svds_op_AtA ?
                              primme_svds->mLocal : primme_svds->nLocal);
   }

   /* Require workspace for 2st stage */
   if (primme_svds->methodStage2 != primme_svds_op_none) {
      assert(primme_svds->methodStage2 != primme_svds_op_AtA &&
             primme_svds->methodStage2 != primme_svds_op_AAt);
      primme = primme_svds->primmeStage2;
      zprimme(NULL, NULL, NULL, &primme);
      intWorkSize = max(intWorkSize, primme.intWorkSize);
      realWorkSize = max(realWorkSize, primme.realWorkSize);
   }

   if (!allocate) {
      primme_svds->intWorkSize  = intWorkSize;
      primme_svds->realWorkSize = realWorkSize;
      return 1;
   }

   /*----------------------------------------------------------------------*/
   /* Allocate the required workspace, if the user did not provide enough  */
   /*----------------------------------------------------------------------*/
   if (primme_svds->realWorkSize < realWorkSize || primme_svds->realWork == NULL) {
      if (primme_svds->realWork != NULL) {
         free(primme_svds->realWork);
      }
      primme_svds->realWorkSize = realWorkSize;
      primme_svds->realWork = (void *) primme_valloc(realWorkSize,"Real Alloc");
      if (primme_svds->printLevel >= 5) fprintf(primme_svds->outputFile, 
         "Allocating real workspace: %ld bytes\n", primme_svds->realWorkSize);
   }

   if (primme_svds->intWorkSize < intWorkSize || primme_svds->intWork==NULL) {
      if (primme_svds->intWork != NULL) {
         free(primme_svds->intWork);
      }
      primme_svds->intWorkSize = intWorkSize;
      primme_svds->intWork= (int *)primme_valloc(primme_svds->intWorkSize ,"Int Alloc");
      if (primme_svds->printLevel >= 5) fprintf(primme_svds->outputFile, 
         "Allocating integer workspace: %d bytes\n", primme_svds->intWorkSize);
   }

   if (primme_svds->intWork == NULL || primme_svds->realWork == NULL) {
      return MALLOC_FAILURE;
   }
      
   return 0;
}
 
static void copy_last_params_to_svds(primme_svds_params *primme_svds, int stage,
      double *svals, Complex_Z *svecs, double *rnorms, int allocatedTargetShifts) {

   int trans = 1, notrans = 0;
   primme_params *primme;
   primme_svds_operator method;
   Complex_Z *aux, ztmp;
   double *norms;
   int n, nMax, i, cut;

   primme = stage == 0 ? &primme_svds->primme : &primme_svds->primmeStage2;
   method = stage == 0 ? primme_svds->method : primme_svds->methodStage2;

   if (method == primme_svds_op_none) {
      primme->maxMatvecs = 1;
      return;
   }

   /* Record performance measurements */ 
   primme_svds->stats.numOuterIterations += primme->stats.numOuterIterations;
   primme_svds->stats.numRestarts        += primme->stats.numRestarts;
   primme_svds->stats.numMatvecs         += primme->stats.numMatvecs;
   primme_svds->stats.numPreconds        += primme->stats.numPreconds;
   primme_svds->stats.elapsedTime        += primme->stats.elapsedTime;


   if (primme->aNorm > 0.0) {
      switch(method) {
      case primme_svds_op_AtA:
      case primme_svds_op_AAt:
         primme_svds->aNorm = sqrt(primme->aNorm);
         break;
      case primme_svds_op_augmented:
         primme_svds->aNorm = primme->aNorm;
         break;
      case primme_svds_op_none:
         break;
      }
   }

   if (method == primme_svds_op_AtA || method == primme_svds_op_AAt) {
      for (i=0; i<primme->initSize; i++) {
         svals[i] = sqrt(max(0.0, svals[i]));
      }
   }
 
   /* Set svecs = [Uc U Vc V] */
   nMax = max(primme_svds->initSize, primme_svds->numSvals) + primme_svds->numOrthoConst;
   primme_svds->initSize = primme->initSize;
   n = primme_svds->initSize + primme_svds->numOrthoConst;
   switch(method) {
   case primme_svds_op_AtA:
      /* Transform svecs to [Uc A*V/Sigma Vc V] */
      primme_svds->matrixMatvec(
            &svecs[primme_svds->mLocal*nMax+primme->nLocal*primme_svds->numOrthoConst],
            &primme_svds->nLocal, &svecs[primme_svds->mLocal*primme_svds->numOrthoConst],
            &primme_svds->mLocal, &primme_svds->initSize, &notrans, primme_svds);
      Num_scalInv_zmatrix(&svecs[primme_svds->mLocal*primme_svds->numOrthoConst],
            primme_svds->mLocal, primme_svds->initSize, primme_svds->mLocal, svals, primme_svds);
      Num_copy_matrix_zprimme(&svecs[primme_svds->mLocal*nMax], primme_svds->nLocal, n,
            primme_svds->nLocal, &svecs[primme_svds->mLocal*n], primme_svds->nLocal);
      break;
   case primme_svds_op_AAt:
      /* Transform svecs to [Uc U Vc A'*U/Sigma] */
      Num_copy_matrix_zprimme(&svecs[primme_svds->mLocal*nMax], primme_svds->nLocal,
            primme_svds->numOrthoConst, primme_svds->nLocal,
            &svecs[primme_svds->mLocal*n], primme_svds->nLocal);
      primme_svds->matrixMatvec(
            &svecs[primme_svds->mLocal*primme_svds->numOrthoConst], &primme_svds->mLocal,
            &svecs[primme_svds->mLocal*n+primme->nLocal*primme_svds->numOrthoConst],
            &primme_svds->nLocal, &primme_svds->initSize, &trans, primme_svds);
      Num_scalInv_zmatrix(
            &svecs[primme_svds->mLocal*n+primme->nLocal*primme_svds->numOrthoConst],
            primme_svds->nLocal, primme_svds->initSize, primme_svds->nLocal, svals, primme_svds);
      break;
   case primme_svds_op_augmented:
      assert(primme->nLocal == primme_svds->mLocal+primme_svds->nLocal);

      /* Shuffle svecs from [Vc V; Uc U] to [Uc U Vc V] */
      aux = (Complex_Z *)primme_calloc(primme->nLocal*n, sizeof(Complex_Z), "aux");
      Num_zcopy_zprimme(primme->nLocal*n, svecs, 1, aux, 1);
      Num_copy_matrix_zprimme(aux, primme_svds->nLocal, n, primme->nLocal,
         &svecs[primme_svds->mLocal*n], primme_svds->nLocal);
      Num_copy_matrix_zprimme(&aux[primme_svds->nLocal], primme_svds->mLocal, n,
         primme->nLocal, svecs, primme_svds->mLocal);

      /* Normalize every column in U and V */
      for (i=0; i<n; i++) {
         ztmp = Num_dot_zprimme(primme_svds->mLocal, &svecs[primme_svds->mLocal*i], 1,
               &svecs[primme_svds->mLocal*i], 1);
         ((double*)aux)[i] = *(double*)&ztmp;
      }
      for (i=0; i<n; i++) {
         ztmp = Num_dot_zprimme(primme_svds->nLocal,
               &svecs[primme_svds->mLocal*n+primme_svds->nLocal*i], 1,
               &svecs[primme_svds->mLocal*n+primme_svds->nLocal*i], 1);
         ((double*)aux)[n+i] = *(double*)&ztmp;
      }
      if (primme_svds->globalSumDouble) {
         int count = 2*n;
         norms = (double*)aux+2*n;
         primme_svds->globalSumDouble((double*)aux, norms, &count, primme_svds);
      }
      else norms = (double*)aux;
      for (i=0; i<n; i++) {
         Complex_Z ztmp0 = {+0.0e+00,+0.0e00};
         *(double*)&ztmp0 = 1.0/sqrt(norms[i]);
         Num_scal_zprimme(primme_svds->mLocal, ztmp0, &svecs[primme_svds->mLocal*i], 1);
      }
      for (i=0; i<n; i++) {
         Complex_Z ztmp0 = {+0.0e+00,+0.0e00};
         *(double*)&ztmp0 = 1.0/sqrt(norms[n+i]);
         Num_scal_zprimme(primme_svds->nLocal, ztmp0,
               &svecs[primme_svds->mLocal*n+primme_svds->nLocal*i], 1);
      }
      free(aux);
      break;
   case primme_svds_op_none:
      break;
   }

   primme_svds->iseed[0] = primme->iseed[0];
   primme_svds->iseed[1] = primme->iseed[1];
   primme_svds->iseed[2] = primme->iseed[2];
   primme_svds->iseed[3] = primme->iseed[3];
   primme_svds->maxMatvecs -= primme->stats.numMatvecs;


   /* Check that primme didn't free the workspaces */
   if ((primme->matrixMatvec == matrixMatvecSVDS) &&
       (method == primme_svds_op_AtA || method == primme_svds_op_AAt)) {
      cut = primme->maxBlockSize * (method == primme_svds_op_AtA ?
                     primme_svds->mLocal : primme_svds->nLocal);
   }
   else {
      cut = 0;
   }
   assert(primme_svds->intWork == primme->intWork);
   assert((Complex_Z*)primme_svds->realWork + cut == primme->realWork);

   /* Zero references to primme workspaces to prevent to be release by primme_Free */
   primme->intWork = NULL;
   primme->realWork = NULL;

   if (stage == 0 && primme_svds->targetShifts == primme->targetShifts &&
       (method == primme_svds_op_AtA || method == primme_svds_op_AAt)) {
      for (i=0; i<primme_svds->numTargetShifts; i++) {
         primme_svds->targetShifts[i] = sqrt(primme_svds->targetShifts[i]);
      }
   }

   if (allocatedTargetShifts) {
      free(primme->targetShifts);
      primme->targetShifts = NULL;
   }

   /* Update residual norms when final stage */
   if (primme_svds->methodStage2 != primme_svds_op_none) {
      switch(method) {
      case primme_svds_op_AtA:
      case primme_svds_op_AAt:
         for (i=0; i<primme_svds->initSize; i++) {
            rnorms[i] = min(rnorms[i]/svals[i], primme_svds->aNorm);
         }
         break;
      case primme_svds_op_augmented:
         for (i=0; i<primme_svds->initSize; i++) {
            rnorms[i] *= sqrt(2.0);
         }
         break;
      case primme_svds_op_none:
         break;
      }
   }
}

/******************************************************************************
 *
 * static int primme_svds_check_input(double *svals, Complex_Z *svecs, double *resNorms, 
 *                        primme_svds_params *primme_svds) 
 *
 * INPUT
 * -----
 *  svals, svecs, resNorms   Output arrays for primme
 *  primme_svds              the main structure of parameters 
 *
 * return value -   0    If input parameters in primme are appropriate
 *              -4..-19  Inappropriate input parameters were found
 *
 ******************************************************************************/
static int primme_svds_check_input(double *svals, Complex_Z *svecs, double *resNorms, 
      primme_svds_params *primme_svds) {
   int ret;
   ret = 0;

   if (primme_svds == NULL)
      ret = -4;
   else if (primme_svds->n <= 0 || primme_svds->m <= 0) 
      ret = -5;
   else if (primme_svds->numProcs < 1)
      ret = -6;
   else if (primme_svds->matrixMatvec == NULL) 
      ret = -7;
   else if (primme_svds->applyPreconditioner == NULL && 
         primme_svds->precondition == 1) 
      ret = -8;
   else if (primme_svds->numProcs >1 && primme_svds->globalSumDouble == NULL)
      ret = -9;
   else if (primme_svds->numSvals > min(primme_svds->n, primme_svds->m))
      ret = -10;
   else if (primme_svds->numSvals < 1)
      ret = -11;
   else if ( primme_svds->target != primme_svds_smallest  &&
             primme_svds->target != primme_svds_largest   &&
             primme_svds->target != primme_svds_closest_abs)
      ret = -13;
   else if ( primme_svds->method != primme_svds_op_AtA &&
             primme_svds->method != primme_svds_op_AAt &&
             primme_svds->method != primme_svds_op_augmented)
      ret = -14;
   else if ( (primme_svds->method == primme_svds_op_augmented &&
              primme_svds->methodStage2 != primme_svds_op_none) ||
             (primme_svds->method != primme_svds_op_augmented &&
              primme_svds->methodStage2 != primme_svds_op_augmented &&
              primme_svds->methodStage2 != primme_svds_op_none))
      ret = -15;
   else if (primme_svds->printLevel < 0 || primme_svds->printLevel > 5)
      ret = -16; 
   else if (svals == NULL)
      ret = -17;
   else if (svecs == NULL)
      ret = -18;
   else if (resNorms == NULL)
      ret = -19;

   return ret;
   /***************************************************************************/
} /* end of check_input
   ***************************************************************************/

/**********************************************************************************
 * void MatrixATA_Matvec(void *x, void *y, int *blockSize, primme_params *primme) *
 **********************************************************************************/
static void matrixMatvecSVDS(void *x_, void *y_, int *blockSize, primme_params *primme) {
   primme_svds_params *primme_svds = (primme_svds_params *) primme->matrix;
   int trans = 1, notrans = 0;
   Complex_Z *x = (Complex_Z*)x_, *y = (Complex_Z*)y_;
   primme_svds_operator method = &primme_svds->primme == primme ?
      primme_svds->method : primme_svds->methodStage2;
   int i, bs;

   switch(method) {
   case primme_svds_op_AtA:
      for (i=0, bs=min((*blockSize-i), primme->maxBlockSize); bs>0;
               i+= bs, bs=min((*blockSize-i), primme->maxBlockSize))
      {
         primme_svds->matrixMatvec(&x[primme->nLocal*i], &primme->nLocal,
            primme_svds->realWork, &primme_svds->mLocal, &bs, &notrans, primme_svds);
         primme_svds->matrixMatvec(primme_svds->realWork, &primme_svds->mLocal,
            &y[i*primme->nLocal], &primme->nLocal, &bs, &trans, primme_svds);
      }
      break;
   case primme_svds_op_AAt:
      for (i=0, bs=min((*blockSize-i), primme->maxBlockSize); bs>0;
               i+= bs, bs=min((*blockSize-i), primme->maxBlockSize))
      {
         primme_svds->matrixMatvec(&x[primme->nLocal*i], &primme->nLocal,
            primme_svds->realWork, &primme_svds->nLocal, &bs, &trans, primme_svds);
         primme_svds->matrixMatvec(primme_svds->realWork, &primme_svds->nLocal,
            &y[i*primme->nLocal], &primme->nLocal, &bs, &notrans, primme_svds);
      }
      break;
   case primme_svds_op_augmented:
      primme_svds->matrixMatvec(&x[primme_svds->nLocal], &primme->nLocal, y,
         &primme->nLocal, blockSize, &trans, primme_svds);
      primme_svds->matrixMatvec(x, &primme->nLocal, &y[primme_svds->nLocal],
         &primme->nLocal, blockSize, &notrans, primme_svds);
      break;
   case primme_svds_op_none:
      break;
   }
}

static void applyPreconditionerSVDS(void *x, void *y, int *blockSize, primme_params *primme) {
   primme_svds_params *primme_svds = (primme_svds_params *) primme->preconditioner;
   int method = (int)(&primme_svds->primme == primme ?
                        primme_svds->method : primme_svds->methodStage2);

   primme_svds->applyPreconditioner(x, &primme->nLocal, y,
      &primme->nLocal, blockSize, &method, primme_svds);
}

static void Num_scalInv_zmatrix(Complex_Z *x, int m, int n, int ldx, double *factors,
                                       primme_svds_params *primme_svds) {
   int i, ONE=1;
   Complex_Z ztmp;
   double norm, norm0;

   assert(ldx >= m);
   for (i=0; i<n; i++) {
      if (factors[i] > 0 && 1.0L/factors[i] < HUGE_VAL) {
         ztmp.r = 1.0L/factors[i]; ztmp.i = 0.0L;
      }
      else {
         ztmp = Num_dot_zprimme(m, &x[i*ldx], 1, &x[i*ldx], 1);
         norm0 = ztmp.r;
         if (primme_svds->globalSumDouble) {
            primme_svds->globalSumDouble(&norm0, &norm, &ONE, primme_svds);
         }
         else norm = norm0;
         ztmp.r = 1.0L/sqrt(norm); ztmp.i = 0.0L;
      }
      Num_scal_zprimme(m, ztmp, &x[i*ldx], 1);
   }
}
