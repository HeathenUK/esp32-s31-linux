/*
 * gltrace.h - the GL call trace format (tools/glref/gltrace). s31, MIT.
 *
 * A trace is little-endian 32-bit words:
 *
 *   header   "GLTR" magic, version, nnames, then nnames names, each a word
 *            of length and the bytes padded to a word. A record's id is an
 *            index into this table (or one of the pseudo ids below), so a
 *            replayer maps names, not numbers, and a trace survives a
 *            library that exports more functions.
 *   records  word 0 = id | nwords << 12 (nwords counts word 0 too; the
 *            value 0xFFFFF means word 1 holds nwords). The payload of a
 *            GL call is described in gen.py.
 *
 * Pseudo records:
 *   TR_SWAP  frame, flags, live hash, w, h, window
 *            flags: TRS_FULL - the frame's draw calls are in the trace (a
 *            counted or warm-up frame); TRS_COUNT - a counted frame. A
 *            frame without TRS_FULL is "state only": its draw calls
 *            (glBegin/glEnd blocks, glClear, glDraw*, glReadPixels,
 *            glCopyTex*) and queries were dropped, and each glBegin/glEnd
 *            block left only the last colour/texcoord/normal/... it set, so
 *            the GL state at the next frame is what it was live.
 *            hash: FNV-1a 32 of the window's RGB565 pixels, top row first,
 *            as the X server held them after the swap (0 when not full).
 *   TR_CTX   a MakeCurrent: ctx, drawable, w, h, depth bits, stencil
 *            bits, double buffer, red/green/blue bits
 *   TR_NEWCTX ctx, share ctx
 *   TR_DELCTX ctx
 *   TR_UNHANDLED id of a call the tracer cannot record faithfully (the
 *            capture script fails when one occurs)
 *   TR_END   frames, unhandled calls
 */
#ifndef GLTRACE_H
#define GLTRACE_H

#define TR_MAGIC 0x52544c47u /* "GLTR" */
#define TR_VERSION 1

#define TR_ID_BITS 12
#define TR_ID_MASK 0xFFFu
#define TR_NW_EXT 0xFFFFFu

#define TR_SWAP 0xFFFu
#define TR_CTX 0xFFEu
#define TR_NEWCTX 0xFFDu
#define TR_DELCTX 0xFFCu
#define TR_UNHANDLED 0xFFBu
#define TR_END 0xFFAu
#define TR_PSEUDO_MIN 0xFF0u

#define TRS_FULL 1u
#define TRS_COUNT 2u

#endif
