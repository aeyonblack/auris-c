#include "auris_camera_calibration.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

uint64_t auris_calibration_array_id(const auris_array_t *a) {
  if (auris_array_validate(a)!=AURIS_OK) return 0;
  uint64_t hash=UINT64_C(14695981039346656037);
  char record[160];
  for (size_t i=0; i<a->microphone_count; ++i) {
    const auris_vec3f_t p=a->positions[i];
    const int n=snprintf(record,sizeof(record),"%zu:%zu:%zu:%.9g:%.9g:%.9g:%.9g;",
        a->microphone_count,i,a->capture_channels[i],(double)p.x_m,(double)p.y_m,
        (double)p.z_m,(double)a->speed_of_sound_mps);
    if (n<0 || (size_t)n>=sizeof(record)) return 0;
    for (int j=0; j<n; ++j) { hash^=(unsigned char)record[j]; hash*=UINT64_C(1099511628211); }
  }
  return hash;
}

static bool compatible(const auris_view_t *a, const auris_view_t *b) {
  return a->width>=2U && a->height>=2U && a->width<=4096U && a->height<=4096U &&
      a->width==b->width && a->height==b->height &&
      isfinite(a->fx) && isfinite(a->fy) && isfinite(a->cx) && isfinite(a->cy) &&
      a->fx>0 && a->fy>0 && fabsf(a->fx-b->fx)<0.001f &&
      fabsf(a->fy-b->fy)<0.001f && fabsf(a->cx-b->cx)<0.001f &&
      fabsf(a->cy-b->cy)<0.001f;
}

static bool number(char **state, int base, uintmax_t limit, uintmax_t *out) {
  char *token=strtok_r(NULL," \t\r\n",state),*end;
  if (token==NULL || token[0]=='-' || token[0]=='+') return false;
  errno=0;
  const uintmax_t value=strtoumax(token,&end,base);
  if (errno!=0 || end==token || *end!='\0' || value>limit) return false;
  *out=value; return true;
}

static bool real(char **state, float *out) {
  char *token=strtok_r(NULL," \t\r\n",state),*end;
  if (token==NULL) return false;
  errno=0;
  const float value=strtof(token,&end);
  if (errno!=0 || end==token || *end!='\0' || !isfinite(value)) return false;
  *out=value; return true;
}

int auris_camera_calibration_load(const char *path, uint64_t id, auris_view_t *camera) {
  if (path==NULL || camera==NULL || id==0U) return -EINVAL;
  FILE *f=fopen(path,"r");
  if (f==NULL) return -errno;
  auris_view_t read={0};
  int err=-EINVAL;
  char text[1024],*state=NULL;
  const size_t bytes=fread(text,1U,sizeof(text)-1U,f);
  if (ferror(f) || fgetc(f)!=EOF || memchr(text,'\0',bytes)!=NULL) goto done;
  text[bytes]='\0';
  const char *magic=strtok_r(text," \t\r\n",&state);
  uintmax_t found,width,height;
  if (magic==NULL || strcmp(magic,"AURISCAL1")!=0 ||
      !number(&state,16,UINT64_MAX,&found) || found!=id ||
      !number(&state,10,4096,&width) || !number(&state,10,4096,&height) ||
      !real(&state,&read.fx) || !real(&state,&read.fy) ||
      !real(&state,&read.cx) || !real(&state,&read.cy)) goto done;
  read.width=(size_t)width; read.height=(size_t)height;
  if (!compatible(&read,camera)) goto done;
  for (size_t i=0; i<3; ++i) for (size_t j=0; j<3; ++j)
    if (!real(&state,&read.rotation[i][j])) goto done;
  if (strtok_r(NULL," \t\r\n",&state)!=NULL ||
      !auris_calibration_rotation_valid((const float (*)[3])read.rotation)) goto done;
  err=0;
done:
  if (fclose(f)!=0 && err==0) err=-EIO;
  if (err==0) memcpy(camera->rotation,read.rotation,sizeof(camera->rotation));
  return err;
}

int auris_camera_calibration_save(const char *path, uint64_t id, const auris_view_t *camera) {
  if (path==NULL || camera==NULL || id==0U || !compatible(camera,camera) ||
      !auris_calibration_rotation_valid(camera->rotation)) return -EINVAL;
  const size_t length=strlen(path);
  if (length>4000U) return -ENAMETOOLONG;
  char *temporary=malloc(length+12U);
  if (temporary==NULL) return -ENOMEM;
  (void)snprintf(temporary,length+12U,"%s.tmpXXXXXX",path);
  int fd=mkstemp(temporary);
  if (fd<0) { const int err=-errno; free(temporary); return err; }
  FILE *f=fdopen(fd,"w");
  int err=0;
  if (f==NULL) { err=-errno; (void)close(fd); goto done; }
  if (fprintf(f,"AURISCAL1 %016" PRIx64 "\n%zu %zu %.9g %.9g %.9g %.9g\n",
      id,camera->width,camera->height,(double)camera->fx,(double)camera->fy,
      (double)camera->cx,(double)camera->cy)<0) err=-EIO;
  for (size_t i=0; i<3 && err==0; ++i)
    if (fprintf(f,"%.9g %.9g %.9g\n",(double)camera->rotation[i][0],
        (double)camera->rotation[i][1],(double)camera->rotation[i][2])<0) err=-EIO;
  if (err==0 && (fflush(f)!=0 || fsync(fd)!=0)) err=-errno;
  if (fclose(f)!=0 && err==0) err=-errno;
  if (err==0 && rename(temporary,path)!=0) err=-errno;
  if (err==0) {
    char parent[4002];
    memcpy(parent,path,length+1U);
    char *slash=strrchr(parent,'/');
    if (slash==NULL) (void)strcpy(parent,".");
    else if (slash==parent) parent[1]='\0';
    else *slash='\0';
    const int directory=open(parent,O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory<0) err=-errno;
    else { if (fsync(directory)!=0) err=-errno; (void)close(directory); }
  }
done:
  if (err!=0) (void)unlink(temporary);
  free(temporary);
  return err;
}
