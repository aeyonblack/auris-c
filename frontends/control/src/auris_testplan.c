#include "auris_testplan.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILE 65536U
#define SEPARATORS " \t\r"

static bool copy_field(char *out,size_t capacity,const char *in) {
  if (!in || strlen(in)>=capacity) return false;
  strcpy(out,in);
  return true;
}
static void show_spaces(char *field) {
  for (;*field;++field) if (*field=='_') *field=' ';
}
/* A folder name: nothing that could climb out of the results folder. */
static bool safe_name(const char *name) {
  if (!*name) return false;
  for (;*name;++name) {
    const char c=*name;
    if (!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') ||
          c=='_' || c=='-')) return false;
  }
  return true;
}
static bool number(const char *in,float *out) {
  char *end=NULL;
  if (!in) return false;
  const float value=strtof(in,&end);
  if (end==in || *end!='\0' || !isfinite(value)) return false;
  *out=value;
  return true;
}
static bool parse_where(const auris_testplan_t *plan,char *list,unsigned int *mask) {
  char *saved=NULL;
  *mask=0U;
  for (char *name=strtok_r(list,",",&saved);name;name=strtok_r(NULL,",",&saved)) {
    size_t i=0U;
    while (i<plan->location_count && strcmp(plan->locations[i],name)!=0) ++i;
    if (i==plan->location_count) return false;
    *mask|=1U<<i;
  }
  return *mask!=0U;
}
static bool parse_clip(auris_testplan_t *plan,char **saved) {
  if (plan->clip_count==AURIS_TESTPLAN_MAX_CLIPS) return false;
  auris_testplan_clip_t clip={0};
  char *where=strtok_r(NULL,SEPARATORS,saved);
  const char *test=strtok_r(NULL,SEPARATORS,saved);
  const char *id=strtok_r(NULL,SEPARATORS,saved);
  const char *distance=strtok_r(NULL,SEPARATORS,saved);
  const char *position=strtok_r(NULL,SEPARATORS,saved);
  const char *source=strtok_r(NULL,SEPARATORS,saved);
  const char *device=strtok_r(NULL,SEPARATORS,saved);
  const char *right=strtok_r(NULL,SEPARATORS,saved);
  const char *up=strtok_r(NULL,SEPARATORS,saved);
  const char *seconds=strtok_r(NULL,SEPARATORS,saved);
  const char *pictures=strtok_r(NULL,SEPARATORS,saved);
  if (!pictures || strtok_r(NULL,SEPARATORS,saved)) return false;
  if (!parse_where(plan,where,&clip.locations)) return false;
  float value=0.0f;
  if (!number(test,&value) || value<0.0f || value!=floorf(value)) return false;
  clip.test=(unsigned int)value;
  bool known=false;
  for (size_t i=0U;i<plan->test_count;++i) known=known || plan->tests[i].number==clip.test;
  if (!known) return false;
  if (!safe_name(id) || !copy_field(clip.id,sizeof(clip.id),id) ||
      !copy_field(clip.distance,sizeof(clip.distance),distance) ||
      !copy_field(clip.position,sizeof(clip.position),position) ||
      !copy_field(clip.source,sizeof(clip.source),source) ||
      !copy_field(clip.device,sizeof(clip.device),device)) return false;
  show_spaces(clip.distance); show_spaces(clip.position);
  show_spaces(clip.source); show_spaces(clip.device);
  if (strcmp(right,"-")==0 && strcmp(up,"-")==0) clip.has_target=false;
  else {
    if (!number(right,&clip.right_deg) || !number(up,&clip.up_deg) ||
        fabsf(clip.right_deg)>=80.0f || fabsf(clip.up_deg)>=80.0f) return false;
    clip.has_target=true;
  }
  if (!number(seconds,&clip.seconds) || clip.seconds<1.0f || clip.seconds>60.0f) return false;
  if (strcmp(pictures,"yes")==0) clip.pictures=true;
  else if (strcmp(pictures,"no")!=0) return false;
  for (size_t i=0U;i<plan->clip_count;++i)
    if (plan->clips[i].test==clip.test && strcmp(plan->clips[i].id,clip.id)==0) return false;
  plan->clips[plan->clip_count++]=clip;
  return true;
}
static bool parse_line(auris_testplan_t *plan,char *line) {
  char *saved=NULL;
  const char *word=strtok_r(line,SEPARATORS,&saved);
  if (!word || word[0]=='#') return true;
  if (strcmp(word,"location")==0) {
    const char *name=strtok_r(NULL,SEPARATORS,&saved);
    if (plan->location_count==AURIS_TESTPLAN_MAX_LOCATIONS || !name || !safe_name(name) ||
        strtok_r(NULL,SEPARATORS,&saved)) return false;
    return copy_field(plan->locations[plan->location_count++],sizeof(plan->locations[0]),name);
  }
  if (strcmp(word,"test")==0) {
    float value=0.0f;
    if (plan->test_count==AURIS_TESTPLAN_MAX_TESTS ||
        !number(strtok_r(NULL,SEPARATORS,&saved),&value) || value<0.0f ||
        value!=floorf(value)) return false;
    auris_testplan_test_t *test=&plan->tests[plan->test_count];
    test->number=(unsigned int)value;
    if (!copy_field(test->title,sizeof(test->title),strtok_r(NULL,SEPARATORS,&saved)) ||
        strtok_r(NULL,SEPARATORS,&saved)) return false;
    show_spaces(test->title);
    ++plan->test_count;
    return true;
  }
  if (strcmp(word,"clip")==0) return parse_clip(plan,&saved);
  return false;
}
int auris_testplan_parse(const char *text,auris_testplan_t *out,size_t *bad_line) {
  if (!text || !out) return -EINVAL;
  static auris_testplan_t plan;
  memset(&plan,0,sizeof(plan));
  size_t number_of_line=1U;
  while (*text) {
    const char *end=strchr(text,'\n');
    const size_t length=end ? (size_t)(end-text) : strlen(text);
    char line[256];
    if (length>=sizeof(line)) {
      if (bad_line) *bad_line=number_of_line;
      return -EINVAL;
    }
    memcpy(line,text,length);
    line[length]='\0';
    if (!parse_line(&plan,line)) {
      if (bad_line) *bad_line=number_of_line;
      return -EINVAL;
    }
    text+=length+(end ? 1U : 0U);
    ++number_of_line;
  }
  if (!plan.location_count || !plan.test_count || !plan.clip_count) {
    if (bad_line) *bad_line=number_of_line;
    return -EINVAL;
  }
  *out=plan;
  return 0;
}
int auris_testplan_load(const char *path,auris_testplan_t *plan,size_t *bad_line) {
  static char text[MAX_FILE];
  FILE *file=fopen(path,"r");
  if (!file) return -errno;
  const size_t bytes=fread(text,1U,sizeof(text)-1U,file);
  const bool failed=ferror(file) || bytes==sizeof(text)-1U;
  (void)fclose(file);
  if (failed) return -EINVAL;
  text[bytes]='\0';
  return auris_testplan_parse(text,plan,bad_line);
}
int auris_testplan_clip_dir(const auris_testplan_t *plan,size_t location,size_t clip,
                            const char *root,char *out,size_t capacity) {
  if (!plan || !root || !out || location>=plan->location_count ||
      clip>=plan->clip_count) return -EINVAL;
  const int n=snprintf(out,capacity,"%s/%s/test%u/%s",root,plan->locations[location],
                       plan->clips[clip].test,plan->clips[clip].id);
  return n<0 || (size_t)n>=capacity ? -ENAMETOOLONG : 0;
}
