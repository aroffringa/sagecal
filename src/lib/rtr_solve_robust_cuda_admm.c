/*
 *
 Copyright (C) 2006-2008 Sarod Yatawatta <sarod@users.sf.net>  
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
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#include "sagecal.h"
#include <cuda_runtime.h>

//#define DEBUG
/* helper functions for diagnostics */
static void
checkStatus(culaStatus status, char *file, int line)
{
    char buf[80];
    if(!status)
        return;
    culaGetErrorInfoString(status, culaGetErrorInfo(), buf, sizeof(buf));
    fprintf(stderr,"GPU (CULA): %s %s %d\n", buf,file,line);
    culaShutdown();
    exit(EXIT_FAILURE);
}


static void
checkCudaError(cudaError_t err, char *file, int line)
{
    if(!err)
        return;
    fprintf(stderr,"GPU (CUDA): %s %s %d\n", cudaGetErrorString(err),file,line);
    culaShutdown();
    exit(EXIT_FAILURE);
}


static void
checkCublasError(cublasStatus_t cbstatus, char *file, int line)
{
   if (cbstatus!=CUBLAS_STATUS_SUCCESS) {
    fprintf(stderr,"%s: %d: CUBLAS failure\n",file,line);
    exit(EXIT_FAILURE);  
   }
}


/* cost function */
static float
cudakernel_fns_f_robust_admm(int ThreadsPerBlock, int BlocksPerGrid, int N, int M, cuFloatComplex *x, cuFloatComplex *Y, cuFloatComplex *Z, float admm_rho, float *y, float *coh, char *bbh,  float *wtd, cublasHandle_t cbhandle, float *gWORK){
 cuFloatComplex *Yd;
 cublasStatus_t cbstatus;
 cuFloatComplex alpha,a;
 unsigned long int moff=0;
 Yd=(cuFloatComplex*)&gWORK[moff];
 moff+=8*N;
 /* original cost function */
 float f0=cudakernel_fns_f_robust(ThreadsPerBlock,BlocksPerGrid,N,M,x,y,coh,bbh,wtd,gWORK);
#ifdef DEBUG
 printf("orig cost %f ",f0);
#endif
 /* extra cost from ADMM */ 
 /* add ||Y^H(J-BZ)|| + rho/2 ||J-BZ||^2 */

 /* Yd=J-BZ */
 cublasCcopy(cbhandle,4*N,x,1,Yd,1);
 alpha.x=-1.0f;alpha.y=0.0f;
 cbstatus=cublasCaxpy(cbhandle,4*N, &alpha, Z, 1, Yd, 1);

 /* ||Y^H Yd|| = 2 real(Y(:)^H Yd(:)) */
 cbstatus=cublasCdotc(cbhandle,4*N, Y, 1, Yd, 1, &a);
#ifdef DEBUG
 printf("up %f ",2.0f*a.x);
#endif
 f0+=2.0f*a.x;

 /* rho/2 ||J-BZ||^2  = rho/2 real(Yd(:)^H Yd(:)) */
 cbstatus=cublasCdotc(cbhandle,4*N, Yd, 1, Yd, 1, &a);
#ifdef DEBUG
 printf("up %f\n",0.5f*admm_rho*a.x);
#endif
 f0+=0.5f*admm_rho*a.x;
 return f0;
}

/* Projection 
   rnew: new value : Euclidean space, just old value */
static void
cudakernel_fns_proj_admm(int N, cuFloatComplex *x, cuFloatComplex *z, cuFloatComplex *rnew, cublasHandle_t cbhandle) {
 cublasStatus_t cbstatus;

 cbstatus=cublasCcopy(cbhandle,4*N,z,1,rnew,1);
 checkCublasError(cbstatus,__FILE__,__LINE__);
}


