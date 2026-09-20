#include "imager_calibration.h"
#include "auris_array_configs.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static auris_vec3f_t ray(double x,double y) {
  const double n=hypot(hypot(x,y),1);
  return (auris_vec3f_t){(float)(x/n),(float)(y/n),(float)(1/n)};
}

static auris_vec3f_t rotate(const float r[3][3],auris_vec3f_t v) {
  const float a[3]={v.x_m,v.y_m,v.z_m}; float b[3]={0};
  for(size_t i=0;i<3;++i) for(size_t j=0;j<3;++j) b[i]+=r[i][j]*a[j];
  return (auris_vec3f_t){b[0],b[1],b[2]};
}

static void matrix(float r[3][3],double degrees) {
  /* Rodrigues about a non-coordinate axis, exercising every matrix entry. */
  const double axis[3]={1/sqrt(14),2/sqrt(14),3/sqrt(14)};
  const double t=degrees/57.29577951308232,c=cos(t),s=sin(t);
  for(size_t i=0;i<3;++i) for(size_t j=0;j<3;++j)
    r[i][j]=(float)((i==j ? c:0)+(1-c)*axis[i]*axis[j]);
  r[0][1]-=(float)(s*axis[2]); r[1][0]+=(float)(s*axis[2]);
  r[0][2]+=(float)(s*axis[1]); r[2][0]-=(float)(s*axis[1]);
  r[1][2]-=(float)(s*axis[0]); r[2][1]+=(float)(s*axis[0]);
}

static void pairs_for(auris_calibration_pair_t *pairs,const float r[3][3]) {
  for(size_t y=0;y<3;++y) for(size_t x=0;x<3;++x) {
    const size_t i=y*3U+x;
    pairs[i].camera=ray(((double)x-1)*.35,((double)y-1)*.25);
    pairs[i].array=rotate(r,pairs[i].camera);
  }
}

