/*
 *
 Copyright (C) 2017 Sarod Yatawatta <sarod@users.sf.net>  
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 $Id$
 */

#include <stdio.h>
#include <string.h>
#include "Dirac.h"


/* 
 * Z = arg min \| Z_k - Z Phi_k\|^2 + \lambda \|Z\|^2 + \mu \|Z\|_1
 * Z : 2*Npoly*N x 2G matrix to be estimated (output)
 * Zbar: each of Z_k (M values) : 2*Npoly*N x 2 (times M)
 * Phikk : sum Phi_k x Phi_k^H + \lambda I : 2G x 2G
 * Phi: each of Phi_K (M values) : 2G x 2 (times M)
 * mu: L1 constraint
 * maxiter: max iterations
 * FISTA: fast iterative shrinkage thresholding Beck&Teboulle 2009
 */
int 
update_spatialreg_fista(complex double *Z, complex double *Zbar, complex double *Phikk,
    complex double *Phi, int N, int M, int Npoly, int G, double mu, int maxiter) {

  /* gradient Z ( Phikk + \lambda I ) - sum_k Z_k Phi_k^H : size 2*Npoly*N x 2*G
   */
  complex double *gradf;
  complex double *Zold,*Y;
  /* Lipschitz constant of gradient, use ||Phikk||^2 as estimate */
  double L=my_cdot(2*G*2*G,Phikk,Phikk);
  /* intial t */
  double t=1.0;
  if ((gradf=(complex double*)calloc((size_t)2*Npoly*N*2*G,sizeof(complex double)))==0) {
      fprintf(stderr,"%s: %d: no free memory\n",__FILE__,__LINE__);
      exit(1);
  }
  if ((Zold=(complex double*)calloc((size_t)2*Npoly*N*2*G,sizeof(complex double)))==0) {
      fprintf(stderr,"%s: %d: no free memory\n",__FILE__,__LINE__);
      exit(1);
  }
  if ((Y=(complex double*)calloc((size_t)2*Npoly*N*2*G,sizeof(complex double)))==0) {
      fprintf(stderr,"%s: %d: no free memory\n",__FILE__,__LINE__);
      exit(1);
  }

   /* reset Z to 0 */
   memset(Z,0,2*Npoly*N*2*G*sizeof(complex double));

   for (int it=0; it<maxiter; it++) {
    /* Zold <- Z */
    memcpy(Zold,Z,2*Npoly*N*2*G*sizeof(complex double));

    /* proximal step using Y instead of Z */
    /* gradf=Z (Phikk +\lambda I) */
    my_zgemm('N','N',2*Npoly*N,2*G,2*G,1.0,Y,2*Npoly*N,Phikk,2*G,0.0,gradf,2*Npoly*N);

    /* gradf = gradf - sum_k Z_k Phi_k^H (each Z_k 2*Npoly*N x 2, each Phi_k 2G x 2) */
    for (int ci=0; ci<M; ci++) {
      my_zgemm('N','C',2*Npoly*N,2*G,2,-1.0,&Zbar[ci*2*Npoly*N*2],2*Npoly*N,&Phi[ci*2*G*2],2*G,1.0,gradf,2*Npoly*N);
    }
    /* take gradient descent step Y - 1/L gradf */
    my_caxpy(2*Npoly*N*2*G, gradf, -1.0/L, Y);
    /* soft threshold and update Z */
    double thresh=t*mu;
    for (int ci=0; ci<2*Npoly*N*2*G; ci++) {
       double r=creal(Y[ci]);
       double r1=fabs(r)-thresh; 
       double mplus=(r1>0.0?r1:0.0);
       double realval=(r>0.0?mplus:-mplus);
       r=cimag(Y[ci]);
       r1=fabs(r)-thresh; 
       mplus=(r1>0.0?r1:0.0);
       double imagval=(r>0.0?mplus:-mplus);
       Z[ci]=realval+_Complex_I*imagval;
       //printf("%lf %lf %lf %lf\n",creal(Y[ci]),cimag(Y[ci]),creal(Z[ci]),cimag(Z[ci]));
    }
    double t0=t;
    t=(1.0+sqrt(1.0+4.0*t*t))*0.5;
    /* update Y = Z + (t-1)/told (Z-Zold) = (1+(t-1)/told) Z - (t-1)/told Zold */
    memcpy(Y,Z,2*Npoly*N*2*G*sizeof(complex double));
    double scalefac=(t-1.0)/t0;
    my_cscal(2*Npoly*N*2*G,1.0+scalefac,Y);
    my_caxpy(2*Npoly*N*2*G, Zold, -scalefac, Y);
    //printf("%lf %lf %lf %lf %lf\n",t,creal(Y[10]),cimag(Y[10]),creal(Z[10]),cimag(Z[10]));
  }

  free(gradf);
  free(Zold);
  free(Y);
  return 0;
}
