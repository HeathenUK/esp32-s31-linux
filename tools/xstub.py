#!/usr/bin/env python3
"""A logging X11 server stub: what does a real client actually ask for?

Runs on the DEVELOPMENT HOST over TCP, with the board's client pointed at it
(DISPLAY=<host>:0).  That is deliberate - iterating on the protocol in Python
on the host takes seconds, where a cross-compile and deploy per experiment
takes minutes, and the answer we want is a list of requests, not performance.

The output is the specification for the shim inside lvdesk: implement what
xclock actually sends, in the order it sends it, rather than "X11".

Two rules learned the hard way elsewhere in this project:

  * Fail loudly.  Every unknown opcode is printed with its length.  A silent
    gap in a protocol implementation shows up as a client that hangs with no
    clue, which is the most expensive kind of bug to chase.
  * Never let a wrong reply look like a right one.  Requests that expect a
    reply and are not implemented are logged as BLOCKING, because that is
    where the client will stop and everything after it is unknown.
"""
import socket
import struct
import sys
import threading

# Core protocol opcodes, for readable output.
OPCODES = {
    1: "CreateWindow", 2: "ChangeWindowAttributes", 3: "GetWindowAttributes",
    4: "DestroyWindow", 6: "ChangeSaveSet", 7: "ReparentWindow",
    8: "MapWindow", 9: "MapSubwindows", 10: "UnmapWindow",
    12: "ConfigureWindow", 14: "GetGeometry", 15: "QueryTree",
    16: "InternAtom", 17: "GetAtomName", 18: "ChangeProperty",
    19: "DeleteProperty", 20: "GetProperty", 21: "ListProperties",
    22: "SetSelectionOwner", 23: "GetSelectionOwner", 24: "ConvertSelection",
    25: "SendEvent", 26: "GrabPointer", 27: "UngrabPointer",
    28: "GrabButton", 29: "UngrabButton", 31: "GrabKeyboard",
    32: "UngrabKeyboard", 33: "GrabKey", 34: "UngrabKey",
    36: "GrabServer", 37: "UngrabServer", 38: "QueryPointer",
    40: "TranslateCoordinates", 42: "SetInputFocus", 43: "GetInputFocus",
    45: "OpenFont", 46: "CloseFont", 47: "QueryFont", 48: "QueryTextExtents",
    49: "ListFonts", 53: "CreatePixmap", 54: "FreePixmap", 55: "CreateGC",
    56: "ChangeGC", 57: "CopyGC", 58: "SetDashes", 59: "SetClipRectangles",
    60: "FreeGC", 61: "ClearArea", 62: "CopyArea", 63: "CopyPlane",
    64: "PolyPoint", 65: "PolyLine", 66: "PolySegment", 67: "PolyRectangle",
    68: "PolyArc", 69: "FillPoly", 70: "PolyFillRectangle", 71: "PolyFillArc",
    72: "PutImage", 73: "GetImage", 74: "PolyText8", 76: "ImageText8",
    78: "CreateColormap", 79: "FreeColormap", 84: "AllocColor",
    85: "AllocNamedColor", 88: "FreeColors", 91: "QueryColors",
    92: "LookupColor", 93: "CreateCursor", 94: "CreateGlyphCursor",
    95: "FreeCursor", 98: "QueryExtension", 99: "ListExtensions",
    101: "GetKeyboardMapping", 103: "GetKeyboardControl", 106: "SetPointerMapping",
    107: "GetPointerMapping", 108: "SetModifierMapping", 109: "GetModifierMapping",
    119: "GetModifierMapping", 127: "NoOperation",
}

# Requests that expect a reply. If we do not answer one, the client stops.
EXPECTS_REPLY = {
    3, 14, 15, 16, 17, 20, 21, 23, 26, 31, 38, 40, 43, 47, 48, 49,
    73, 84, 85, 91, 92, 98, 99, 101, 103, 107, 109, 119,
}

ROOT, VISUAL, CMAP = 0x0000_0100, 0x0000_0021, 0x0000_0020
W, H = 800, 480


