#include "auris_calibration.h"
#include <errno.h>
#include <math.h>
#include <string.h>

#define RAD_TO_DEG 57.29577951308232

static bool unit(auris_vec3f_t in, double v[3]) {
  const double length = hypot(hypot((double)in.x_m, (double)in.y_m), (double)in.z_m);
  if (!isfinite(length) || length < 1.0e-20) return false;
  v[0] = (double)in.x_m / length;
  v[1] = (double)in.y_m / length;
  v[2] = (double)in.z_m / length;
  return true;
}

static double dot(const double a[3], const double b[3]) {
  return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static void cross(const double a[3], const double b[3], double v[3]) {
  v[0]=a[1]*b[2]-a[2]*b[1];
  v[1]=a[2]*b[0]-a[0]*b[2];
  v[2]=a[0]*b[1]-a[1]*b[0];
}

bool auris_calibration_rotation_valid(const float r[3][3]) {
  if (r == NULL) return false;
  for (size_t i=0; i<3; ++i) for (size_t j=0; j<3; ++j) {
    double d=0;
    for (size_t k=0; k<3; ++k) d+=(double)r[k][i]*r[k][j];
    if (!isfinite(d) || fabs(d-(i==j ? 1.0:0.0))>1.0e-4) return false;
  }
  const double det=(double)r[0][0]*(r[1][1]*r[2][2]-r[1][2]*r[2][1])-
      (double)r[0][1]*(r[1][0]*r[2][2]-r[1][2]*r[2][0])+
      (double)r[0][2]*(r[1][0]*r[2][1]-r[1][1]*r[2][0]);
  return fabs(det-1.0)<1.0e-4;
}

static double error_deg(const float r[3][3], const double c[3], const double a[3]) {
  double predicted[3]={0}, v[3];
  for (size_t i=0; i<3; ++i)
    for (size_t j=0; j<3; ++j) predicted[i]+=(double)r[i][j]*c[j];
  cross(predicted,a,v);
  return RAD_TO_DEG*atan2(sqrt(dot(v,v)),dot(predicted,a));
}

/* Two separated vector pairs give a proper rotation candidate (TRIAD). */
static bool basis(const double a[3], const double b[3], double m[3][3]) {
  double v[3],w[3]; cross(a,b,v);
  const double length=sqrt(dot(v,v));
  if (length<0.10) return false;
  for (size_t i=0; i<3; ++i) v[i]/=length;
  cross(a,v,w);
  for (size_t i=0; i<3; ++i) { m[i][0]=a[i]; m[i][1]=v[i]; m[i][2]=w[i]; }
  return true;
}

/* Quaternion least squares, scalar-first q; H = sum(camera * array^T).
   Jacobi diagonalization finds the largest algebraic eigenvalue, including
   rotations near 180 degrees (no power-iteration initial-vector trap). */
static void refine(double c[][3], double a[][3], const bool *used, size_t n,
                    float r[3][3]) {
  double h[3][3]={{0}};
  for (size_t p=0; p<n; ++p) if (used[p])
    for (size_t i=0; i<3; ++i) for (size_t j=0; j<3; ++j) h[i][j]+=c[p][i]*a[p][j];
  const double trace=h[0][0]+h[1][1]+h[2][2];
  double k[4][4]={{0}}, v[4][4]={{0}};
  k[0][0]=trace;
  k[0][1]=k[1][0]=h[1][2]-h[2][1];
  k[0][2]=k[2][0]=h[2][0]-h[0][2];
  k[0][3]=k[3][0]=h[0][1]-h[1][0];
  for (size_t i=0; i<3; ++i) for (size_t j=0; j<3; ++j)
    k[i+1U][j+1U]=h[i][j]+h[j][i]-(i==j ? trace:0);
  for (size_t i=0; i<4; ++i) v[i][i]=1;
  for (size_t sweep=0; sweep<80; ++sweep) {
    size_t p=0,q=1;
    for (size_t i=0; i<4; ++i) for (size_t j=i+1U; j<4; ++j)
      if (fabs(k[i][j])>fabs(k[p][q])) { p=i; q=j; }
    if (fabs(k[p][q])<1.0e-12) break;
    const double angle=0.5*atan2(2*k[p][q],k[q][q]-k[p][p]);
    const double cs=cos(angle),sn=sin(angle),pp=k[p][p],qq=k[q][q],pq=k[p][q];
    for (size_t i=0; i<4; ++i) {
      if (i!=p && i!=q) {
        const double ip=k[i][p],iq=k[i][q];
        k[i][p]=k[p][i]=cs*ip-sn*iq;
        k[i][q]=k[q][i]=sn*ip+cs*iq;
      }
      const double ip=v[i][p],iq=v[i][q];
      v[i][p]=cs*ip-sn*iq; v[i][q]=sn*ip+cs*iq;
    }
    k[p][p]=cs*cs*pp-2*cs*sn*pq+sn*sn*qq;
    k[q][q]=sn*sn*pp+2*cs*sn*pq+cs*cs*qq;
    k[p][q]=k[q][p]=0;
  }
  size_t best=0;
  for (size_t i=1; i<4; ++i) if (k[i][i]>k[best][best]) best=i;
  const double w=v[0][best],x=v[1][best],y=v[2][best],z=v[3][best];
  r[0][0]=(float)(1-2*(y*y+z*z)); r[0][1]=(float)(2*(x*y-z*w)); r[0][2]=(float)(2*(x*z+y*w));
  r[1][0]=(float)(2*(x*y+z*w)); r[1][1]=(float)(1-2*(x*x+z*z)); r[1][2]=(float)(2*(y*z-x*w));
  r[2][0]=(float)(2*(x*z-y*w)); r[2][1]=(float)(2*(y*z+x*w)); r[2][2]=(float)(1-2*(x*x+y*y));
}

int auris_calibration_solve(const auris_calibration_pair_t *pairs, size_t n,
                            double threshold, auris_calibration_result_t *out) {
  if (pairs==NULL || out==NULL || n<6U || n>AURIS_CALIBRATION_MAX_POINTS ||
      !isfinite(threshold) || threshold<=0 || threshold>10) return -EINVAL;
  double c[AURIS_CALIBRATION_MAX_POINTS][3],a[AURIS_CALIBRATION_MAX_POINTS][3];
  for (size_t p=0; p<n; ++p)
    if (!unit(pairs[p].camera,c[p]) || !unit(pairs[p].array,a[p]) || c[p][2]<=0)
      return -EINVAL;
  auris_calibration_result_t result={0};
  double best_cost=INFINITY;
  for (size_t p=0; p<n; ++p) for (size_t q=p+1U; q<n; ++q) {
    double cb[3][3],ab[3][3]; float r[3][3]={{0}};
    if (!basis(c[p],c[q],cb) || !basis(a[p],a[q],ab)) continue;
    for (size_t i=0; i<3; ++i) for (size_t j=0; j<3; ++j)
      for (size_t k=0; k<3; ++k) r[i][j]+=(float)(ab[i][k]*cb[j][k]);
    bool used[AURIS_CALIBRATION_MAX_POINTS]={false}; size_t inliers=0;
    double cost=0;
    for (size_t i=0; i<n; ++i) {
      const double e=error_deg((const float (*)[3])r,c[i],a[i]);
      if (e<=threshold) { used[i]=true; ++inliers; cost+=e*e; }
    }
    if (inliers>result.inliers || (inliers==result.inliers && cost<best_cost)) {
      result.inliers=inliers; best_cost=cost;
      memcpy(result.used,used,sizeof(used));
    }
  }
  if (result.inliers<6U || result.inliers*4U<n*3U) return -ERANGE;
  /* Refit the winning consensus; then trim once and refit consistently. */
  refine(c,a,result.used,n,result.rotation);
  for (size_t i=0; i<n; ++i)
    result.used[i]=result.used[i] &&
        error_deg((const float (*)[3])result.rotation,c[i],a[i])<=threshold;
  result.inliers=0;
  double mx=0,my=0,xx=0,yy=0,xy=0;
  for (size_t i=0; i<n; ++i) if (result.used[i]) {
    ++result.inliers;
    const double x=c[i][0]/c[i][2],y=c[i][1]/c[i][2];
    mx+=x; my+=y; xx+=x*x; yy+=y*y; xy+=x*y;
  }
  if (result.inliers<6U || result.inliers*4U<n*3U) return -ERANGE;
  const double count=(double)result.inliers;
  xx=xx/count-(mx/count)*(mx/count); yy=yy/count-(my/count)*(my/count);
  xy=xy/count-(mx/count)*(my/count);
  /* Smallest 2-D covariance eigenvalue: reject clustered/line-like clicks. */
  if (0.5*(xx+yy-hypot(xx-yy,2*xy))<0.0025) return -ERANGE;
  refine(c,a,result.used,n,result.rotation);
  if (!auris_calibration_rotation_valid((const float (*)[3])result.rotation)) return -ERANGE;
  for (size_t i=0; i<n; ++i) if (result.used[i]) {
    const double e=error_deg((const float (*)[3])result.rotation,c[i],a[i]);
    result.rms_deg+=e*e; result.max_deg=fmax(result.max_deg,e);
  }
  result.rms_deg=sqrt(result.rms_deg/count);
  if (result.max_deg>threshold || result.rms_deg>threshold*0.75) return -ERANGE;
  *out=result;
  return 0;
}

int auris_calibration_error(const auris_view_t *view, auris_calibration_pair_t pair,
                            double *degrees, double *pixels) {
  double c[3],a[3],p[3]={0};
  if (view==NULL || degrees==NULL || pixels==NULL ||
      !auris_calibration_rotation_valid(view->rotation) ||
      !isfinite(view->fx) || !isfinite(view->fy) || view->fx<=0 || view->fy<=0 ||
      !unit(pair.camera,c) || !unit(pair.array,a) || c[2]<=1.0e-6) return -EINVAL;
  for (size_t i=0; i<3; ++i) for (size_t j=0; j<3; ++j)
    p[i]+=(double)view->rotation[j][i]*a[j];
  if (p[2]<=1.0e-6) return -ERANGE;
  *degrees=error_deg(view->rotation,c,a);
  *pixels=hypot((double)view->fx*(p[0]/p[2]-c[0]/c[2]),
                (double)view->fy*(p[1]/p[2]-c[1]/c[2]));
  return 0;
}
