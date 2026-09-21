#include "auris_output.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
typedef struct { atomic_bool entered, release, closed; atomic_uint sequence, pixel; } fake_t;
static void pause_short(void) {
  const struct timespec t={.tv_nsec=1000000L}; (void)nanosleep(&t,NULL);
}
static int present(void *context,const auris_rgb24_t *rgb,const auris_output_frame_t *f,
                    auris_output_stats_t *stats,auris_output_cancel_t cancel,void *token) {
  (void)stats;
  fake_t *s=context;
  atomic_store(&s->entered,true);
  while (!atomic_load(&s->release)) { if (cancel(token)) return -ECANCELED; pause_short(); }
  atomic_store(&s->sequence,f->sequence);
  atomic_store(&s->pixel,rgb->data[0]);
  return 0;
}
static void close_fake(void *context) { fake_t *s=context; atomic_store(&s->closed,true); }
int main(void) {
  fake_t slow={0},fast={0};
  atomic_store(&fast.release,true);
  const auris_output_backend_t backend={NULL,present,close_fake};
  auris_output_t *a=NULL,*b=NULL;
  CHECK(!auris_output_open_backend(&a,2U,2U,&slow,&backend));
  CHECK(!auris_output_open_backend(&b,2U,2U,&fast,&backend));
  uint8_t pixels[12]={7};
  auris_rgb24_t rgb={pixels,2U,2U,6U,sizeof(pixels)};
  auris_output_frame_t f={.sequence=1U};
  CHECK(!auris_output_submit(a,&rgb,&f));
  for (int i=0;i<1000 && !atomic_load(&slow.entered);++i) pause_short();
  CHECK(atomic_load(&slow.entered));
  for (uint32_t i=2U;i<=10U;++i) { f.sequence=i; pixels[0]=(uint8_t)i; CHECK(!auris_output_submit(a,&rgb,&f)); }
  CHECK(auris_output_flush(a,10U)==-ETIMEDOUT);
  CHECK(!auris_output_submit(b,&rgb,&f));
  pixels[0]=99U;
  CHECK(!auris_output_flush(b,1000U));
  CHECK(atomic_load(&fast.pixel)==10U);
  auris_output_stats_t stats;
  auris_output_stats(a,&stats);
  CHECK(stats.submitted==10U && stats.dropped==8U);
  atomic_store(&slow.release,true);
  CHECK(!auris_output_flush(a,1000U));
  CHECK(atomic_load(&slow.sequence)==10U && atomic_load(&slow.pixel)==10U);
  auris_output_stats(a,&stats); CHECK(stats.sent==2U);
  rgb.stride=SIZE_MAX; CHECK(auris_output_submit(a,&rgb,&f)==-EINVAL);
  auris_output_close(a); auris_output_close(b);
  CHECK(atomic_load(&slow.closed) && atomic_load(&fast.closed));
  fake_t stuck={0}; a=NULL; rgb.stride=6U;
  CHECK(!auris_output_open_backend(&a,2U,2U,&stuck,&backend));
  CHECK(!auris_output_submit(a,&rgb,&f));
  for (int i=0;i<1000 && !atomic_load(&stuck.entered);++i) pause_short();
  CHECK(atomic_load(&stuck.entered));
  auris_output_close(a); CHECK(atomic_load(&stuck.closed));
  puts("worker ownership, isolation, replacement and cancellation passed");
  return 0;
}