def setup_reply(vendor=b"lvdesk-shim"):
    """The connection setup reply. Xlib gives up immediately if this is wrong."""
    fmts = b"".join(struct.pack("<BBB5x", d, bpp, 32)
                    for d, bpp in ((1, 1), (16, 16), (24, 32)))

    visual = struct.pack("<IBBHIII4x", VISUAL, 4, 8, 0,
                         0xF800, 0x07E0, 0x001F)      # TrueColor RGB565
    depth16 = struct.pack("<BxH4x", 16, 1) + visual
    depth1 = struct.pack("<BxH4x", 1, 0)

    screen = struct.pack("<IIIIIHHHHHHIBBBB",
                         ROOT, CMAP, 0x00FFFFFF, 0x00000000, 0,
                         W, H, W // 4, H // 4, 1, 1, VISUAL,
                         0, 0, 16, 2) + depth16 + depth1

    vend = vendor + b"\x00" * ((-len(vendor)) % 4)
    # 4 unused bytes, not 2: the fixed part of the setup reply after the first
    # eight bytes is exactly 32, and getting it wrong makes the whole reply the
    # wrong length - which Xlib reports as nothing at all, it just gives up.
    body = struct.pack("<IIIIHHBBBBBBBB4x",
                       1, 0x00400000, 0x001FFFFF, 0,
                       len(vendor), 65535, 1, len(fmts) // 8,
                       0, 0, 32, 32, 8, 255)
    body += vend + fmts + screen
    assert len(body) % 4 == 0, len(body)
    return struct.pack("<BxHHH", 1, 11, 0, len(body) // 4) + body


def reply(seq, data24, extra=b"", detail=0):
    """A reply: 32 bytes minimum, then extra in 4-byte units."""
    assert len(data24) == 24, len(data24)
    assert len(extra) % 4 == 0, len(extra)
    return (struct.pack("<BBHI", 1, detail, seq & 0xFFFF, len(extra) // 4)
            + data24 + extra)


def serve(conn, addr):
    print(f"--- client connected from {addr}", flush=True)
    hdr = conn.recv(12)
    if len(hdr) < 12:
        return
    order, _, maj, minr, nauth, ndata, _ = struct.unpack("<BBHHHHH", hdr)
    print(f"    setup: order={chr(order)} version={maj}.{minr} "
          f"auth={nauth}/{ndata}", flush=True)
    need = (nauth + 3) // 4 * 4 + (ndata + 3) // 4 * 4
    while need > 0:
        need -= len(conn.recv(need))
    conn.sendall(setup_reply())

    seq, buf, counts, atoms = 0, b"", {}, {}
    windows = {}
    unanswered = set()
    while True:
        try:
            chunk = conn.recv(65536)
        except OSError:
            break
        if not chunk:
            break
        buf += chunk
        while len(buf) >= 4:
            op, detail, rlen = struct.unpack("<BBH", buf[:4])
            if rlen == 0:
                print(f"    !! zero-length request, opcode {op}", flush=True)
                return
            nbytes = rlen * 4
            if len(buf) < nbytes:
                break
            body, buf = buf[:nbytes], buf[nbytes:]
            seq += 1
            name = OPCODES.get(op, f"opcode-{op}")
            if op >= 128:
                name = f"EXTENSION-{op}"
            counts[name] = counts.get(name, 0) + 1
            extra = ""

            if op == 98:                                   # QueryExtension
                n = struct.unpack("<H", body[4:6])[0]
                ext = body[8:8 + n].decode("latin1")
                extra = f"  <{ext}>"
                # Refuse everything. RENDER will be added deliberately later;
                # refusing XKB sends Xlib down its core-keyboard path.
                conn.sendall(reply(seq, struct.pack("<BBBB20x", 0, 0, 0, 0)))

            elif op == 16:                                 # InternAtom
                n = struct.unpack("<H", body[4:6])[0]
                nm = body[8:8 + n].decode("latin1")
                extra = f"  <{nm}>"
                a = atoms.setdefault(nm, len(atoms) + 1)
                conn.sendall(reply(seq, struct.pack("<I20x", a)))

            elif op == 20:                                 # GetProperty
                # None: type 0, no value. Xt copes and moves on.
                conn.sendall(reply(seq, struct.pack("<IIII8x", 0, 0, 0, 0)))

            elif op == 43:                                 # GetInputFocus
                conn.sendall(reply(seq, struct.pack("<I20x", ROOT), detail=1))

            elif op == 14:                                 # GetGeometry
                conn.sendall(reply(seq, struct.pack("<IhhHHH10x",
                                                    ROOT, 0, 0, W, H, 0),
                                   detail=16))

            elif op == 15:                                 # QueryTree
                conn.sendall(reply(seq, struct.pack("<IIH14x", ROOT, 0, 0)))

            elif op == 38:                                 # QueryPointer
                conn.sendall(reply(seq, struct.pack("<IIhhhhH2x",
                                                    ROOT, 0, 0, 0, 0, 0, 0),
                                   detail=1))

            elif op == 84:                                 # AllocColor
                r, g, b = struct.unpack("<HHH", body[8:14])
                px = ((r >> 11) << 11) | ((g >> 10) << 5) | (b >> 11)
                conn.sendall(reply(seq, struct.pack("<HHH2xI10x", r, g, b, px)))

            elif op == 101:                                # GetKeyboardMapping
                # One keysym per keycode, all NoSymbol: enough to satisfy the
                # setup path without inventing a layout yet.
                first, count = body[4], body[5]
                conn.sendall(reply(seq, b"\x00" * 24,
                                   b"\x00\x00\x00\x00" * count, detail=1))

            elif op == 47:                                 # QueryFont
                # A synthetic fixed 8x8 face covering ASCII 32..126.
                #
                # xclock takes the CORE font path here, not Xft - so the shim
                # needs QueryFont whatever else it does. The reply is a fixed
                # header plus one 12-byte CHARINFO per character, and every
                # field has to be consistent or the toolkit lays text out on
                # garbage metrics.
                CW, ASC, DESC = 8, 7, 1
                ci = struct.pack("<hhhhhH", 0, CW, CW, ASC, DESC, 0)
                first, last = 32, 126
                chars = ci * (last - first + 1)
                hdr = (ci + b"\x00" * 4 +          # min-bounds
                       ci + b"\x00" * 4 +          # max-bounds
                       struct.pack("<HHHHBBBBhhI",
                                   first, last, first, 0,
                                   0, 0, 0, 1,      # LTR, byte1 range, all exist
                                   ASC, DESC, last - first + 1))
                # hdr is 24 bytes of "data24" plus the rest as extra.
                conn.sendall(reply(seq, hdr[:24], hdr[24:] + chars))

            elif op == 48:                                 # QueryTextExtents
                conn.sendall(reply(seq, struct.pack("<hhhhhh12x",
                                                    7, 1, 7, 1, 0, 0), detail=0))

            elif op == 49:                                 # ListFonts
                conn.sendall(reply(seq, struct.pack("<H22x", 0)))

            elif op == 119 or op == 109:                   # GetModifierMapping
                conn.sendall(reply(seq, b"\x00" * 24, b"\x00" * 32, detail=2))

            elif op == 1:                                  # CreateWindow
                wid, parent = struct.unpack("<II", body[4:12])
                x, y, w, h = struct.unpack("<hhHH", body[12:20])
                windows[wid] = (w, h)
                extra = f"  id=0x{wid:x} {w}x{h}+{x}+{y} parent=0x{parent:x}"

            elif op in (8, 9):                              # MapWindow/Subwindows
                wid = struct.unpack("<I", body[4:8])[0]
                w, h = windows.get(wid, (W, H))
                extra = f"  id=0x{wid:x}"
                # Expose EVERY window we know about, not just this one.
                # xclock draws into a CHILD widget window, so an Expose sent
                # only to the top-level leaves it sitting in its event loop
                # with nothing to react to - indistinguishable from a hang.
                for owid, (ow, oh) in windows.items():
                    conn.sendall(struct.pack("<BxHIIB19x",
                                             19, seq & 0xFFFF, owid, owid, 0))
                    conn.sendall(struct.pack("<BxHIHHHHH14x",
                                             12, seq & 0xFFFF, owid,
                                             0, 0, ow, oh, 0))
                print(f"       -> exposed {len(windows)} window(s)", flush=True)
                print(f"  [{seq:3}] {name:<24} len={nbytes:<5}{extra}",
                      flush=True)
                continue

            elif op == 999:                                 # (unused)
                pass

            elif op == 8888:                                # (unused)
                wid = 0

            elif op in EXPECTS_REPLY:
                unanswered.add(name)
                print(f"  [{seq:3}] {name:<24} len={nbytes:<5}"
                      f"  ** NO REPLY IMPLEMENTED - client will block here",
                      flush=True)
                continue

            print(f"  [{seq:3}] {name:<24} len={nbytes:<5}{extra}", flush=True)

    print("\n=== requests seen ===", flush=True)
    for k, v in sorted(counts.items(), key=lambda x: -x[1]):
        print(f"  {v:4} x {k}", flush=True)
    if unanswered:
        print("=== blocked on (implement these next) ===", flush=True)
        for k in sorted(unanswered):
            print(f"  {k}", flush=True)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 6000
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port))
    s.listen(4)
    print(f"xstub listening on :{port}  (DISPLAY=<host>:{port - 6000})",
          flush=True)
    while True:
        conn, addr = s.accept()
        threading.Thread(target=serve, args=(conn, addr), daemon=True).start()


if __name__ == "__main__":
    main()
