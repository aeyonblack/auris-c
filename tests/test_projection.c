#ifdef AURIS_PROJECTION_AVAILABLE
#include "auris_projection.h"
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return EXIT_FAILURE; } } while (0)
#define CLOSE(a,b,e) (fabsf((a)-(b)) <= (e))

static auris_view_t view(size_t width, size_t height, float fx, float fy) {
  return (auris_view_t){width,height,fx,fy,(float)(width-1U)/2.0f,
      (float)(height-1U)/2.0f,{{1,0,0},{0,1,0},{0,0,1}}};
}

int main(void) {
  auris_view_t search=view(640U,480U,480.0f,480.0f);
  auris_view_t camera=view(1640U,1232U,1358.5007f,1356.8622f);
  auris_vec3f_t directions[65U*49U];
  auris_projected_point_t points[65U*49U];
  const auris_projection_layout_t layout={640U,360U,80U,0U,479U,360U};
  CHECK(auris_view_grid(&search,65U,49U,directions,65U*49U)==AURIS_OK);
  CHECK(auris_projection_build(&camera,&layout,directions,65U*49U,
                               points,65U*49U)==AURIS_OK);
  size_t visible=0U;
  for(size_t q=0U;q<65U*49U;++q) visible += points[q].visible ? 1U : 0U;
  CHECK(visible==2537U);
  const size_t centre=24U*65U+32U;
  CHECK(points[centre].visible);
  CHECK(CLOSE(points[centre].x,319.0f,1.0e-4f));
  CHECK(CLOSE(points[centre].y,179.5f,1.0e-4f));
  CHECK(!points[0].visible && !points[65U*49U-1U].visible);

  auris_vec3f_t ray;
  auris_projected_point_t point;
  CHECK(auris_view_ray(&camera,0.0f,0.0f,&ray)==AURIS_OK);
  CHECK(auris_projection_build(&camera,&layout,&ray,1U,&point,1U)==AURIS_OK);
  CHECK(point.visible && CLOSE(point.x,79.64604f,1.0e-3f) &&
        CLOSE(point.y,-0.35390f,1.0e-3f));
  CHECK(auris_view_ray(&camera,1639.0f,1231.0f,&ray)==AURIS_OK);
  CHECK(auris_projection_build(&camera,&layout,&ray,1U,&point,1U)==AURIS_OK);
  CHECK(point.visible && CLOSE(point.x,558.35396f,1.0e-3f) &&
        CLOSE(point.y,359.35390f,1.0e-3f));

  /* Use the actual resizer's sampling equation to select source rays.
     Projection must return their destination pixel centres. */
  for (size_t y=0U; y<360U; y+=17U) {
    for (size_t x=0U; x<479U; x+=19U) {
      const float source_u=((float)x+0.5f)*1640.0f/479.0f-0.5f;
      const float source_v=((float)y+0.5f)*1232.0f/360.0f-0.5f;
      CHECK(auris_view_ray(&camera,source_u,source_v,&ray)==AURIS_OK);
      CHECK(auris_projection_build(&camera,&layout,&ray,1U,&point,1U)==AURIS_OK);
      CHECK(point.visible && CLOSE(point.x,80.0f+(float)x,0.001f) &&
            CLOSE(point.y,(float)y,0.001f));
    }
  }

  float u,v;
  bool shown;
  ray=(auris_vec3f_t){0,0,3};
  CHECK(auris_view_project(&camera,ray,&u,&v,&shown)==AURIS_OK);
  CHECK(shown && CLOSE(u,819.5f,1.0e-5f) && CLOSE(v,615.5f,1.0e-5f));
  const float scales[]={FLT_MIN,1.0e-10f,1.0f,FLT_MAX};
  for (size_t i=0U;i<4U;++i) {
    ray=(auris_vec3f_t){0.25f*scales[i],0.125f*scales[i],scales[i]};
    CHECK(auris_view_project(&camera,ray,&u,&v,&shown)==AURIS_OK);
    CHECK(shown && CLOSE(u,1159.1252f,0.001f) && CLOSE(v,445.8922f,0.001f));
  }
  CHECK(auris_view_project(&camera,(auris_vec3f_t){0,0,0},&u,&v,&shown)==
        AURIS_ERROR_INVALID_ARGUMENT && !shown);
  CHECK(auris_view_project(NULL,(auris_vec3f_t){0,0,1},&u,&v,&shown)==
        AURIS_ERROR_INVALID_ARGUMENT);
  ray=(auris_vec3f_t){0,0,-1};
  CHECK(auris_view_project(&camera,ray,&u,&v,&shown)==AURIS_OK && !shown);
  ray=(auris_vec3f_t){1,0,0.1f};
  CHECK(auris_view_project(&camera,ray,&u,&v,&shown)==AURIS_OK && !shown);

  camera.rotation[0][0]=0.8660254f; camera.rotation[0][2]=0.5f;
  camera.rotation[2][0]=-0.5f; camera.rotation[2][2]=0.8660254f;
  ray=(auris_vec3f_t){0.5f,0,0.8660254f};
  CHECK(auris_view_project(&camera,ray,&u,&v,&shown)==AURIS_OK);
  CHECK(shown && CLOSE(u,819.5f,2.0e-4f) && CLOSE(v,615.5f,2.0e-4f));
  camera.rotation[0][0]=1; camera.rotation[0][2]=0;
  camera.rotation[2][0]=0; camera.rotation[2][2]=1;

  auris_projection_layout_t bad=layout;
  bad.content_width=561U;
  CHECK(auris_projection_build(&camera,&bad,directions,1U,points,1U)==
        AURIS_ERROR_INVALID_ARGUMENT);
  CHECK(auris_projection_build(&camera,&layout,directions,2U,points,1U)==
        AURIS_ERROR_CAPACITY);
  ray=(auris_vec3f_t){NAN,0,1};
  CHECK(auris_view_project(&camera,ray,&u,&v,&shown)==
        AURIS_ERROR_INVALID_ARGUMENT);
  camera.rotation[0][0]=-1;
  CHECK(auris_view_project(&camera,(auris_vec3f_t){0,0,1},&u,&v,&shown)==
        AURIS_ERROR_INVALID_ARGUMENT);
  camera.rotation[0][0]=1;
  const auris_projection_layout_t one={1,1,0,0,1,1};
  CHECK(auris_projection_build(&camera,&one,
        &(auris_vec3f_t){0,0,1},1U,&point,1U)==AURIS_OK);
  CHECK(point.visible && point.x==0.0f && point.y==0.0f);
  puts("Projection checks passed");
  return EXIT_SUCCESS;
}
#else
#include <stdio.h>
int main(void) {
  puts("Module 12 projection API not implemented yet");
  return 77;
}
#endif
