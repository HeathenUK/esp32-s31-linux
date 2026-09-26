#include "zgl.h"

static char *op_table_str[]=
{
#define ADD_OP(a,b,c) "gl" #a " " #c,

#include "opinfo.h"
};

static void (*op_table_func[])(GLContext *,GLParam *)=
{
#define ADD_OP(a,b,c) glop ## a ,

#include "opinfo.h"
};

static int op_table_size[]=
{
#define ADD_OP(a,b,c) b + 1 ,

#include "opinfo.h"
};


GLContext *gl_get_context(void)
{
  return gl_ctx;
}

/* s31: list names are 1..MAX_DISPLAY_LISTS-1; TinyGL indexed the table
   with any value the application passed */
static GLList *find_list(GLContext *c,unsigned int list)
{
  if (list == 0 || list >= MAX_DISPLAY_LISTS) return NULL;
  return c->shared_state.lists[list];
}

static void delete_list(GLContext *c,int list)
{
  GLParamBuffer *pb,*pb1;
  GLList *l;

  l=find_list(c,list);
  if (l == NULL) return;
  
  /* s31: pixel data the list copied at compile time */
  while (l->owned != NULL) {
    void *n = *(void **)l->owned;
    gl_free(l->owned);
    l->owned = n;
  }
  /* free param buffer */
  pb=l->first_op_buffer;
  while (pb!=NULL) {
    pb1=pb->next;
    gl_free(pb);
    pb=pb1;
  }
  
  gl_free(l);
  c->shared_state.lists[list]=NULL;
}

static GLList *alloc_list(GLContext *c,int list)
{
  GLList *l;
  GLParamBuffer *ob;

  l=gl_zalloc(sizeof(GLList));
  ob=gl_zalloc(sizeof(GLParamBuffer));
  if (l == NULL || ob == NULL) {
    gl_free(l); gl_free(ob);
    gl_set_error(c, GL_OUT_OF_MEMORY);
    return NULL;
  }

  ob->next=NULL;
  l->first_op_buffer=ob;
  
  ob->ops[0].op=OP_EndList;

  c->shared_state.lists[list]=l;
  return l;
}


void gl_print_op(FILE *f,GLParam *p)
{
  int op;
  char *s;

  op=p[0].op;
  p++;
  s=op_table_str[op];
  while (*s != 0) {
    if (*s == '%') {
      s++;
      switch (*s++) {
      case 'f':
	fprintf(f,"%g",p[0].f);
	break;
      default:
	fprintf(f,"%d",p[0].i);
	break;
      }
      p++;
    } else {
      fputc(*s,f);
      s++;
    }
  }
  fprintf(f,"\n");
}


void gl_compile_op(GLContext *c,GLParam *p)
{
  int op,op_size;
  GLParamBuffer *ob,*ob1;
  int index,i;

  op=p[0].op;
  op_size=op_table_size[op];
  index=c->current_op_buffer_index;
  ob=c->current_op_buffer;

  /* we should be able to add a NextBuffer opcode */
  if ((index + op_size) > (OP_BUFFER_MAX_SIZE-2)) {

    ob1=gl_zalloc(sizeof(GLParamBuffer));
    if (ob1 == NULL) {
      /* s31: the rest of the list is lost, the application is not */
      gl_set_error(c, GL_OUT_OF_MEMORY);
      return;
    }
    ob1->next=NULL;

    ob->next=ob1;
    ob->ops[index].op=OP_NextBuffer;
    ob->ops[index+1].p=(void *)ob1;

    c->current_op_buffer=ob1;
    ob=ob1;
    index=0;
  }

  for(i=0;i<op_size;i++) {
    ob->ops[index]=p[i];
    index++;
  }
  c->current_op_buffer_index=index;
}

/* s31: blocks carry their chain pointer in a header of their own
   (s31_list_block below), so the op keeps a plain pointer to the data */
int gl_list_own(GLContext *c, void *block)
{
  GLList *l;
  if (!c->compile_flag || block == NULL) return 0;
  l = find_list(c, c->list_index);
  if (l == NULL) return 0;
  *(void **)block = l->owned;
  l->owned = block;
  return 1;
}