int main(void) {
  auris_calibration_pair_t pairs[9]; auris_calibration_result_t fit;
  const double angles[]={0,12,-23,90,179.99,180};
  for(size_t k=0;k<sizeof(angles)/sizeof(angles[0]);++k) {
    float expected[3][3]; matrix(expected,angles[k]);
    pairs_for(pairs,(const float (*)[3])expected);
    assert(auris_calibration_solve(pairs,9,3,&fit)==0 && fit.inliers==9U);
    assert(fit.rms_deg<.001);
    for(size_t i=0;i<3;++i) for(size_t j=0;j<3;++j)
      assert(fabsf(fit.rotation[i][j]-expected[i][j])<1.0e-5f);
  }
  float expected[3][3]; matrix(expected,12);
  pairs_for(pairs,(const float (*)[3])expected);
  pairs[0].array=ray(.8,.8); pairs[8].array=ray(-.9,-.8);
  assert(auris_calibration_solve(pairs,9,3,&fit)==0 && fit.inliers==7U);
  assert(!fit.used[0] && !fit.used[8] && fit.rms_deg<.001);
  pairs_for(pairs,(const float (*)[3])expected);
  for(size_t i=0;i<9;++i) {
    pairs[i].array.x_m+=(float)(.002*sin((double)i*1.7));
    pairs[i].array.y_m+=(float)(.002*cos((double)i*.9));
  }
  assert(auris_calibration_solve(pairs,9,3,&fit)==0 && fit.rms_deg<.2);
  /* Wrong reflections cannot masquerade as a proper rotation. */
  for(size_t i=0;i<9;++i) { pairs[i].array=pairs[i].camera; pairs[i].array.x_m*=-1; }
  assert(auris_calibration_solve(pairs,9,3,&fit)==-ERANGE);
  for(size_t i=0;i<9;++i) pairs[i]=(auris_calibration_pair_t){ray(.03*(double)i,0),ray(.03*(double)i,0)};
  assert(auris_calibration_solve(pairs,9,3,&fit)==-ERANGE);
  for(size_t i=0;i<9;++i) pairs[i]=pairs[0];
  assert(auris_calibration_solve(pairs,9,3,&fit)==-ERANGE);
  pairs[0].camera.x_m=NAN;
  assert(auris_calibration_solve(pairs,9,3,&fit)==-EINVAL);
  assert(auris_calibration_solve(pairs,5,3,&fit)==-EINVAL);
  assert(auris_calibration_solve(pairs,33,3,&fit)==-EINVAL);

  auris_view_t camera={.width=640,.height=480,.fx=480,.fy=480,.cx=319.5f,.cy=239.5f};
  matrix(camera.rotation,0);
  const auris_calibration_pair_t offset={ray(0,0),ray(.1,0)};
  double degrees,pixels;
  assert(auris_calibration_error(&camera,offset,&degrees,&pixels)==0);
  assert(fabs(pixels-48)<.001 && fabs(degrees-5.710593)<.001);

  char directory[]="/tmp/auris-calibration-test.XXXXXX";
  assert(mkdtemp(directory)!=NULL);
  char path[160]; (void)snprintf(path,sizeof(path),"%s/camera.txt",directory);
  const uint64_t id=auris_calibration_array_id(auris_config_respeaker_usb_4());
  assert(id!=0);
  assert(auris_camera_calibration_load(path,id,&camera)==-ENOENT);
  matrix(camera.rotation,12);
  assert(auris_camera_calibration_save(path,id,&camera)==0);
  auris_view_t loaded=camera; matrix(loaded.rotation,0);
  assert(auris_camera_calibration_load(path,id,&loaded)==0);
  assert(memcmp(loaded.rotation,camera.rotation,sizeof(camera.rotation))==0);
  assert(auris_camera_calibration_load(path,id+1U,&loaded)==-EINVAL);
  loaded.fx+=1;
  assert(auris_camera_calibration_load(path,id,&loaded)==-EINVAL);
  loaded=camera; loaded.rotation[0][0]=NAN;
  assert(auris_camera_calibration_save(path,id,&loaded)==-EINVAL);
  loaded=camera;
  assert(auris_camera_calibration_load(path,id,&loaded)==0); /* old file survives */
  FILE *f=fopen(path,"a"); assert(f!=NULL);
  assert(fputs("GARBAGE",f)>=0 && fclose(f)==0);
  assert(auris_camera_calibration_load(path,id,&loaded)==-EINVAL);
  f=fopen(path,"w"); assert(f!=NULL);
  assert(fputs("AURISCAL1 fffffffffffffffffffffffffffffffffff 640 480 480 480 0 0",f)>=0);
  assert(fclose(f)==0);
  assert(auris_camera_calibration_load(path,id,&loaded)==-EINVAL);
  assert(unlink(path)==0 && rmdir(directory)==0);

  imager_calibration_capture_t capture={.active=true,.start_us=1000000,.deadline_us=3500000};
  for(size_t i=0;i<64;++i) imager_calibration_feed(&capture,ray(.1,.1),
      1000000U+i*16000U,1030000U+i*16000U,true);
  auris_vec3f_t mean; double scatter;
  assert(imager_calibration_average(&capture,&mean,&scatter)==0 && scatter<.001);
  capture.rays[10]=ray(-.2,-.3);
  assert(imager_calibration_average(&capture,&mean,&scatter)==-ERANGE);
  capture.count=0; capture.rejected=0;
  imager_calibration_feed(&capture,ray(0,0),1000000,1030000,false);
  assert(capture.count==0U && capture.rejected==1U);
  assert(imager_calibration_average(&capture,&mean,&scatter)==-EAGAIN);

  /* Session: stale/off-content clicks do not arm; malformed commands do not save. */
  imager_calibration_t session={0};
  matrix(camera.rotation,0); session.baseline=camera;
  session.drawing=(auris_overlay_config_t){.camera=camera,.search=camera,
      .layout={640,480,0,0,640,480},.columns=3,.rows=3,.cutoff=.5f,.opacity=.5f};
  assert(auris_overlay_open(&session.overlay,&session.drawing)==0);
  capture=(imager_calibration_capture_t){0};
  imager_calibration_frame(&session,7U,1000000U,true);
  imager_calibration_command(&session,"point 7 300 200",2100001U,&capture);
  assert(!capture.active && !session.pending);
  imager_calibration_command(&session,"point 7 nan 200",1100000U,&capture);
  assert(!capture.active);
  imager_calibration_command(&session,"point 999999999999999999999999 1 2",1100000U,&capture);
  assert(!capture.active);
  imager_calibration_command(&session,"point 7 1 2 junk",1100000U,&capture);
  assert(!capture.active);
  imager_calibration_command(&session,"point 7 300 200",1100000U,&capture);
  assert(capture.active && session.pending);
  imager_calibration_command(&session,"cancel",1100001U,&capture);
  assert(!capture.active && !session.pending);
  /* End-to-end session: fitting previews but cannot save without held-out data. */
  char session_dir[]="/tmp/auris-calibration-session.XXXXXX";
  assert(mkdtemp(session_dir)!=NULL);
  (void)snprintf(path,sizeof(path),"%s/cal.txt",session_dir);
  session.path=path; session.array_id=id;
  pairs_for(session.pairs,(const float (*)[3])expected); session.count=9U;
  imager_calibration_command(&session,"solve",2000000U,&capture);
  assert(session.solved && session.check_count==0U);
  imager_calibration_command(&session,"save",2000001U,&capture);
  assert(access(path,F_OK)!=0 && errno==ENOENT);
  for(size_t i=0;i<3;++i) {
    session.checks[i].camera=ray(-.2+.2*(double)i,.12);
    session.checks[i].array=rotate((const float (*)[3])expected,session.checks[i].camera);
  }
  session.check_count=3;
  imager_calibration_command(&session,"save",2000002U,&capture);
  loaded=camera;
  assert(auris_camera_calibration_load(path,id,&loaded)==0);
  assert(unlink(path)==0);
  session.checks[0].array=ray(.9,.9);
  imager_calibration_command(&session,"save",2000003U,&capture);
  assert(access(path,F_OK)!=0 && errno==ENOENT);
  imager_calibration_command(&session,"undo",2000004U,&capture);
  assert(!session.solved && session.check_count==0U && session.count==8U);
  assert(rmdir(session_dir)==0);
  auris_overlay_close(session.overlay);
  puts("calibration solver, persistence, capture and session checks passed");
  return 0;
}
