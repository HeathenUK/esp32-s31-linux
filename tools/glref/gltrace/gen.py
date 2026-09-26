#!/usr/bin/env python3
"""gen.py - code generator for the GL call tracer (tools/glref/gltrace).

Parses the prototypes of gl/include/GL/gl.h, glext.h and glx.h (the headers
our libGL is built against) and writes:

  gen.py wrap EXPORTS OUT.c
      The recording half of the host tracer: one wrapper per name our
      libGL.so.1 exports (EXPORTS: `nm -D --defined-only` lines, "T name"),
      so the tracer library exports exactly what the real one does - no
      more (an extra export would change what SDL or the app finds), no
      less (LD_BIND_NOW would refuse the app). gl* names with a prototype
      get a C wrapper that records the call; glX* names get a wrapper that
      records the call's name (replay ignores them; the ones that matter -
      MakeCurrent, SwapBuffers, GetProcAddress, CreateContext - are
      hand-written in gltrace_rt.c); anything else (s31gl_*, names without
      a prototype) is an aarch64 tail-jump trampoline to the real symbol.

  gen.py dispatch NAMES OUT.c [--mode direct|table|null]
      The replay half: a function per name in NAMES (one per line; the
      gl* entry points the library under test defines) that decodes one
      record and calls the entry point. direct: call by name (the RV32
      bench, linked with the library objects); table: call through a
      pointer table filled at start (the host replayer: glXGetProcAddress /
      dlsym, so Mesa and ours are both reachable); null: call an empty
      noinline function with the same signature (the harness floor: what
      the replay itself costs).

Record layout (little endian, 32-bit words; gltrace.h has the constants):
  word 0     id | nwords << 12   (nwords == 0xFFFFF: word 1 is nwords)
  scalars    every non-pointer parameter in order: 1 word, or 2 for
             GLdouble / GLclampd / GLint64 / GLuint64
  pointers   every pointer parameter in order: a length word (0xFFFFFFFF =
             NULL), then for input data that many bytes padded to a word;
             an output pointer has only its length (the replay passes
             scratch memory of that size), except glGen* names, whose
             returned values follow (the replay remaps texture names)
  extra      glGenLists: its return value

s31, MIT.
"""
import re
import sys

# GLX entry points gltrace_rt.c implements itself (they carry the frame,
# context and GetProcAddress logic); they only get an id here
HAND = {'glXMakeCurrent', 'glXMakeContextCurrent', 'glXSwapBuffers', 'glXGetProcAddress',
        'glXGetProcAddressARB', 'glXCreateContext', 'glXCreateNewContext', 'glXDestroyContext'}
TYPES8 = {'GLdouble', 'GLclampd', 'GLint64', 'GLuint64', 'GLint64EXT', 'GLuint64EXT'}
TYPESF = {'GLfloat', 'GLclampf'}
SCALAR_OK = {
    'GLenum', 'GLboolean', 'GLbitfield', 'GLbyte', 'GLshort', 'GLint', 'GLsizei',
    'GLubyte', 'GLushort', 'GLuint', 'GLfloat', 'GLclampf', 'GLdouble', 'GLclampd',
    'GLint64', 'GLuint64', 'GLint64EXT', 'GLuint64EXT', 'GLintptr', 'GLsizeiptr',
    'GLintptrARB', 'GLsizeiptrARB', 'GLhandleARB', 'GLhalfNV', 'GLfixed', 'GLclampx',
    'GLchar', 'GLcharARB', 'GLhalf', 'GLhalfARB', 'GLvdpauSurfaceNV',
    'int', 'unsigned int', 'Bool', 'unsigned long', 'long',
}


