# glref report: phase5/p5c/x4/MUL8

Implementation under test: **ours**; reference: Mesa llvmpipe (cached in `../../../ref-mesa`).
Xvfb 800x480x16, deterministic time (1/60 s per swap), LD_BIND_NOW=1. Thresholds: channel tolerance 16/255, tolerant-bad <= 1.00% to PASS (see tools/glref/compare.py for why).

**Apps: 3.** PASS 3

| app | verdict | command |
|---|---|---|
| ipers | **PASS** | `$GD/ipers -geometry 320x240+0+0` |
| fire | **PASS** | `$GD/fire -geometry 320x240+0+0` |
| teapot | **PASS** | `$GD/teapot -geometry 320x240+0+0` |

## Per frame

| app | frame | verdict | strict bad % | tolerant bad % | max err | mean err | ours run | mesa ref | notes |
|---|---|---|---|---|---|---|---|---|---|
| ipers | 3 | **PASS** | 0.009 | 0.001 | 123 | 0.23 | ok | ok |  |
| ipers | 20 | **PASS** | 0.004 | 0.001 | 123 | 0.22 | ok | ok |  |
| ipers | 60 | **PASS** | 0.004 | 0.000 | 123 | 0.22 | ok | ok |  |
| fire | 3 | **PASS** | 0.535 | 0.223 | 206 | 1.51 | ok | ok |  |
| fire | 20 | **PASS** | 0.626 | 0.272 | 90 | 1.74 | ok | ok |  |
| fire | 60 | **PASS** | 0.707 | 0.352 | 90 | 1.54 | ok | ok |  |
| teapot | 3 | **PASS** | 0.010 | 0.005 | 82 | 1.72 | ok | ok |  |
| teapot | 20 | **PASS** | 0.029 | 0.020 | 82 | 1.73 | ok | ok |  |
| teapot | 60 | **PASS** | 0.303 | 0.242 | 82 | 1.81 | ok | ok |  |

## Images

Mesa reference, ours, diff (grey = reference, red = tolerant-bad, yellow = edge-only). EXACT frames have no diff image.

**ipers f3: PASS**  
![](../../../../ref-mesa/mesa/ipers.f3.png) ![](ours/ipers.f3.png) ![](diff/ipers.f3.png)

**ipers f20: PASS**  
![](../../../../ref-mesa/mesa/ipers.f20.png) ![](ours/ipers.f20.png) ![](diff/ipers.f20.png)

**ipers f60: PASS**  
![](../../../../ref-mesa/mesa/ipers.f60.png) ![](ours/ipers.f60.png) ![](diff/ipers.f60.png)

**fire f3: PASS**  
![](../../../../ref-mesa/mesa/fire.f3.png) ![](ours/fire.f3.png) ![](diff/fire.f3.png)

**fire f20: PASS**  
![](../../../../ref-mesa/mesa/fire.f20.png) ![](ours/fire.f20.png) ![](diff/fire.f20.png)

**fire f60: PASS**  
![](../../../../ref-mesa/mesa/fire.f60.png) ![](ours/fire.f60.png) ![](diff/fire.f60.png)

**teapot f3: PASS**  
![](../../../../ref-mesa/mesa/teapot.f3.png) ![](ours/teapot.f3.png) ![](diff/teapot.f3.png)

**teapot f20: PASS**  
![](../../../../ref-mesa/mesa/teapot.f20.png) ![](ours/teapot.f20.png) ![](diff/teapot.f20.png)

**teapot f60: PASS**  
![](../../../../ref-mesa/mesa/teapot.f60.png) ![](ours/teapot.f60.png) ![](diff/teapot.f60.png)


## xlite load arm (board X stack, load time only)

Each app and the GL libraries it loads, resolved with `ldd -r` (every
relocation, as musl binds) against our libGL + xlite libX11 + xstubs
libXext (+ xlite's libXrandr/libXxf86vm), built for the host from the
repo sources; libXi/libXrender are the host's, standing in for the
board's. LOADS means no undefined symbol; it says nothing about what
the calls then do.

| app | result | undefined symbols (library) |
|---|---|---|
| glxgears | LOADS | |
| glxinfo | LOADS | |
| glxheads | LOADS | |
| manywin | LOADS | |
| multictx | LOADS | |
| offset | LOADS | |
| glxgears_fbconfig | LOADS | |
| gears | LOADS | |
| morph3d | LOADS | |
| bounce | LOADS | |
| spectex | LOADS | |
| geartrain | LOADS | |
| ipers | LOADS | |
| terrain | LOADS | |
| tunnel | LOADS | |
| fire | LOADS | |
| teapot | LOADS | |
| texcyl | LOADS | |
| isosurf | LOADS | |
| testgl | LOADS | |
| testgl2 | LOADS | host SDL2 only, not counted (board SDL2 binds X11 at run time): XdbeAllocateBackBufferName XdbeBeginIdiom XdbeDeallocateBackBufferName XdbeEndIdiom XdbeFreeVisualInfo XdbeGetBackBufferAttributes XdbeGetVisualInfo XdbeQueryExtension XdbeSwapBuffers Xutf8DrawString Xutf8ResetIC Xutf8TextExtents |
| rrootage | LOADS | host SDL2 only, not counted (board SDL2 binds X11 at run time): XdbeAllocateBackBufferName XdbeBeginIdiom XdbeDeallocateBackBufferName XdbeEndIdiom XdbeFreeVisualInfo XdbeGetBackBufferAttributes XdbeGetVisualInfo XdbeQueryExtension XdbeSwapBuffers Xutf8DrawString Xutf8ResetIC Xutf8TextExtents |

22 load, 0 would abort at load on the board.

These are gaps in xlite / the stub libraries (not libGL: every gl*/glX*
import resolves). Owners: xlite for libX11 names, xstubs for libXext.
