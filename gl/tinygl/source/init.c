#include <stdlib.h>
#include "zgl.h"
#include "s31_tex8.h"
#include "s31_ramtext.h"

GLContext *gl_ctx;


void initSharedState(GLContext *c)
{
  GLSharedState *s=&c->shared_state;
  s->lists=gl_zalloc(sizeof(GLList *) * MAX_DISPLAY_LISTS);
  s->texture_hash_table=
      gl_zalloc(sizeof(GLTexture *) * TEXTURE_HASH_TABLE_SIZE);
  s->refs=gl_zalloc(sizeof(int));
  *s->refs=1;

  alloc_texture(c,0);
}

/* s31: frees everything once the last sharing context goes (TinyGL leaked
   every display list and every texture chain past the first) */
void endSharedState(GLContext *c)
{
  GLSharedState *s=&c->shared_state;
  int i,j;

  if (s->refs && --*s->refs > 0) return;

  for(i=0;i<MAX_DISPLAY_LISTS;i++) {
    GLList *l=s->lists[i];
    if (l != NULL) {
      GLParamBuffer *pb=l->first_op_buffer, *pb1;
      while (pb != NULL) { pb1=pb->next; gl_free(pb); pb=pb1; }
      gl_free(l);
    }
  }
  gl_free(s->lists);

  for(i=0;i<TEXTURE_HASH_TABLE_SIZE;i++) {
    GLTexture *t=s->texture_hash_table[i], *t1;
    while (t != NULL) {
      t1=t->next;
      for (j=0;j<TGL_STORED_LEVELS;j++)
        if (t->images[j].pixmap) gl_free(t->images[j].pixmap);
      gl_tex_free_mip(t);         /* s31: the mipmap levels (phase 4) */
      gl_free(t);
      t=t1;
    }
  }
  gl_free(s->texture_hash_table);
  gl_free(s->refs);
}