def strip_comments(t):
    t = re.sub(r'/\*.*?\*/', ' ', t, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', t)


def parse_params(s):
    s = ' '.join(s.split())
    if s in ('', 'void'):
        return []
    out = []
    for i, p in enumerate(s.split(',')):
        p = p.strip()
        arr = None
        m = re.match(r'(.*?)\s*\[(\w*)\]$', p)
        if m:
            p, arr = m.group(1), m.group(2) or '1'
        m = re.match(r'(.*?[\s\*])(\w+)$', p)
        if not m or m.group(1).strip() in ('', 'const'):
            typ, name = p, 'p%d' % i
        else:
            typ, name = m.group(1).strip(), m.group(2)
        if arr:
            typ = typ + ' *'
        out.append((' '.join(typ.replace('*', ' * ').split()).replace(' *', '*'), name))
    return out


def parse(glh, glext, glx):
    protos = {}
    t = strip_comments(open(glh).read() + '\n' + open(glext).read())
    for m in re.finditer(r'GLAPI\s+([^;{}()]*?)\b(?:GLAPIENTRY|APIENTRY)\s+(gl\w+)\s*\(([^()]*)\)\s*;', t):
        ret, name, ps = m.group(1), m.group(2), m.group(3)
        ret = ' '.join(ret.replace('GLAPI', ' ').split())
        protos.setdefault(name, (ret, parse_params(ps)))
    t = strip_comments(open(glx).read())
    for m in re.finditer(r'extern\s+([^;{}()]*?)\b(glX\w+)\s*\(([^()]*)\)\s*;', t):
        ret, name, ps = ' '.join(m.group(1).split()), m.group(2), m.group(3)
        protos.setdefault(name, (ret, parse_params(ps)))
    return protos


def base(typ):
    return typ.replace('const', '').replace('*', '').strip()


def is_ptr(typ):
    return '*' in typ


def is_const_ptr(typ):
    return is_ptr(typ) and typ.startswith('const')


# ------------------------------------------------------------ size rules
# For an INPUT pointer: a C expression (in the wrapper, over the parameter
# names) for the bytes the GL reads. For an OUTPUT pointer: the scratch
# bytes the replay must provide. None: not handled (the call is recorded as
# UNHANDLED and the capture fails).

PNAME_FNS = re.compile(r'^gl(Fog|Light|LightModel|Material|TexEnv|TexParameter|TexParameterI|TexGen|'
                       r'PointParameter|ColorTableParameter|ConvolutionParameter|MultiTexEnv|'
                       r'MultiTexParameter|MultiTexGen|TextureParameter|SamplerParameter|SamplerParameterI)'
                       r'(f|i|d|x|ui|I|Iui)v(ARB|EXT|OES|NV|SGIS)?$')
VEC_FNS = re.compile(r'^gl(Vertex|Color|TexCoord|MultiTexCoord|Normal|RasterPos|WindowPos|SecondaryColor|'
                     r'EvalCoord|FogCoord|Index|VertexAttrib|Uniform)([1-4])?(b|s|i|f|d|ub|us|ui|x)v'
                     r'(ARB|EXT|MESA|OES|NV)?$')
IMG_FNS = {
    # name: (w, h, d, format, type) parameter names
    'glTexImage1D': ('width', '1', '1', 'format', 'type'),
    'glTexImage2D': ('width', 'height', '1', 'format', 'type'),
    'glTexImage3D': ('width', 'height', 'depth', 'format', 'type'),
    'glTexSubImage1D': ('width', '1', '1', 'format', 'type'),
    'glTexSubImage2D': ('width', 'height', '1', 'format', 'type'),
    'glTexSubImage3D': ('width', 'height', 'depth', 'format', 'type'),
    'glDrawPixels': ('width', 'height', '1', 'format', 'type'),
    'glColorTable': ('width', '1', '1', 'format', 'type'),
    'glColorSubTable': ('count', '1', '1', 'format', 'type'),
    'glConvolutionFilter1D': ('width', '1', '1', 'format', 'type'),
    'glConvolutionFilter2D': ('width', 'height', '1', 'format', 'type'),
}
for k in list(IMG_FNS):
    for suf in ('EXT', 'ARB'):
        IMG_FNS[k + suf] = IMG_FNS[k]
# functions whose pointer is a deferred client-array pointer or needs
# semantics the tracer does not model: always UNHANDLED
UNHANDLED = re.compile(r'^gl(\w*Pointer\w*|InterleavedArrays|ArrayElement\w*|Map[12][fd]|'
                       r'ShaderSource\w*|DebugMessageCallback\w*|MultiDraw\w*|DrawArrays\w*|'
                       r'BufferData\w*|BufferSubData\w*|'
                       r'CompressedTex\w*)$')
# functions with client-array semantics that are recorded but must be
# checked for enabled arrays at record time (gltrace_rt.c: tr_arrays_off)
ARRAY_DRAWS = {'glDrawElements', 'glDrawRangeElements', 'glDrawArrays',
               'glDrawElementsEXT', 'glDrawRangeElementsEXT', 'glDrawArraysEXT'}


def in_size(name, params, idx):
    typ, pn = params[idx]
    b = base(typ)
    esz = 'sizeof(%s)' % b if b not in ('void', 'GLvoid') else '1'
    if name in IMG_FNS:
        w, h, d, f, ty = IMG_FNS[name]
        return 'tr_image_bytes(%s, %s, %s, %s, %s, 0)' % (w, h, d, f, ty)
    if name == 'glBitmap':
        return 'tr_bitmap_bytes(width, height, 0)'
    if name == 'glPolygonStipple':
        return 'tr_bitmap_bytes(32, 32, 0)'
    if name in ('glCallLists',):
        return 'n * tr_type_bytes(type)'
    if name in ('glDrawElements', 'glDrawElementsEXT'):
        return 'count * tr_type_bytes(type)'
    if name in ('glDrawRangeElements', 'glDrawRangeElementsEXT'):
        return 'count * tr_type_bytes(type)'
    if re.match(r'^gl(Load|Mult)(Transpose)?Matrix[fdx](ARB|EXT)?$', name):
        return '16 * %s' % esz
    if name == 'glClipPlane':
        return '4 * sizeof(GLdouble)'
    if re.match(r'^glRect[dfisx]v$', name):
        return '2 * %s' % esz
    if name in ('glEdgeFlagv',):
        return '1'
    if re.match(r'^gl(DeleteTextures|DeleteTexturesEXT|PrioritizeTextures|PrioritizeTexturesEXT|'
                     r'AreTexturesResident|AreTexturesResidentEXT|DeleteBuffers\w*|DeleteFramebuffers\w*|'
                     r'DeleteRenderbuffers\w*|DeleteQueries\w*|DeleteVertexArrays\w*|DeleteSamplers|'
                     r'DeleteProgramsARB|DeleteFencesNV)$', name):
        return 'n * %s' % esz
    m = re.match(r'^glPixelMap(fv|uiv|usv)$', name)
    if m:
        return 'mapsize * %s' % esz
    if base(typ) in ('GLchar', 'char', 'GLcharARB'):
        return 'strlen((const char *)%s) + 1' % pn
    if name in ('glDrawBuffers', 'glDrawBuffersARB'):
        return 'n * %s' % esz
    m = re.match(r'^glUniform([1-4])(f|i|ui)v(ARB)?$', name)
    if m:
        return 'count * %s * %s' % (m.group(1), esz)
    m = re.match(r'^glUniformMatrix([2-4])fv(ARB)?$', name)
    if m:
        return 'count * %s * %s * %s' % (m.group(1), m.group(1), esz)
    if re.match(r'^glVertexAttrib4N(b|s|i|ub|us|ui)v(ARB)?$', name):
        return '4 * %s' % esz
    m = PNAME_FNS.match(name)
    if m and any(p[1] == 'pname' for p in params):
        return 'tr_pname_count(pname) * %s' % esz
    m = VEC_FNS.match(name)
    if m:
        n = m.group(2) or '1'
        if m.group(1) == 'Uniform':
            return None
        return '%s * %s' % (n, esz)
    return None


def out_size(name, params, idx):
    typ, pn = params[idx]
    b = base(typ)
    esz = 'sizeof(%s)' % b if b not in ('void', 'GLvoid') else '1'
    if name == 'glReadPixels':
        return 'tr_image_bytes(width, height, 1, format, type, 1)'
    if name in ('glGetTexImage', 'glGetTexImageEXT'):
        return 'tr_teximage_bytes(target, level, format, type)'
    if name == 'glGetPolygonStipple':
        return 'tr_bitmap_bytes(32, 32, 1)'
    if re.match(r'^glGen(Textures|TexturesEXT|Buffers\w*|Framebuffers\w*|Renderbuffers\w*|Queries\w*|'
                r'VertexArrays\w*|Samplers|ProgramsARB|FencesNV)$', name):
        return 'n * %s' % esz
    if name in ('glAreTexturesResident', 'glAreTexturesResidentEXT'):
        return 'n'
    if name == 'glSelectBuffer':
        return 'size * sizeof(GLuint)'
    if name == 'glFeedbackBuffer':
        return 'size * sizeof(GLfloat)'
    if name.startswith('glGet'):
        return '4096'     # every glGet*v fits (a 4x4 double matrix is 128)
    return None


def is_gen(name):
    return re.match(r'^glGen(Textures|TexturesEXT)$', name) is not None


# record classes for the tracer's state-only frames (see gltrace_rt.c)
def call_class(name):
    if re.match(r'^glVertex[234]', name) or re.match(r'^glEvalPoint|^glEvalCoord|^glArrayElement', name):
        return 'TRC_VERTEX'
    if re.match(r'^glColor[34]', name):
        return 'TRC_COLOR'
    if re.match(r'^glSecondaryColor3', name):
        return 'TRC_SECCOLOR'
    if re.match(r'^glTexCoord[1-4]', name):
        return 'TRC_TEXCOORD'
    if re.match(r'^glMultiTexCoord[1-4]', name):
        return 'TRC_MTEXCOORD'
    if re.match(r'^glNormal3', name):
        return 'TRC_NORMAL'
    if re.match(r'^glFogCoord', name):
        return 'TRC_FOGCOORD'
    if re.match(r'^glEdgeFlag', name) and 'Pointer' not in name:
        return 'TRC_EDGE'
    if re.match(r'^glIndex[dfisu]', name) and 'Pointer' not in name:
        return 'TRC_INDEX'
    if name == 'glBegin':
        return 'TRC_BEGIN'
    if name == 'glEnd':
        return 'TRC_END'
    if re.match(r'^gl(Clear|ClearIndex)$', name) or name in ARRAY_DRAWS or re.match(
            r'^gl(DrawPixels|CopyPixels|ReadPixels|Rect[dfis]v?|CopyTexImage[12]D\w*|CopyTexSubImage[123]D\w*|'
            r'Accum|EvalMesh[12])$', name):
        return 'TRC_DRAW'
    if (name.startswith('glGet') and name != 'glGetError') or re.match(r'^glIs[A-Z]|^glAreTexturesResident', name):
        return 'TRC_QUERY'
    return 'TRC_KEEP'


def is_tex_call(name):
    """allocations made inside these are counted as texture memory (replay)"""
    return re.search(r'Tex(Image|SubImage|ture|Parameter)|Textures|Mipmap|ColorTable', name) is not None and \
        not name.startswith('glGet') and not name.startswith('glIs')


def cdecl(typ, pn):
    return '%s %s' % (typ, pn)


def scalar_words(typ):
    return 2 if base(typ) in TYPES8 else 1


def wrap(exports_path, out_path, protos):
    names = []
    for l in open(exports_path):
        f = l.split()
        if len(f) >= 2 and f[-2] in ('T', 'W', 'i'):
            names.append(f[-1])
    names = sorted(set(names))
    o = []
    o.append('/* generated by tools/glref/gltrace/gen.py wrap - do not edit */')
    o.append('#include "gltrace_rt.h"')
    tramp, recorded, glxw, unh_all = [], [], [], []
    ids = {}
    for n in names:
        if n.startswith('gl') and not n.startswith('glX') and n in protos:
            ids[n] = len(ids)
    glx_ids = {}
    for n in names:
        if n.startswith('glX') and (n in protos or n in HAND):
            glx_ids[n] = len(ids) + len(glx_ids)
    allids = dict(ids)
    allids.update(glx_ids)
    order = sorted(allids, key=lambda k: allids[k])
    o.append('const char *const tr_names[] = {')
    for n in order:
        o.append('\t"%s",' % n)
    o.append('};')
    o.append('const int tr_nnames = %d;' % len(order))
    o.append('const unsigned char tr_class[] = {')
    for n in order:
        o.append('\t%s, /* %s */' % (call_class(n) if n in ids else 'TRC_GLX', n))
    o.append('};')
    # real pointers, resolved in tr_init
    o.append('void *tr_real[%d];' % len(order))
    for n in names:
        if n in allids:
            continue
        tramp.append(n)
    o.append('void *tr_tramp_real[%d];' % max(1, len(tramp)))
    o.append('const char *const tr_tramp_names[] = {')
    for n in tramp:
        o.append('\t"%s",' % n)
    o.append('\t0 };')
    o.append('const int tr_ntramp = %d;' % len(tramp))
    o.append('')
    for n in order:
        if n in HAND:
            o.append('/* %s: hand-written in gltrace_rt.c */' % n)
            continue
        ret, params = protos[n]
        i = allids[n]
        pl = ', '.join(cdecl(t, p) for t, p in params) or 'void'
        al = ', '.join(p for t, p in params)
        fpt = '%s (*)(%s)' % (ret, ', '.join(t for t, p in params) or 'void')
        call = '((%s)tr_real[%d])(%s)' % (fpt, i, al)
        isvoid = ret == 'void'
        if n.startswith('glX'):
            o.append('TR_EXPORT %s %s(%s)\n{' % (ret, n, pl))
            o.append('\tTR_RESOLVE(%d);' % i)
            o.append('\tif (!tr_depth) tr_glx_note(%d);' % i)
            o.append('\t%s%s;' % ('' if isvoid else 'return ', call))
            o.append('}')
            continue
        # a gl* recording wrapper
        body = []
        body.append('TR_EXPORT %s %s(%s)\n{' % (ret, n, pl))
        body.append('\tTR_RESOLVE(%d);' % i)
        body.append('\tif (tr_depth || !tr_on) { %s%s; %s}' % ('' if isvoid else 'return ', call,
                                                            'return; ' if isvoid else ''))
        unh = False
        if UNHANDLED.match(n) and n not in ARRAY_DRAWS:
            unh = True
        ptrs = []
        for k, (t, p) in enumerate(params):
            if is_ptr(t):
                if 'GLDEBUGPROC' in t:
                    unh = True
                    continue
                if is_const_ptr(t):
                    sz = in_size(n, params, k)
                    kind = 'in'
                else:
                    sz = out_size(n, params, k)
                    kind = 'gen' if is_gen(n) else 'out'
                if sz is None:
                    unh = True
                ptrs.append((k, t, p, kind, sz))
            elif base(t) not in SCALAR_OK and not t.startswith('GLDEBUGPROC'):
                unh = True
            elif t.startswith('GLDEBUGPROC') or base(t) in ('GLsync',):
                unh = True
        if n in ('glMapBuffer', 'glMapBufferARB', 'glMapBufferRange', 'glUnmapBuffer'):
            unh = True
        if unh:
            unh_all.append(n)
            body.append('\ttr_depth++;')
            if isvoid:
                body.append('\t%s;' % call)
            else:
                body.append('\t%s tr_r = %s;' % (ret, call))
            body.append('\ttr_unhandled(%d);' % i)
            body.append('\ttr_depth--;')
            if not isvoid:
                body.append('\treturn tr_r;')
            body.append('}')
            o.extend(body)
            continue
        body.append('\ttr_depth++;')
        if n in ARRAY_DRAWS:
            body.append('\tif (!tr_arrays_off(%d)) { tr_depth--; %s%s; %s}' %
                        (i, '' if isvoid else 'return ', call, 'return; ' if isvoid else ''))
        if isvoid:
            body.append('\t%s;' % call)
        else:
            body.append('\t%s tr_r = %s;' % (ret, call))
        # size of the record
        nsw = sum(scalar_words(t) for t, p in params if not is_ptr(t))
        body.append('\tunsigned tr_nw = 1 + %d;' % nsw)
        for k, t, p, kind, sz in ptrs:
            body.append('\tlong tr_sz%d = %s ? (long)(%s) : -1;' % (k, p, sz))
            body.append('\tif (tr_sz%d < 0) tr_sz%d = -1;' % (k, k))
            if kind in ('in', 'gen'):
                body.append('\ttr_nw += 1 + (tr_sz%d > 0 ? ((unsigned)tr_sz%d + 3) / 4 : 0);' % (k, k))
            else:
                body.append('\ttr_nw += 1;')
        if n == 'glGenLists':
            body.append('\ttr_nw += 1;')
        body.append('\tuint32_t *tr_w = tr_begin(%d, tr_nw);' % i)
        body.append('\tif (tr_w) {')
        for t, p in params:
            if is_ptr(t):
                continue
            b = base(t)
            if b in TYPES8:
                body.append('\t\ttr_put8(&tr_w, &%s);' % p)
            elif b in TYPESF:
                body.append('\t\ttr_putf(&tr_w, %s);' % p)
            else:
                body.append('\t\t*tr_w++ = (uint32_t)(%s);' % p)
        for k, t, p, kind, sz in ptrs:
            if kind in ('in', 'gen'):
                body.append('\t\ttr_putblob(&tr_w, %s, tr_sz%d);' % (p, k))
            else:
                body.append('\t\t*tr_w++ = (uint32_t)tr_sz%d;' % k)
        if n == 'glGenLists':
            body.append('\t\t*tr_w++ = (uint32_t)tr_r;')
        body.append('\t\ttr_end(%d, tr_w);' % i)
        body.append('\t}')
        body.append('\ttr_depth--;')
        if not isvoid:
            body.append('\treturn tr_r;')
        body.append('}')
        o.extend(body)
    # trampolines (aarch64)
    o.append('__asm__(".text\\n"')
    for k, n in enumerate(tramp):
        o.append('\t".globl %s\\n.type %s,%%function\\n.balign 16\\n%s:\\n"' % (n, n, n))
        o.append('\t"\\tadrp x16, tr_tramp_real+%d\\n\\tldr x16, [x16, :lo12:tr_tramp_real+%d]\\n\\tbr x16\\n"' %
                 (k * 8, k * 8))
    o.append(');')
    open(out_path, 'w').write('\n'.join(o) + '\n')
    sys.stderr.write('gen.py wrap: %d recorded gl, %d glX, %d trampolines, %d UNHANDLED if called\n' %
                     (len(ids), len(glx_ids), len(tramp), len(unh_all)))
    if tramp:
        sys.stderr.write('  trampolines: %s\n' % ' '.join(tramp))


def dispatch(names_path, out_path, protos, mode):
    names = [l.strip() for l in open(names_path) if l.strip()]
    names = sorted(set(n for n in names if n in protos and n.startswith('gl') and not n.startswith('glX')))
    o = ['/* generated by tools/glref/gltrace/gen.py dispatch --mode %s - do not edit */' % mode,
         '#include "replay.h"']
    o.append('const char *const rp_names[] = {')
    for n in names:
        o.append('\t"%s",' % n)
    o.append('};')
    o.append('const int rp_nnames = %d;' % len(names))
    if mode == 'table':
        o.append('void *rp_fp[%d];' % len(names))
    skipped = []
    for i, n in enumerate(names):
        ret, params = protos[n]
        fpt = '%s (*)(%s)' % (ret, ', '.join(t for t, p in params) or 'void')
        if mode == 'null':
            pl = ', '.join('%s %s' % (t, p) for t, p in params) or 'void'
            o.append('__attribute__((noinline)) static %s null_%s(%s) { __asm__ volatile(""); %s}' %
                     (ret, n, pl, '' if ret == 'void' else 'return (%s)0; ' % ret))
        o.append('static void d_%s(const uint32_t *w)\n{' % n)
        # scalars first
        off = 0
        args = []
        for t, p in params:
            if is_ptr(t):
                continue
            b = base(t)
            if b in TYPES8:
                o.append('\t%s a_%s; memcpy(&a_%s, w + %d, 8);' % (b, p, p, 1 + off))
                off += 2
            elif b in TYPESF:
                o.append('\t%s a_%s; memcpy(&a_%s, w + %d, 4);' % (b, p, p, 1 + off))
                off += 1
            else:
                o.append('\t%s a_%s = (%s)(int32_t)w[%d];' % (t, p, t, 1 + off))
                off += 1
        o.append('\tconst uint32_t *q = w + %d; (void)q;' % (1 + off))
        texname = re.search(r'Texture', n) is not None
        for t, p in params:
            if not is_ptr(t):
                continue
            kind = 'in' if is_const_ptr(t) else ('gen' if is_gen(n) else 'out')
            o.append('\tuint32_t n_%s = *q++;' % p)
            if kind == 'in':
                if texname and base(t) == 'GLuint' and p == 'textures':
                    o.append('\t%s a_%s = (%s)rp_texnames(q, n_%s);' % (t, p, t, p))
                else:
                    o.append('\t%s a_%s = n_%s == 0xFFFFFFFFu ? 0 : (%s)(const void *)q;' % (t, p, p, t))
                o.append('\tif (n_%s != 0xFFFFFFFFu) q += (n_%s + 3) / 4;' % (p, p))
            elif kind == 'gen':
                o.append('\tconst uint32_t *g_%s = q;' % p)
                o.append('\t%s a_%s = n_%s == 0xFFFFFFFFu ? 0 : (%s)rp_scratch(n_%s);' % (t, p, p, t, p))
                o.append('\tif (n_%s != 0xFFFFFFFFu) q += (n_%s + 3) / 4;' % (p, p))
            else:
                o.append('\t%s a_%s = n_%s == 0xFFFFFFFFu ? 0 : (%s)rp_scratch(n_%s);' % (t, p, p, t, p))
        for t, p in params:
            if texname and not is_ptr(t) and base(t) == 'GLuint' and p == 'texture':
                o.append('\ta_%s = rp_texname(a_%s);' % (p, p))
        al = ', '.join('a_%s' % p for t, p in params)
        tex = is_tex_call(n)
        if tex:
            o.append('\trp_cls = 1;')
        if mode == 'direct':
            o.append('\t%s(%s);' % (n, al))
        elif mode == 'null':
            o.append('\tnull_%s(%s);' % (n, al))
        else:
            o.append('\t((%s)rp_fp[%d])(%s);' % (fpt, i, al))
        if tex:
            o.append('\trp_cls = 0;')
        for t, p in params:
            if is_ptr(t) and not is_const_ptr(t) and is_gen(n):
                o.append('\tif (n_%s != 0xFFFFFFFFu) rp_gen_textures(g_%s, (const GLuint *)a_%s, n_%s / 4);' %
                         (p, p, p, p))
        o.append('}')
    o.append('const rp_fn rp_fns[] = {')
    for n in names:
        o.append('\td_%s,' % n)
    o.append('};')
    open(out_path, 'w').write('\n'.join(o) + '\n')
    sys.stderr.write('gen.py dispatch (%s): %d entry points\n' % (mode, len(names)))


def main():
    here = __file__.rsplit('/', 1)[0]
    inc = here + '/../../../gl/include/GL/'
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == '--inc':
        inc = a[1].rstrip('/') + '/'
        a = a[2:]
    protos = parse(inc + 'gl.h', inc + 'glext.h', inc + 'glx.h')
    if a[0] == 'wrap':
        wrap(a[1], a[2], protos)
    elif a[0] == 'dispatch':
        mode = 'direct'
        if '--mode' in a:
            mode = a[a.index('--mode') + 1]
        dispatch(a[1], a[2], protos, mode)
    elif a[0] == 'protos':
        for n in sorted(protos):
            print(n, protos[n])
    else:
        sys.exit('usage: gen.py [--inc DIR] wrap EXPORTS OUT.c | dispatch NAMES OUT.c [--mode M]')


if __name__ == '__main__':
    main()
