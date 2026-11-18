#ifndef AURIS_TESTPLAN_H
#define AURIS_TESTPLAN_H
#include <stdbool.h>
#include <stddef.h>
/* The qualification tests as a text file, so that a clip's name, setup and
   length are never typed in or remembered on site. See configs/test_plan.txt. */
#define AURIS_TESTPLAN_MAX_LOCATIONS 4U
#define AURIS_TESTPLAN_MAX_TESTS 8U
#define AURIS_TESTPLAN_MAX_CLIPS 64U
#define AURIS_TESTPLAN_FIELD 16U
typedef struct {
  unsigned int locations; /* one bit per plan location */
  unsigned int test;      /* test number */
  char id[32];            /* the clip's folder name */
  /* Shown on screen, with underscores turned into spaces. */
  char distance[AURIS_TESTPLAN_FIELD], position[AURIS_TESTPLAN_FIELD];
  char source[AURIS_TESTPLAN_FIELD], device[AURIS_TESTPLAN_FIELD];
  bool has_target;        /* false: no place to mark in the picture */
  float right_deg, up_deg;
  float seconds;          /* how long the source plays */
  bool pictures;          /* false: log only, for timing measurements */
} auris_testplan_clip_t;
typedef struct { unsigned int number; char title[32]; } auris_testplan_test_t;
typedef struct {
  char locations[AURIS_TESTPLAN_MAX_LOCATIONS][20];
  size_t location_count;
  auris_testplan_test_t tests[AURIS_TESTPLAN_MAX_TESTS];
  size_t test_count;
  auris_testplan_clip_t clips[AURIS_TESTPLAN_MAX_CLIPS];
  size_t clip_count;
} auris_testplan_t;
/* All-or-nothing. Returns 0, -EINVAL with *bad_line set for a bad line, or
   -errno if the file cannot be read. bad_line may be NULL. */
int auris_testplan_parse(const char *text,auris_testplan_t *plan,size_t *bad_line);
int auris_testplan_load(const char *path,auris_testplan_t *plan,size_t *bad_line);
/* root/location/testN/id. Returns 0 or -ENAMETOOLONG. */
int auris_testplan_clip_dir(const auris_testplan_t *plan,size_t location,size_t clip,
                            const char *root,char *out,size_t capacity);
#endif