/* gradient, also projected to tangent space */
/* need 8N*BlocksPerGrid+ 8N*2 float storage */
static void
cudakernel_fns_fgrad_robust_admm(int ThreadsPerBlock, int BlocksPerGrid, int N, int M, cuFloatComplex *x, cuFloatComplex *Y, cuFloatComplex *Z, float admm_rho, cuFloatComplex *eta, float *y, float *coh, char *bbh, float *iw, float *wtd, int negate, cublasHandle_t cbhandle,float *gWORK) {

 cuFloatComplex *tempeta;
 cublasStatus_t cbstatus;
 cuFloatComplex alpha;
 unsigned long int moff=0;
 tempeta=(cuFloatComplex*)&gWORK[moff];
 moff+=8*N;
 float *gWORK1=&gWORK[moff];

 /*************************/
 /* baselines */
 int nbase=N*(N-1)/2;
 /* timeslots */
 int ntime=(M+nbase-1)/nbase;
 /* blocks per timeslot */
 /* total blocks is Bt x ntime */
 int Bt=(nbase+ThreadsPerBlock-1)/ThreadsPerBlock;
 /* max size of M for one kernel call, to determine optimal blocks */
 cudakernel_fns_fgradflat_robust_admm(ThreadsPerBlock, Bt*ntime, N, M, x, tempeta, y, coh, bbh, wtd, cbhandle, gWORK1);

 /* weight for missing (flagged) baselines */
 cudakernel_fns_fscale(N, tempeta, iw);
 /* find -ve gradient */
 if (negate) {
  alpha.x=-1.0f;alpha.y=0.0f;
  cbstatus=cublasCscal(cbhandle,4*N,&alpha,tempeta,1);
 }

#ifdef DEBUG
 /******************************/
 /* print norms , use eta as temp storage */
 float n1,n2,n3;
 cublasScnrm2(cbhandle,4*N,tempeta,1,&n1);
 cublasScnrm2(cbhandle,4*N,Y,1,&n2);
 cudaMemcpy(eta,x,4*N*sizeof(cuFloatComplex),cudaMemcpyDeviceToDevice);
 alpha.x=-1.0f; alpha.y=0.0f;
 cublasCaxpy(cbhandle,4*N, &alpha, Z, 1, eta, 1);
 cublasScnrm2(cbhandle,4*N,eta,1,&n3);
 printf("Norm %lf %lf %lf\n",n1,0.5f*n2,0.5f*admm_rho*n3);
 /******************************/
#endif

 /* extra terms  0.5*Y+0.5*rho*(J-BZ)
   add to -ve grad */
 if (negate) {
  alpha.x=0.5f; alpha.y=0.0f;
  cbstatus=cublasCaxpy(cbhandle,4*N, &alpha, Y, 1, tempeta, 1);
  alpha.x=0.5f*admm_rho; alpha.y=0.0f;
  cbstatus=cublasCaxpy(cbhandle,4*N, &alpha, x, 1, tempeta, 1);
  alpha.x=-0.5f*admm_rho; alpha.y=0.0f;
  cbstatus=cublasCaxpy(cbhandle,4*N, &alpha, Z, 1, tempeta, 1);
 } else {
  alpha.x=-0.5f; alpha.y=0.0f;
  cbstatus=cublasCaxpy(cbhandle,4*N, &alpha, Y, 1, tempeta, 1);
  alpha.x=-0.5f*admm_rho; alpha.y=0.0f;
  cbstatus=cublasCaxpy(cbhandle,4*N, &alpha, x, 1, tempeta, 1);
  alpha.x=0.5f*admm_rho; alpha.y=0.0f;
  cbstatus=cublasCaxpy(cbhandle,4*N, &alpha, Z, 1, tempeta, 1);
 }

 checkCublasError(cbstatus,__FILE__,__LINE__);
 cudaMemcpy(eta,tempeta,4*N*sizeof(cuFloatComplex),cudaMemcpyDeviceToDevice);

}

/* Hessian, also projected to tangent space */
/* need 8N*BlocksPerGrid+ 8N*2 float storage */
static void
cudakernel_fns_fhess_robust_admm(int ThreadsPerBlock, int BlocksPerGrid, int N, int M, cuFloatComplex *x,  cuFloatComplex *Y, cuFloatComplex *Z, float admm_rho, cuFloatComplex *eta, cuFloatComplex *fhess, float *y, float *coh, char *bbh, float *iw, float *wtd, cublasHandle_t cbhandle, float *gWORK) {
 cuFloatComplex *tempeta;
 cublasStatus_t cbstatus;
 unsigned long int moff=0;
 tempeta=(cuFloatComplex*)&gWORK[moff];
 moff+=8*N;
 float *gWORK1=&gWORK[moff];

 /* baselines */
 int nbase=N*(N-1)/2;
 /* timeslots */
 int ntime=(M+nbase-1)/nbase;
 /* blocks per timeslot */
 /* total blocks is Bt x ntime */
 int Bt=(nbase+ThreadsPerBlock-1)/ThreadsPerBlock;

 cudakernel_fns_fhessflat_robust_admm(ThreadsPerBlock, Bt*ntime, N, M, x, eta, tempeta, y, coh, bbh, wtd, cbhandle, gWORK1);

 cudakernel_fns_fscale(N, tempeta, iw);
 
 /* extra terms 0.5*rho*eta*/
 cuFloatComplex alpha;
 alpha.x=0.5f*admm_rho;alpha.y=0.0f;
 cbstatus=cublasCaxpy(cbhandle,4*N, &alpha, eta, 1, tempeta, 1);

 cudaMemcpy(fhess,tempeta,4*N*sizeof(cuFloatComplex),cudaMemcpyDeviceToDevice);
}