void glInit(void *zbuffer1)
{
  ZBuffer *zbuffer=(ZBuffer *)zbuffer1;
  GLContext *c;
  GLViewport *v;
  int i;

  c=gl_zalloc(sizeof(GLContext));
  gl_ctx=c;

  c->zb=zbuffer;

  /* allocate GLVertex array */
  c->vertex_max = POLYGON_MAX_VERTEX;
  c->vertex = gl_malloc(POLYGON_MAX_VERTEX*sizeof(GLVertex));
  
  /* viewport */
  v=&c->viewport;
  v->xmin=0;
  v->ymin=0;
  v->xsize=zbuffer->xsize;
  v->ysize=zbuffer->ysize;
  v->updated=1;

  /* shared state */
  initSharedState(c);

  /* lists */

  c->exec_flag=1;
  c->compile_flag=0;
  c->print_flag=0;
  gl_update_vtx_run(c);

  c->in_begin=0;

  /* lights */
  for(i=0;i<MAX_LIGHTS;i++) {
    GLLight *l=&c->lights[i];
    l->ambient=gl_V4_New(0,0,0,1);
    /* s31 (phase 3a): GL 1.3 table 6.9 - LIGHT0's diffuse and specular are
       (1,1,1,1), every other light's (0,0,0,1). TinyGL gave all of them
       LIGHT0's, so enabling GL_LIGHT1 without setting its colours lit the
       scene white where Mesa adds nothing (gl/tests/glx_geo.c row 3) */
    l->diffuse=i == 0 ? gl_V4_New(1,1,1,1) : gl_V4_New(0,0,0,1);
    l->specular=l->diffuse;
    l->has_specular=i == 0;
    l->position=gl_V4_New(0,0,1,0);
    l->norm_position=gl_V3_New(0,0,1);
    l->spot_direction=gl_V3_New(0,0,-1);
    l->norm_spot_direction=gl_V3_New(0,0,-1);
    l->spot_exponent=0;
    l->spot_cutoff=180;
    l->attenuation[0]=1;
    l->attenuation[1]=0;
    l->attenuation[2]=0;
    l->enabled=0;
  }
  c->first_light=NULL;
  c->light_dirty=1;   /* s31: light.c gl_light_products */
  c->ambient_light_model=gl_V4_New(0.2,0.2,0.2,1);
  c->local_light_model=0;
  c->lighting_enabled=0;
  c->light_model_two_side = 0;

  /* default materials */
  for(i=0;i<2;i++) {
    GLMaterial *m=&c->materials[i];
    m->emission=gl_V4_New(0,0,0,1);
    m->ambient=gl_V4_New(0.2,0.2,0.2,1);
    m->diffuse=gl_V4_New(0.8,0.8,0.8,1);
    m->specular=gl_V4_New(0,0,0,1);
    m->shininess=0;
    m->do_specular=0;       /* s31: specular 0 */
    m->specbuf=NULL;
  }
  c->current_color_material_mode=GL_FRONT_AND_BACK;
  c->current_color_material_type=GL_AMBIENT_AND_DIFFUSE;
  c->color_material_enabled=0;

  /* textures */
  glInitTextures(c);

  /* default state */
  c->current_color.X=1.0;
  c->current_color.Y=1.0;
  c->current_color.Z=1.0;
  c->current_color.W=1.0;
  c->longcurrent_color[0] = 65535;
  c->longcurrent_color[1] = 65535;
  c->longcurrent_color[2] = 65535;

  c->current_normal.X=1.0;
  c->current_normal.Y=0.0;
  c->current_normal.Z=0.0;
  c->current_normal.W=0.0;

  c->current_edge_flag=1;
  
  c->current_tex_coord.X=0;
  c->current_tex_coord.Y=0;
  c->current_tex_coord.Z=0;
  c->current_tex_coord.W=1;

  c->polygon_mode_front=GL_FILL;
  c->polygon_mode_back=GL_FILL;

  c->current_front_face=0; /* 0 = GL_CCW  1 = GL_CW */
  c->current_cull_face=GL_BACK;
  c->current_shade_model=GL_SMOOTH;
  c->cull_face_enabled=0;
  
  /* clear */
  c->clear_color.v[0]=0;
  c->clear_color.v[1]=0;
  c->clear_color.v[2]=0;
  c->clear_color.v[3]=0;
  c->clear_depth=1.0f;   /* s31: GL's default (see clear.c) */

  /* selection */
  c->render_mode=GL_RENDER;
  c->select_buffer=NULL;
  c->name_stack_size=0;

  /* matrix */
  c->matrix_mode=0;
  
  c->matrix_stack_depth_max[0]=MAX_MODELVIEW_STACK_DEPTH;
  c->matrix_stack_depth_max[1]=MAX_PROJECTION_STACK_DEPTH;
  c->matrix_stack_depth_max[2]=MAX_TEXTURE_STACK_DEPTH;
  c->matrix_stack_depth_max[3]=MAX_TEXTURE_STACK_DEPTH;   /* s31: unit 1's */

  for(i=0;i<4;i++) {
    c->matrix_stack[i]=gl_zalloc(c->matrix_stack_depth_max[i] * sizeof(M4));
    c->matrix_stack_ptr[i]=c->matrix_stack[i];
  }

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_TEXTURE);
  glLoadIdentity();
  gl_M4_Id(c->matrix_stack[3]);     /* s31 (phase 5 O1): unit 1's */
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  c->matrix_model_projection_updated=1;
  c->xf_dirty=-1;   /* s31 (phase 3a G14) */

  /* opengl 1.1 arrays */
  c->client_states = 0;
  
  /* opengl 1.1 polygon offset */
  c->offset_states = 0;
  
  /* clear the resize callback function pointer */
  c->gl_resize_viewport = NULL;
  
  /* specular buffer */
  c->specbuf_first = NULL;
  c->specbuf_used_counter = 0;
  c->specbuf_num_buffers = 0;

  /* depth test */
  c->depth_test = 0;

  /* s31 state */
  s31_state_init(c);
  gl_mtex_init(c);          /* phase 5 O1: texture unit 1 (s31_mtex.c) */
  /* s31 (phase 4, s31_tfilter.c): the general path's per-triangle state;
     S31GL_MIPMAPS=0 keeps levels > 0 unstored (the mipmap filters then
     sample level 0) */
  c->pipe.x = &c->pipex;
  c->pipe.xact = 0;
  c->pipex.slot_col = NULL;
  c->pipex.tf[0].slot = c->pipex.tf[1].slot = NULL;
  {
    /* S31GL_TEXFILTER=0: nearest in level 0 for every filter, and no
       level > 0 stored (phase 4 review: the pre-phase-4 behaviour, for
       the board A/B of filtered apps such as QuakeSpasm) */
    const char *e, *f = getenv("S31GL_TEXFILTER");
#ifndef S31GL_TEXFILTER_DEFAULT
#define S31GL_TEXFILTER_DEFAULT 1      /* gl/bench: -DS31GL_TEXFILTER_DEFAULT=0 */
#endif
    c->tex_filter = f ? atoi(f) != 0 : S31GL_TEXFILTER_DEFAULT;
    /* S31GL_PERSPCOLOR=0: Gouraud colour stays screen-affine (read here
       once, not in gl_update_raster: review 4 R4-hot) */
    e = getenv("S31GL_PERSPCOLOR");
    c->pc_enable = e ? atoi(e) != 0 : 1;
    e = getenv("S31GL_MIPMAPS");
    c->mip_store = c->tex_filter && (e ? atoi(e) != 0 : 1);
    /* phase 5: texel storage (s31_tex8.c) */
    c->tex8 = gl_tex8_knob();
    e = getenv("S31GL_FILT8");
    c->filt8 = e ? atoi(e) != 0 : 0;
  }
}

void glClose(void)
{
  int i;
  GLContext *c=gl_get_context();
  GLSpecBuf *b,*b1;
  endSharedState(c);

  gl_free(c->vertex);
  gl_free(c->vc);   /* s31 (phase 3a G14): arrays.c vertex cache (one block) */
  gl_free(c->pipex.stab);   /* phase 4 F8: the stencil table (raster_sel.c) */
  c->pipex.stab = NULL;
  gl_free(c->pipex.wtab);   /* phase 5 O1: zpipe_fused.c's tables */
  c->pipex.wtab = NULL;
  gl_free(c->pipex.btab);   /* phase 5 O2: its blend tables */
  c->pipex.btab = NULL;

  for(i=0;i<4;i++) {
    gl_free(c->matrix_stack[i]);
  }
  gl_mtex_free(c);          /* phase 5 O1 */
  free_texture_detached(c->tex1d_default);   /* s31: the 1D default object */
  gl_free(c->pixpipe);                       /* s31: s31_draw.c's cache */
  /* s31: the specular tables were leaked */
  for (b=c->specbuf_first; b != NULL; b=b1) { b1=b->next; gl_free(b); }

  gl_free(c);
}