void gl_add_op(GLParam *p)
{
  GLContext *c=gl_get_context();
  int op;

  op=p[0].op;
  if (c->exec_flag) {
    op_table_func[op](c,p);
  }
  if (c->compile_flag) {
    gl_compile_op(c,p);
  }
  if (c->print_flag) {
    gl_print_op(stderr,p);
  }
}

/* this opcode is never called directly */
void glopEndList(GLContext *c,GLParam *p)
{
  assert(0);
}

/* this opcode is never called directly */
void glopNextBuffer(GLContext *c,GLParam *p)
{
  assert(0);
}


void glopCallList(GLContext *c,GLParam *p)
{
  GLList *l;
  int list,op;
  static int depth;   /* GL_MAX_LIST_NESTING; a list may call itself */

  list=p[1].ui;
  l=find_list(c,list);
  /* s31: GL ignores an undefined list; TinyGL exited */
  if (l == NULL) return;
  if (depth >= 64) return;
  depth++;
  p=l->first_op_buffer->ops;

  while (1) {
    op=p[0].op;
    if (op == OP_EndList) break;
    if (op == OP_NextBuffer) {
      p=(GLParam *)p[1].p;
    } else {
      op_table_func[op](c,p);
      p+=op_table_size[op];
    }
  }
  depth--;
}

void glDeleteLists(unsigned int list, int range)
{
	GLList *l;
	int i;
	GLContext *c = gl_get_context();

	if (range < 0) { gl_set_error(c, GL_INVALID_VALUE); return; }

	for (i = list; i < list + range; i++)
	{
		l = find_list(c, i);
		if (l != NULL) delete_list(c, i);
	}
}

void glNewList(unsigned int list, int mode)
{
  GLList *l;
  GLContext *c=gl_get_context();

  if (list == 0) { gl_set_error(c, GL_INVALID_VALUE); return; }
  if (mode != GL_COMPILE && mode != GL_COMPILE_AND_EXECUTE) {
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  if (c->compile_flag || c->in_begin) {
    gl_set_error(c, GL_INVALID_OPERATION);
    return;
  }
  if (list >= MAX_DISPLAY_LISTS) {
    /* s31: TinyGL's table is fixed; a real implementation has no limit */
    gl_warn_once("display list names >= 1024");
    gl_set_error(c, GL_OUT_OF_MEMORY);
    return;
  }

  l=find_list(c,list);
  if (l!=NULL) delete_list(c,list);
  l=alloc_list(c,list);
  if (l == NULL) return;
  c->list_index=list;
  c->list_mode=mode;

  c->current_op_buffer=l->first_op_buffer;
  c->current_op_buffer_index=0;
  
  c->compile_flag=1;
  c->exec_flag=(mode == GL_COMPILE_AND_EXECUTE);
}

void glEndList(void)
{
  GLContext *c=gl_get_context();
  GLParam p[1];

  if (c->compile_flag != 1) {
    gl_set_error(c, GL_INVALID_OPERATION);
    return;
  }
  
  /* end of list */
  p[0].op=OP_EndList;
  gl_compile_op(c,p);
  
  c->compile_flag=0;
  c->exec_flag=1;
  c->list_index=0;
  c->list_mode=0;
}

int glIsList(unsigned int list)
{
  GLContext *c=gl_get_context();
  GLList *l;
  l=find_list(c,list);
  return (l != NULL);
}

unsigned int glGenLists(int range)
{
  GLContext *c=gl_get_context();
  int count,i,list;
  GLList **lists;

  if (range < 0) { gl_set_error(c, GL_INVALID_VALUE); return 0; }
  if (range == 0) return 0;
  lists=c->shared_state.lists;
  count=0;
  /* s31: from 1 - list 0 is not a name (0 is glGenLists' failure value) */
  for(i=1;i<MAX_DISPLAY_LISTS;i++) {
    if (lists[i]==NULL) {
      count++;
      if (count == range) {
	list=i-range+1;
	for(i=0;i<range;i++) {
	  alloc_list(c,list+i);
	}
	return list;
      }
    } else {
      count=0;
    }
  }
  return 0;
}

