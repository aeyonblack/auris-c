#include "auris_settings.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

auris_settings_t auris_settings_defaults(void) {
  return (auris_settings_t){true,true,true,0.55f,0.75f,15U};
}
bool auris_settings_valid(const auris_settings_t *s) {
  return s && isfinite(s->cutoff) && s->cutoff>=0.0f && s->cutoff<=0.95f &&
    isfinite(s->opacity) && s->opacity>=0.0f && s->opacity<=1.0f &&
    s->fps>=1U && s->fps<=30U;
}
int auris_settings_load(const char *path, auris_settings_t *s) {
  if (!path || !s) return -EINVAL;
  FILE *f=fopen(path,"r");
  if (!f) return -errno;
  int overlay=0,gate=0,smooth=0;
  auris_settings_t next={0};
  int fields=fscanf(f,"AURIS_SETTINGS 1\noverlay %1d\ngating %1d\nsmoothing %1d\ncutoff %32f\nopacity %32f\nfps %3u",
      &overlay,&gate,&smooth,&next.cutoff,&next.opacity,&next.fps);
  int c;
  do { c=fgetc(f); } while (c==' ' || c=='\t' || c=='\r' || c=='\n');
  const bool ok=fields==6 && c==EOF && !ferror(f) &&
    (overlay==0 || overlay==1) && (gate==0 || gate==1) && (smooth==0 || smooth==1) &&
    auris_settings_valid(&next);
  (void)fclose(f);
  if (!ok) return -EINVAL;
  next.overlay=overlay!=0; next.gating=gate!=0; next.smoothing=smooth!=0;
  *s=next;
  return 0;
}
struct auris_settings_writer {
  pthread_mutex_t mutex;
  pthread_cond_t ready;
  pthread_t thread;
  char *path;
  auris_settings_t pending;
  bool queued,stop;
  int status;
};
static int save_file(const char *path,const auris_settings_t *s) {
  size_t n=strlen(path);
  char *temp=malloc(n+12U), *dir=strdup(path);
  if (!temp || !dir) { free(temp); free(dir); return -ENOMEM; }
  (void)snprintf(temp,n+12U,"%s.tmp.XXXXXX",path);
  char *slash=strrchr(dir,'/');
  if (slash) { if (slash==dir) slash[1]='\0'; else *slash='\0'; }
  else { free(dir); dir=strdup("."); }
  if (!dir) { free(temp); return -ENOMEM; }
  int result=0;
  int fd=mkstemp(temp);
  if (fd<0) { result=-errno; goto done; }
  char data[256];
  int size=snprintf(data,sizeof(data),
      "AURIS_SETTINGS 1\noverlay %d\ngating %d\nsmoothing %d\ncutoff %.9g\nopacity %.9g\nfps %u\n",
      s->overlay,s->gating,s->smoothing,(double)s->cutoff,(double)s->opacity,s->fps);
  size_t sent=0U;
  while (sent<(size_t)size) {
    ssize_t wrote=write(fd,data+sent,(size_t)size-sent);
    if (wrote<0 && errno==EINTR) continue;
    if (wrote<=0) { result=wrote<0 ? -errno : -EIO; break; }
    sent+=(size_t)wrote;
  }
  if (!result && fsync(fd)) result=-errno;
  if (close(fd) && !result) result=-errno;
  if (!result && rename(temp,path)) result=-errno;
  if (!result) {
    int directory=open(dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (directory<0) result=-errno;
    else { if (fsync(directory)) result=-errno; (void)close(directory); }
  }
  if (result) (void)unlink(temp);
done:
  free(temp); free(dir); return result;
}
static void *writer_main(void *context) {
  auris_settings_writer_t *w=context;
  (void)pthread_mutex_lock(&w->mutex);
  for (;;) {
    while (!w->queued && !w->stop) (void)pthread_cond_wait(&w->ready,&w->mutex);
    if (!w->queued && w->stop) break;
    auris_settings_t copy=w->pending;
    w->queued=false;
    (void)pthread_mutex_unlock(&w->mutex);
    int err=save_file(w->path,&copy);
    (void)pthread_mutex_lock(&w->mutex);
    w->status=err;
  }
  (void)pthread_mutex_unlock(&w->mutex);
  return NULL;
}
int auris_settings_writer_open(auris_settings_writer_t **out,const char *path) {
  if (!out || *out || !path || !*path) return -EINVAL;
  auris_settings_writer_t *w=calloc(1U,sizeof(*w));
  if (!w) return -ENOMEM;
  w->path=strdup(path);
  if (!w->path) { free(w); return -ENOMEM; }
  int err=pthread_mutex_init(&w->mutex,NULL);
  if (err) goto fail;
  err=pthread_cond_init(&w->ready,NULL);
  if (err) { (void)pthread_mutex_destroy(&w->mutex); goto fail; }
  sigset_t blocked,previous;
  sigemptyset(&blocked); sigaddset(&blocked,SIGINT); sigaddset(&blocked,SIGTERM);
  err=pthread_sigmask(SIG_BLOCK,&blocked,&previous);
  if (!err) {
    err=pthread_create(&w->thread,NULL,writer_main,w);
    (void)pthread_sigmask(SIG_SETMASK,&previous,NULL);
  }
  if (err) {
    (void)pthread_cond_destroy(&w->ready); (void)pthread_mutex_destroy(&w->mutex); goto fail;
  }
  *out=w; return 0;
fail:
  free(w->path); free(w); return -err;
}
int auris_settings_save(auris_settings_writer_t *w,const auris_settings_t *s) {
  if (!w || !auris_settings_valid(s)) return -EINVAL;
  (void)pthread_mutex_lock(&w->mutex);
  int err=w->status==1 ? -EBUSY : 0;
  if (!err) { w->pending=*s; w->queued=true; w->status=1; (void)pthread_cond_signal(&w->ready); }
  (void)pthread_mutex_unlock(&w->mutex);
  return err;
}
int auris_settings_save_status(auris_settings_writer_t *w) {
  if (!w) return -ENODEV;
  (void)pthread_mutex_lock(&w->mutex);
  int status=w->status;
  (void)pthread_mutex_unlock(&w->mutex);
  return status;
}
void auris_settings_writer_close(auris_settings_writer_t *w) {
  if (!w) return;
  (void)pthread_mutex_lock(&w->mutex);
  w->stop=true; (void)pthread_cond_signal(&w->ready);
  (void)pthread_mutex_unlock(&w->mutex);
  (void)pthread_join(w->thread,NULL);
  (void)pthread_cond_destroy(&w->ready); (void)pthread_mutex_destroy(&w->mutex);
  free(w->path); free(w);
}