/* Armijo step calculation,
  output teta: Armijo gradient 
  return value: 0 : cost reduced, 1: no cost reduction, so do not run again 
  mincost: min value of cost, is possible
*/
/* need 8N*BlocksPerGrid+ 8N*2 float storage */
static int
armijostep(int ThreadsPerBlock, int BlocksPerGrid, int N, int M, cuFloatComplex *x,  cuFloatComplex *Y, cuFloatComplex *Z, float admm_rho, cuFloatComplex *teta, float *y, float *coh, char *bbh, float *iw, float *wtd, float *mincost, cublasHandle_t cbhandle, float *gWORK) {
 float alphabar=10.0f;
 float beta=0.2f;
 float sigma=0.5f;
 cublasStatus_t cbstatus;
 /* temp storage, re-using global storage */
 cuFloatComplex *eta, *x_prop;
 unsigned long int moff=0;
 eta=(cuFloatComplex*)&gWORK[moff];
 moff+=8*N;
 x_prop=(cuFloatComplex*)&gWORK[moff];
 moff+=8*N;
 float *gWORK1=&gWORK[moff];

 float fx=cudakernel_fns_f_robust_admm(ThreadsPerBlock,BlocksPerGrid,N,M,x,Y,Z,admm_rho,y,coh,bbh,wtd,cbhandle,gWORK1);
 cudakernel_fns_fgrad_robust_admm(ThreadsPerBlock,BlocksPerGrid,N,M,x,Y,Z,admm_rho,eta,y,coh,bbh,iw,wtd, 0,cbhandle, gWORK1);
 float beta0=beta;
 float minfx=fx; float minbeta=beta0;
 float lhs,rhs,metric;
 int m,nocostred=0;
 cuFloatComplex alpha;
 *mincost=fx;

 float metric0=cudakernel_fns_g(N,x,eta,eta,cbhandle);
 for (m=0; m<50; m++) {
   cbstatus=cublasCcopy(cbhandle,4*N,eta,1,teta,1);
   alpha.x=beta0*alphabar;alpha.y=0.0f;
   cbstatus=cublasCscal(cbhandle,4*N,&alpha,teta,1);
   cudakernel_fns_R(N,x,teta,x_prop,cbhandle);
   lhs=cudakernel_fns_f_robust_admm(ThreadsPerBlock,BlocksPerGrid,N,M,x_prop,Y,Z,admm_rho,y,coh,bbh,wtd,cbhandle,gWORK1);
   if (lhs<minfx) {
     minfx=lhs;
     *mincost=minfx;
     minbeta=beta0;
   }
   metric=beta0*alphabar*metric0;
   rhs=fx+sigma*metric;
   if ((!isnan(lhs) && lhs<=rhs)) {
    minbeta=beta0;
    break;
   }
   beta0=beta0*beta;
 }

 /* if no further cost improvement is seen */
 if (lhs>fx) {
     nocostred=1;
 }

 cbstatus=cublasCcopy(cbhandle,4*N,eta,1,teta,1);
 alpha.x=minbeta*alphabar; alpha.y=0.0f;
 cbstatus=cublasCscal(cbhandle,4*N,&alpha,teta,1);

 return nocostred;
}


/* truncated conjugate gradient method 
  x, grad, eta, r, z, delta, Hxd  : size 2N x 2  complex 
  so, vector size is 4N complex double

  output: eta
  return value: stop_tCG code   

  y: vec(V) visibilities
*/
/* need 8N*(BlocksPerGrid+2)+ 8N*6 float storage */
static int
tcg_solve_cuda(int ThreadsPerBlock, int BlocksPerGrid, int N, int M, cuFloatComplex *x,  cuFloatComplex *Y, cuFloatComplex *Z, float admm_rho, cuFloatComplex *grad, cuFloatComplex *eta, cuFloatComplex *fhess, float Delta, float theta, float kappa, int max_inner, int min_inner, float *y, float *coh, char *bbh, float *iw, float *wtd, cublasHandle_t cbhandle, float *gWORK) { 
  cuFloatComplex *r,*z,*delta,*Hxd, *rnew;
  float  e_Pe, r_r, norm_r, z_r, d_Pd, d_Hd, alpha, e_Pe_new,
     e_Pd, Deltasq, tau, zold_rold, beta, norm_r0;
  int cj, stop_tCG;
  unsigned long int moff=0;
  r=(cuFloatComplex*)&gWORK[moff];
  moff+=8*N;
  z=(cuFloatComplex*)&gWORK[moff];
  moff+=8*N;
  delta=(cuFloatComplex*)&gWORK[moff];
  moff+=8*N;
  Hxd=(cuFloatComplex*)&gWORK[moff];
  moff+=8*N;
  rnew=(cuFloatComplex*)&gWORK[moff];
  moff+=8*N;
  float *gWORK1=&gWORK[moff];

  cublasStatus_t cbstatus;
  cuFloatComplex a0;

  /*
  initial values
  */
  cbstatus=cublasCcopy(cbhandle,4*N,grad,1,r,1);
  e_Pe=0.0f;


  r_r=cudakernel_fns_g(N,x,r,r,cbhandle);
  norm_r=sqrtf(r_r);
  norm_r0=norm_r;

  cbstatus=cublasCcopy(cbhandle,4*N,r,1,z,1);

  z_r=cudakernel_fns_g(N,x,z,r,cbhandle);
  d_Pd=z_r;

  /*
   initial search direction
  */
  cudaMemset(delta, 0, sizeof(cuFloatComplex)*4*N); 
  a0.x=-1.0f; a0.y=0.0f;
  cbstatus=cublasCaxpy(cbhandle,4*N, &a0, z, 1, delta, 1);
  e_Pd=cudakernel_fns_g(N,x,eta,delta,cbhandle);

  stop_tCG=5;

  /* % begin inner/tCG loop
    for j = 1:max_inner,
  */
  for(cj=1; cj<=max_inner; cj++) {
    cudakernel_fns_fhess_robust_admm(ThreadsPerBlock,BlocksPerGrid,N,M,x,Y,Z,admm_rho,delta,Hxd,y,coh,bbh,iw,wtd,cbhandle, gWORK1);
    d_Hd=cudakernel_fns_g(N,x,delta,Hxd,cbhandle);

    alpha=z_r/d_Hd;
    e_Pe_new = e_Pe + 2.0f*alpha*e_Pd + alpha*alpha*d_Pd;


    Deltasq=Delta*Delta;
    if (d_Hd <= 0.0f || e_Pe_new >= Deltasq) {
      tau = (-e_Pd + sqrtf(e_Pd*e_Pd + d_Pd*(Deltasq-e_Pe)))/d_Pd;
      a0.x=tau;
      cbstatus=cublasCaxpy(cbhandle,4*N, &a0, delta, 1, eta, 1);
      /* Heta = Heta + tau *Hdelta */
      cbstatus=cublasCaxpy(cbhandle,4*N, &a0, Hxd, 1, fhess, 1);
      stop_tCG=(d_Hd<=0.0f?1:2);
      break;
    }

    e_Pe=e_Pe_new;
    a0.x=alpha;
    cbstatus=cublasCaxpy(cbhandle,4*N, &a0, delta, 1, eta, 1);
    /* Heta = Heta + alpha*Hdelta */
    cbstatus=cublasCaxpy(cbhandle,4*N, &a0, Hxd, 1, fhess, 1);
    
    cbstatus=cublasCaxpy(cbhandle,4*N, &a0, Hxd, 1, r, 1);
    cudakernel_fns_proj_admm(N, x, r, rnew, cbhandle);
    cbstatus=cublasCcopy(cbhandle,4*N,rnew,1,r,1);
    r_r=cudakernel_fns_g(N,x,r,r,cbhandle);
    norm_r=sqrtf(r_r);

    /*
      check kappa/theta stopping criterion
    */
    if (cj >= min_inner) {
      float norm_r0pow=powf(norm_r0,theta);
      if (norm_r <= norm_r0*MIN(norm_r0pow,kappa)) {
       stop_tCG=(kappa<norm_r0pow?3:4);
       break;
      }
    }

    cbstatus=cublasCcopy(cbhandle,4*N,r,1,z,1);
    zold_rold=z_r;

    z_r=cudakernel_fns_g(N,x,z,r,cbhandle);

    beta=z_r/zold_rold;
    a0.x=beta; 
    cbstatus=cublasCscal(cbhandle,4*N,&a0,delta,1);
    a0.x=-1.0f; 
    cbstatus=cublasCaxpy(cbhandle,4*N, &a0, z, 1, delta, 1);


    e_Pd = beta*(e_Pd + alpha*d_Pd);
    d_Pd = z_r + beta*beta*d_Pd;
  }

  return stop_tCG;
}


int
rtr_solve_cuda_robust_admm_fl(
  float *x0,         /* initial values and updated solution at output (size 8*N float) */
  float *Y, /* Lagrange multiplier size 8N */
  float *Z, /* consensus term B Z  size 8N */
  float *y,         /* data vector (size 8*M float) */
  int N,              /* no of stations */
  int M,              /* no of constraints */
  int itmax_sd,          /* maximum number of iterations RSD */
  int itmax_rtr,          /* maximum number of iterations RTR */
  float Delta_bar, float Delta0, /* Trust region radius and initial value */
  float admm_rho, /* ADMM regularization */
  double robust_nulow, double robust_nuhigh, /* robust nu range */
  double *info, /* initial and final residuals */

  cublasHandle_t cbhandle, /* device handle */
  float *gWORK, /* GPU allocated memory */
  int tileoff, /* tile offset when solving for many chunks */
  int ntiles, /* total tile (data) size being solved for */
  me_data_t *adata)
{

  /* general note: all device variables end with a 'd' */
  cudaError_t err;
  cublasStatus_t cbstatus;

  /* ME data */
  me_data_t *dp=(me_data_t*)adata;
  int Nbase=(dp->Nbase)*(ntiles); /* note: we do not use the total tile size */
  /* coherency on device */
  float *cohd;
  /* baseline-station map on device/host */
  char *bbd;

  /* calculate no of cuda threads and blocks */
  int ThreadsPerBlock=128;
  int BlocksPerGrid=(M+ThreadsPerBlock-1)/ThreadsPerBlock;


  /* reshape x to make J: 2Nx2 complex double 
  */
  complex float *x;
  if ((x=(complex float*)malloc((size_t)4*N*sizeof(complex float)))==0) {
#ifndef USE_MIC
   fprintf(stderr,"%s: %d: No free memory\n",__FILE__,__LINE__);
#endif
   exit(1);
  }
  /* map x: [(re,im)J_1(0,0) (re,im)J_1(0,1) (re,im)J_1(1,0) (re,im)J_1(1,1)...]
   to
  J: [J_1(0,0) J_1(1,0) J_2(0,0) J_2(1,0) ..... J_1(0,1) J_1(1,1) J_2(0,1) J_2(1,1)....]
 */
  float *Jd=(float*)x;
  /* re J(0,0) */
  my_fcopy(N, &x0[0], 8, &Jd[0], 4);
  /* im J(0,0) */
  my_fcopy(N, &x0[1], 8, &Jd[1], 4);
  /* re J(1,0) */
  my_fcopy(N, &x0[4], 8, &Jd[2], 4);
  /* im J(1,0) */
  my_fcopy(N, &x0[5], 8, &Jd[3], 4);
  /* re J(0,1) */
  my_fcopy(N, &x0[2], 8, &Jd[4*N], 4);
  /* im J(0,1) */
  my_fcopy(N, &x0[3], 8, &Jd[4*N+1], 4);
  /* re J(1,1) */
  my_fcopy(N, &x0[6], 8, &Jd[4*N+2], 4);
  /* im J(1,1) */
  my_fcopy(N, &x0[7], 8, &Jd[4*N+3], 4);

  complex float *Zx,*Yx;
  if ((Zx=(complex float*)malloc((size_t)4*N*sizeof(complex float)))==0) {
#ifndef USE_MIC
   fprintf(stderr,"%s: %d: No free memory\n",__FILE__,__LINE__);
#endif
   exit(1);
  }
  if ((Yx=(complex float*)malloc((size_t)4*N*sizeof(complex float)))==0) {
#ifndef USE_MIC
   fprintf(stderr,"%s: %d: No free memory\n",__FILE__,__LINE__);
#endif
   exit(1);
  }
  float *YY=(float*)Yx;
  my_fcopy(N, &Y[0], 8, &YY[0], 4);
  my_fcopy(N, &Y[1], 8, &YY[1], 4);
  my_fcopy(N, &Y[4], 8, &YY[2], 4);
  my_fcopy(N, &Y[5], 8, &YY[3], 4);
  my_fcopy(N, &Y[2], 8, &YY[4*N], 4);
  my_fcopy(N, &Y[3], 8, &YY[4*N+1], 4);
  my_fcopy(N, &Y[6], 8, &YY[4*N+2], 4);
  my_fcopy(N, &Y[7], 8, &YY[4*N+3], 4);
  float *ZZ=(float*)Zx;
  my_fcopy(N, &Z[0], 8, &ZZ[0], 4);
  my_fcopy(N, &Z[1], 8, &ZZ[1], 4);
  my_fcopy(N, &Z[4], 8, &ZZ[2], 4); 
  my_fcopy(N, &Z[5], 8, &ZZ[3], 4);
  my_fcopy(N, &Z[2], 8, &ZZ[4*N], 4);
  my_fcopy(N, &Z[3], 8, &ZZ[4*N+1], 4);
  my_fcopy(N, &Z[6], 8, &ZZ[4*N+2], 4);
  my_fcopy(N, &Z[7], 8, &ZZ[4*N+3], 4);


  int ci;

/***************************************************/
 cuFloatComplex *xd,*fgradxd,*etad,*Hetad,*x_propd,*Yd,*Zd;
 float *yd;
 float *wtd,*qd; /* for robust weight and log(weight) */
 float robust_nu=(float)dp->robust_nu;
 float q_sum,robust_nu1;
 float deltanu;
 int Nd=100; /* no of points where nu is sampled, note Nd<N */
 if (Nd>M) { Nd=M; }
 deltanu=(float)(robust_nuhigh-robust_nulow)/(float)Nd;

 /* for counting how many baselines contribute to each station
   grad/hess calculation */
 float *iwd,*iw;
 if ((iw=(float*)malloc((size_t)N*sizeof(float)))==0) {
#ifndef USE_MIC
   fprintf(stderr,"%s: %d: No free memory\n",__FILE__,__LINE__);
#endif
   exit(1);
 }


 unsigned long int moff=0;
 fgradxd=(cuFloatComplex*)&gWORK[moff];
 moff+=8*N; /* 4N complex means 8N float */
 etad=(cuFloatComplex*)&gWORK[moff];
 moff+=8*N;
 Hetad=(cuFloatComplex*)&gWORK[moff];
 moff+=8*N;
 x_propd=(cuFloatComplex*)&gWORK[moff];
 moff+=8*N;
 xd=(cuFloatComplex*)&gWORK[moff];
 moff+=8*N;

 yd=&gWORK[moff];
 moff+=8*M;
 cohd=&gWORK[moff];
 moff+=Nbase*8;
 bbd=(char*)&gWORK[moff];
 unsigned long int charstor=(Nbase*2*sizeof(char))/sizeof(float);
 if (!charstor || charstor%4) {
  moff+=(charstor/4+1)*4; /* NOTE +4 multiple to align memory */
 } else {
  moff+=charstor;
 }
 iwd=&gWORK[moff];
 if (!(N%4)) {
  moff+=N;
 } else {
  moff+=(N/4+1)*4;
 }
 wtd=&gWORK[moff];
 if (!(M%4)) {
  moff+=M;
 } else {
  moff+=(M/4+1)*4;
 }
 qd=&gWORK[moff];
 if (!(M%4)) {
  moff+=M;
 } else {
  moff+=(M/4+1)*4;
 }


 cudaMalloc((void **)&Yd, 4*N*sizeof(cuFloatComplex));
 cudaMalloc((void **)&Zd, 4*N*sizeof(cuFloatComplex));


 /* need 8N*(BlocksPerGrid+8) for tcg_solve+grad/hess storage,
   so total storage needed is 
   8N*(BlocksPerGrid+8) + 8N*5 + 8*M + 8*Nbase + 2*Nbase + N + M + M
   plus 8N + 8N for ADMM params (Y and BZ)
 */
 /* remaining memory */
 float *gWORK1=&gWORK[moff];

 /* yd <=y : V */
 err=cudaMemcpy(yd, y, 8*M*sizeof(float), cudaMemcpyHostToDevice);
 checkCudaError(err,__FILE__,__LINE__);
 /* need to give right offset for coherencies */
 /* offset: cluster offset+time offset */
 /* C */
 err=cudaMemcpy(cohd, &(dp->ddcohf[(dp->Nbase)*(dp->tilesz)*(dp->clus)*8+(dp->Nbase)*tileoff*8]), Nbase*8*sizeof(float), cudaMemcpyHostToDevice);
 checkCudaError(err,__FILE__,__LINE__);
 /* correct offset for baselines */
 err=cudaMemcpy(bbd, &(dp->ddbase[2*(dp->Nbase)*(tileoff)]), Nbase*2*sizeof(char), cudaMemcpyHostToDevice);
 checkCudaError(err,__FILE__,__LINE__);
 /* xd <=x : solution */
 err=cudaMemcpy(xd, x, 8*N*sizeof(float), cudaMemcpyHostToDevice);
 checkCudaError(err,__FILE__,__LINE__);
 err=cudaMemcpy(Yd, Yx, 8*N*sizeof(float), cudaMemcpyHostToDevice);
 checkCudaError(err,__FILE__,__LINE__);
 err=cudaMemcpy(Zd, Zx, 8*N*sizeof(float), cudaMemcpyHostToDevice);
 checkCudaError(err,__FILE__,__LINE__);

 float fx,fx0,norm_grad,Delta,fx_prop,rhonum,rhoden,rho;

 /* count how many baselines contribute to each station, store (inverse) in iwd */
 count_baselines(Nbase,N,iw,&(dp->ddbase[2*(dp->Nbase)*(tileoff)]),dp->Nt);
 err=cudaMemcpy(iwd, iw, N*sizeof(float), cudaMemcpyHostToDevice);
 checkCudaError(err,__FILE__,__LINE__);
 free(iw);

 /* set initial weights to 1 by a cuda kernel */
 cudakernel_setweights_fl(ThreadsPerBlock, (M+ThreadsPerBlock-1)/ThreadsPerBlock, M, wtd, 1.0f);
 fx=cudakernel_fns_f_robust_admm(ThreadsPerBlock,BlocksPerGrid,N,M,xd,Yd,Zd,admm_rho,yd,cohd,bbd,wtd,cbhandle, gWORK1);
 fx0=fx;
#ifdef DEBUG
printf("Initial Cost=%g\n",fx0);
#endif
/***************************************************/
 int rsdstat=0;
 /* RSD solution */
 for (ci=0; ci<itmax_sd; ci++) {
  /* Armijo step */
  rsdstat=armijostep(ThreadsPerBlock, BlocksPerGrid, N, M, xd, Yd,Zd,admm_rho, etad, yd, cohd, bbd,iwd,wtd,&fx,cbhandle,gWORK1);
  /* x=R(x,teta); */
  cudakernel_fns_R(N,xd,etad,x_propd,cbhandle);
  if (!rsdstat) {
   /* cost reduced, update solution */
   cbstatus=cublasCcopy(cbhandle,4*N,x_propd,1,xd,1);
  } else {
   /* no cost reduction, break loop */
   break;
  }
 }

 cudakernel_fns_fupdate_weights(ThreadsPerBlock,BlocksPerGrid,N,M,xd,yd,cohd,bbd,wtd,robust_nu);

 Delta_bar=MIN(fx,Delta_bar);
 Delta0=Delta_bar*0.125f;
//printf("fx=%g Delta_bar=%g Delta0=%g\n",fx,Delta_bar,Delta0);

#ifdef DEBUG
printf("NEW RSD cost=%g\n",fx);
#endif
/***************************************************/
   int min_inner,max_inner,min_outer,max_outer;
   float epsilon,kappa,theta,rho_prime;

   min_inner=1; max_inner=itmax_rtr;//8*N;
   min_outer=3;//itmax_rtr; //3; 
   max_outer=itmax_rtr;
   epsilon=(float)CLM_EPSILON;
   kappa=0.1f;
   theta=1.0f;
   /* default values 0.25, 0.75, 0.25, 2.0 */
   float eta1=0.0001f; float eta2=0.99f; float alpha1=0.25f; float alpha2=3.5f;
   rho_prime=eta1; /* should be <= 0.25, tune for parallel solve  */
   float rho_regularization; /* use large damping */
   rho_regularization=fx*1e-6f;
   /* damping: too small => locally converge, globally diverge
           |\
        |\ | \___
    -|\ | \|
       \
      
    
    right damping:  locally and globally converge
    -|\      
       \|\  
          \|\
             \____ 

    */
   float rho_reg;
   int model_decreased=0;

  /* RTR solution */
  int k=0;
  int stop_outer=(itmax_rtr>0?0:1);
  int stop_inner=0;
  if (!stop_outer) {
   cudakernel_fns_fgrad_robust_admm(ThreadsPerBlock,BlocksPerGrid,N,M,xd, Yd,Zd,admm_rho,fgradxd,yd,cohd,bbd,iwd,wtd,1,cbhandle, gWORK1);
   norm_grad=sqrtf(cudakernel_fns_g(N,xd,fgradxd,fgradxd,cbhandle));
  }
  Delta=Delta0;
  /* initial residual */
  info[0]=fx0;

  /*
   % ** Start of TR loop **
  */
   while(!stop_outer) {
    /*  
     % update counter
    */
     k++;
    /* eta = 0*fgradx; */
    cudaMemset(etad, 0, sizeof(cuFloatComplex)*4*N);


    /* solve TR subproblem, also returns Hessian */
    stop_inner=tcg_solve_cuda(ThreadsPerBlock,BlocksPerGrid, N, M, xd, Yd,Zd,admm_rho,fgradxd, etad, Hetad, Delta, theta, kappa, max_inner, min_inner,yd,cohd,bbd,iwd,wtd,cbhandle, gWORK1);
    /*
        Heta = fns.fhess(x,eta);
    */
    /*
      compute the retraction of the proposal
    */
   cudakernel_fns_R(N,xd,etad,x_propd,cbhandle);

    /*
      compute cost of the proposal
    */
    fx_prop=cudakernel_fns_f_robust_admm(ThreadsPerBlock,BlocksPerGrid,N,M,x_propd,Yd,Zd,admm_rho,yd,cohd,bbd,wtd, cbhandle, gWORK1);

    /*
      check the performance of the quadratic model
    */
    rhonum=fx-fx_prop;
    rhoden=-cudakernel_fns_g(N,xd,fgradxd,etad,cbhandle)-0.5f*cudakernel_fns_g(N,xd,Hetad,etad,cbhandle);
    /* regularization of rho ratio */
    /* 
    rho_reg = max(1, abs(fx)) * eps * options.rho_regularization;
    rhonum = rhonum + rho_reg;
    rhoden = rhoden + rho_reg;
    */
    rho_reg=MAX(1.0f,fx)*rho_regularization; /* no epsilon */
    rhonum+=rho_reg;
    rhoden+=rho_reg;

     /*
        rho =   rhonum  / rhoden;
     */
     rho=rhonum/rhoden;

    /* model_decreased = (rhoden >= 0); */
   /* OLD CODE if (fabsf(rhonum/fx) <sqrtf_epsilon) {
     rho=1.0f;
    } */
    model_decreased=(rhoden>=0.0f?1:0);

#ifdef DEBUG
    printf("stop_inner=%d rho_reg=%g rho =%g/%g= %g rho'= %g\n",stop_inner,rho_reg,rhonum,rhoden,rho,rho_prime);
#endif
    /*
      choose new TR radius based on performance
    */
    if ( !model_decreased || rho<eta1 ) {
      Delta=alpha1*Delta;
    } else if (rho>eta2 && (stop_inner==2 || stop_inner==1)) {
      Delta=MIN(alpha2*Delta,Delta_bar);
    }

    /*
      choose new iterate based on performance
    */
    if (model_decreased && rho>rho_prime) {
     cbstatus=cublasCcopy(cbhandle,4*N,x_propd,1,xd,1);
     fx=fx_prop;
     cudakernel_fns_fgrad_robust_admm(ThreadsPerBlock,BlocksPerGrid,N,M,xd,Yd,Zd,admm_rho, fgradxd,yd,cohd,bbd,iwd,wtd,1,cbhandle, gWORK1);
     norm_grad=sqrtf(cudakernel_fns_g(N,xd,fgradxd,fgradxd,cbhandle));
    }

    /*
     Testing for Stop Criteria
    */
    if (norm_grad<epsilon && k>min_outer) {
      stop_outer=1;
    }

    /*
     stop after max_outer iterations
     */
    if (k>=max_outer) {
      stop_outer=1;
    }

#ifdef DEBUG
printf("Iter %d cost=%g\n",k,fx);
#endif

   }
   /* final residual */
   info[1]=fx;
#ifdef DEBUG
printf("NEW RTR cost=%g\n",fx);
#endif

/***************************************************/
 cudaDeviceSynchronize();
   /* w <= (8+nu)/(1+error^2), q<=w-log(w) */
   cudakernel_fns_fupdate_weights_q(ThreadsPerBlock,BlocksPerGrid,N,M,xd,yd,cohd,bbd,wtd,qd,robust_nu);
   /* sumq<=sum(w-log(w))/N */
   cbstatus=cublasSasum(cbhandle, M, qd, 1, &q_sum);
   q_sum/=(float)M;
#ifdef DEBUG
   printf("deltanu=%f sum(w-log(w))=%f\n",deltanu,q_sum);
#endif
  /* for nu range 2~numax evaluate, p-variate T
     psi((nu0+p)/2)-ln((nu0+p)/2)-psi(nu/2)+ln(nu/2)+1/N sum(ln(w_i)-w_i) +1 
     note: AECM not ECME
     and find min(| |) */
   int ThreadsPerBlock2=ThreadsPerBlock/4;
   cudakernel_evaluatenu_fl_eight(ThreadsPerBlock2, (Nd+ThreadsPerBlock-1)/ThreadsPerBlock2, Nd, q_sum, qd, deltanu,(float)robust_nulow,robust_nu);
   /* find min(abs()) value */
   cbstatus=cublasIsamin(cbhandle, Nd, qd, 1, &ci); /* 1 based index */
   robust_nu1=(float)robust_nulow+(float)(ci-1)*deltanu;
#ifdef DEBUG
   printf("nu updated %d from %f [%lf,%lf] to %f\n",ci,robust_nu,robust_nulow,robust_nuhigh,robust_nu1);
#endif
   dp->robust_nu=(double)robust_nu1;
  
#ifdef DEBUG
  printf("Cost final %g  initial %g\n",fx,fx0);
#endif
  if(fx0>fx) {
   /* copy back current solution, only if cost is reduced */
   err=cudaMemcpy(x,xd,8*N*sizeof(float),cudaMemcpyDeviceToHost);
   checkCudaError(err,__FILE__,__LINE__);


   /* copy back solution to x0 : format checked*/
   /* re J(0,0) */
   my_fcopy(N, &Jd[0], 4, &x0[0], 8);
   /* im J(0,0) */
   my_fcopy(N, &Jd[1], 4, &x0[1], 8);
   /* re J(1,0) */
   my_fcopy(N, &Jd[2], 4, &x0[4], 8);
   /* im J(1,0) */
   my_fcopy(N, &Jd[3], 4, &x0[5], 8);
   /* re J(0,1) */
   my_fcopy(N, &Jd[4*N], 4, &x0[2], 8);
   /* im J(0,1) */
   my_fcopy(N, &Jd[4*N+1], 4, &x0[3], 8);
   /* re J(1,1) */
   my_fcopy(N, &Jd[4*N+2], 4, &x0[6], 8);
   /* im J(1,1) */
   my_fcopy(N, &Jd[4*N+3], 4, &x0[7], 8);
  }

  cudaFree(Yd);
  cudaFree(Zd);

  free(x);
  free(Yx);
  free(Zx);

  return 0;
}
