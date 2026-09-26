# glref report: phase3a/p5-final3/suite

Implementation under test: **ours**; reference: Mesa llvmpipe (cached in `../../ref-mesa`).
Xvfb 800x480x16, deterministic time (1/60 s per swap), LD_BIND_NOW=1. Thresholds: channel tolerance 16/255, tolerant-bad <= 1.00% to PASS (see tools/glref/compare.py for why).

**Apps: 22.** PASS 21, EXACT 1

| app | verdict | command |
|---|---|---|
| glxgears | **PASS** | `$XD/glxgears -geometry 320x240+0+0` |
| glxinfo | **PASS** | `$XD/glxinfo` |
| glxheads | **PASS** | `$XD/glxheads` |
| manywin | **EXACT** | `$XD/manywin 4` |
| multictx | **PASS** | `$XD/multictx` |
| offset | **PASS** | `$XD/offset` |
| glxgears_fbconfig | **PASS** | `$XD/glxgears_fbconfig` |
| gears | **PASS** | `$GD/gears -geometry 320x240+0+0` |
| morph3d | **PASS** | `$GD/morph3d -geometry 320x240+0+0` |
| bounce | **PASS** | `$GD/bounce -geometry 320x240+0+0` |
| spectex | **PASS** | `$GD/spectex -geometry 320x240+0+0` |
| geartrain | **PASS** | `$GD/geartrain -geometry 320x240+0+0` |
| ipers | **PASS** | `$GD/ipers -geometry 320x240+0+0` |
| terrain | **PASS** | `$GD/terrain -geometry 320x240+0+0` |
| tunnel | **PASS** | `$GD/tunnel -geometry 320x240+0+0` |
| fire | **PASS** | `$GD/fire -geometry 320x240+0+0` |
| teapot | **PASS** | `$GD/teapot -geometry 320x240+0+0` |
| texcyl | **PASS** | `$GD/texcyl -geometry 320x240+0+0` |
| isosurf | **PASS** | `$GD/isosurf` |
| testgl | **PASS** | `$SB/sdl12-test/testgl` |
| testgl2 | **PASS** | `$SB/sdl2-test/testgl2 --geometry 320x240` |
| rrootage | **PASS** | `$SB/rrootage/rr -lowres -window -nosound` |

## Per frame

| app | frame | verdict | strict bad % | tolerant bad % | max err | mean err | ours run | mesa ref | notes |
|---|---|---|---|---|---|---|---|---|---|
| glxgears | 3 | **PASS** | 0.004 | 0.000 | 173 | 0.00 | ok | ok |  |
| glxgears | 20 | **PASS** | 0.000 | 0.000 | 16 | 0.00 | ok | ok |  |
| glxgears | 60 | **PASS** | 0.001 | 0.000 | 154 | 0.00 | ok | ok |  |
| glxinfo | 0 | **PASS** | - | - | - | - | exit | exit | OpenGL vendor string: s31; OpenGL renderer string: Software Rasterizer; OpenGL version string: 1.1 s31-tinygl; direct rendering: Yes; GLX version: 1.4 |
| glxheads | 3 | **PASS** | 0.002 | 0.000 | 132 | 0.00 | ok | ok |  |
| glxheads | 20 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| glxheads | 60 | **PASS** | 0.001 | 0.000 | 132 | 0.00 | ok | ok |  |
| manywin | 4 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| manywin | 20 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| manywin | 60 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| multictx | 20 | **PASS** | 0.004 | 0.003 | 91 | 0.00 | ok | ok |  |
| offset | 1 | **PASS** | 1.173 | 0.152 | 255 | 2.04 | ok | ok |  |
| glxgears_fbconfig | 3 | **PASS** | 0.001 | 0.000 | 197 | 0.00 | ok | ok |  |
| glxgears_fbconfig | 20 | **PASS** | 0.002 | 0.000 | 197 | 0.00 | ok | ok |  |
| glxgears_fbconfig | 60 | **PASS** | 0.004 | 0.000 | 197 | 0.00 | ok | ok |  |
| gears | 3 | **PASS** | 0.000 | 0.000 | 9 | 0.00 | ok | ok |  |
| gears | 20 | **PASS** | 0.004 | 0.000 | 247 | 0.01 | ok | ok |  |
| gears | 60 | **PASS** | 0.001 | 0.000 | 247 | 0.00 | ok | ok |  |
| morph3d | 3 | **PASS** | 0.000 | 0.000 | 9 | 0.00 | ok | ok |  |
| morph3d | 20 | **PASS** | 0.000 | 0.000 | 8 | 0.00 | ok | ok |  |
| morph3d | 60 | **PASS** | 0.000 | 0.000 | 8 | 0.00 | ok | ok |  |
| bounce | 3 | **PASS** | 6.159 | 0.014 | 255 | 10.47 | ok | ok |  |
| bounce | 20 | **PASS** | 6.082 | 0.001 | 255 | 10.34 | ok | ok |  |
| bounce | 60 | **PASS** | 6.168 | 0.005 | 255 | 10.49 | ok | ok |  |
| spectex | 3 | **PASS** | 0.207 | 0.020 | 33 | 0.16 | ok | ok |  |
| spectex | 20 | **PASS** | 0.184 | 0.010 | 25 | 0.17 | ok | ok |  |
| spectex | 60 | **PASS** | 0.193 | 0.013 | 33 | 0.17 | ok | ok |  |
| geartrain | 3 | **PASS** | 0.233 | 0.029 | 165 | 0.40 | ok | ok |  |
| geartrain | 20 | **PASS** | 0.242 | 0.030 | 165 | 0.44 | ok | ok |  |
| geartrain | 60 | **PASS** | 0.214 | 0.021 | 165 | 0.44 | ok | ok |  |
| ipers | 3 | **PASS** | 0.009 | 0.001 | 123 | 0.17 | ok | ok |  |
| ipers | 20 | **PASS** | 0.004 | 0.001 | 123 | 0.17 | ok | ok |  |
| ipers | 60 | **PASS** | 0.004 | 0.000 | 115 | 0.16 | ok | ok |  |
| terrain | 3 | **PASS** | 0.004 | 0.000 | 33 | 0.18 | ok | ok |  |
| terrain | 20 | **PASS** | 0.000 | 0.000 | 9 | 0.21 | ok | ok |  |
| terrain | 60 | **PASS** | 0.000 | 0.000 | 9 | 0.16 | ok | ok |  |
| tunnel | 3 | **PASS** | 0.001 | 0.000 | 24 | 0.59 | ok | ok |  |
| tunnel | 20 | **PASS** | 0.000 | 0.000 | 16 | 0.60 | ok | ok |  |
| tunnel | 60 | **PASS** | 0.000 | 0.000 | 16 | 0.59 | ok | ok |  |
| fire | 3 | **PASS** | 0.633 | 0.255 | 206 | 1.53 | ok | ok |  |
| fire | 20 | **PASS** | 0.757 | 0.309 | 90 | 1.82 | ok | ok |  |
| fire | 60 | **PASS** | 0.621 | 0.240 | 90 | 1.49 | ok | ok |  |
| teapot | 3 | **PASS** | 0.010 | 0.005 | 91 | 1.64 | ok | ok |  |
| teapot | 20 | **PASS** | 0.030 | 0.020 | 91 | 1.65 | ok | ok |  |
| teapot | 60 | **PASS** | 0.293 | 0.237 | 91 | 1.73 | ok | ok |  |
| texcyl | 3 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| texcyl | 20 | **PASS** | 0.000 | 0.000 | 9 | 0.02 | ok | ok |  |
| texcyl | 60 | **PASS** | 0.003 | 0.000 | 25 | 0.04 | ok | ok |  |
| isosurf | 1 | **PASS** | 0.002 | 0.001 | 181 | 0.08 | ok | ok |  |
| testgl | 3 | **PASS** | 0.000 | 0.000 | 9 | 0.01 | ok | ok |  |
| testgl | 20 | **PASS** | 0.000 | 0.000 | 9 | 0.03 | ok | ok |  |
| testgl | 60 | **PASS** | 0.000 | 0.000 | 9 | 0.04 | ok | ok |  |
| testgl2 | 3 | **PASS** | 0.000 | 0.000 | 9 | 0.02 | ok | ok |  |
| testgl2 | 20 | **PASS** | 0.000 | 0.000 | 9 | 0.04 | ok | ok |  |
| testgl2 | 60 | **PASS** | 0.000 | 0.000 | 9 | 0.06 | ok | ok |  |
| rrootage | 3 | **PASS** | 0.672 | 0.579 | 83 | 0.35 | ok | ok |  |
| rrootage | 60 | **PASS** | 0.663 | 0.565 | 82 | 0.35 | ok | ok |  |
| rrootage | 300 | **PASS** | 0.629 | 0.536 | 156 | 0.33 | ok | ok |  |

## Images

Mesa reference, ours, diff (grey = reference, red = tolerant-bad, yellow = edge-only). EXACT frames have no diff image.

**glxgears f3: PASS**  
![](../../../ref-mesa/mesa/glxgears.f3.png) ![](ours/glxgears.f3.png) ![](diff/glxgears.f3.png)

**glxgears f20: PASS**  
![](../../../ref-mesa/mesa/glxgears.f20.png) ![](ours/glxgears.f20.png) ![](diff/glxgears.f20.png)

**glxgears f60: PASS**  
![](../../../ref-mesa/mesa/glxgears.f60.png) ![](ours/glxgears.f60.png) ![](diff/glxgears.f60.png)

**glxheads f3: PASS**  
![](../../../ref-mesa/mesa/glxheads.f3.png) ![](ours/glxheads.f3.png) ![](diff/glxheads.f3.png)

**glxheads f20: EXACT**  
![](../../../ref-mesa/mesa/glxheads.f20.png) ![](ours/glxheads.f20.png) -

**glxheads f60: PASS**  
![](../../../ref-mesa/mesa/glxheads.f60.png) ![](ours/glxheads.f60.png) ![](diff/glxheads.f60.png)

**manywin f4: EXACT**  
![](../../../ref-mesa/mesa/manywin.f4.png) ![](ours/manywin.f4.png) -

**manywin f20: EXACT**  
![](../../../ref-mesa/mesa/manywin.f20.png) ![](ours/manywin.f20.png) -

**manywin f60: EXACT**  
![](../../../ref-mesa/mesa/manywin.f60.png) ![](ours/manywin.f60.png) -

**multictx f20: PASS**  
![](../../../ref-mesa/mesa/multictx.f20.png) ![](ours/multictx.f20.png) ![](diff/multictx.f20.png)

**offset f1: PASS**  
![](../../../ref-mesa/mesa/offset.f1.png) ![](ours/offset.f1.png) ![](diff/offset.f1.png)

**glxgears_fbconfig f3: PASS**  
![](../../../ref-mesa/mesa/glxgears_fbconfig.f3.png) ![](ours/glxgears_fbconfig.f3.png) ![](diff/glxgears_fbconfig.f3.png)

**glxgears_fbconfig f20: PASS**  
![](../../../ref-mesa/mesa/glxgears_fbconfig.f20.png) ![](ours/glxgears_fbconfig.f20.png) ![](diff/glxgears_fbconfig.f20.png)

**glxgears_fbconfig f60: PASS**  
![](../../../ref-mesa/mesa/glxgears_fbconfig.f60.png) ![](ours/glxgears_fbconfig.f60.png) ![](diff/glxgears_fbconfig.f60.png)

**gears f3: PASS**  
![](../../../ref-mesa/mesa/gears.f3.png) ![](ours/gears.f3.png) ![](diff/gears.f3.png)

**gears f20: PASS**  
![](../../../ref-mesa/mesa/gears.f20.png) ![](ours/gears.f20.png) ![](diff/gears.f20.png)

**gears f60: PASS**  
![](../../../ref-mesa/mesa/gears.f60.png) ![](ours/gears.f60.png) ![](diff/gears.f60.png)

**morph3d f3: PASS**  
![](../../../ref-mesa/mesa/morph3d.f3.png) ![](ours/morph3d.f3.png) ![](diff/morph3d.f3.png)

**morph3d f20: PASS**  
![](../../../ref-mesa/mesa/morph3d.f20.png) ![](ours/morph3d.f20.png) ![](diff/morph3d.f20.png)

**morph3d f60: PASS**  
![](../../../ref-mesa/mesa/morph3d.f60.png) ![](ours/morph3d.f60.png) ![](diff/morph3d.f60.png)

**bounce f3: PASS**  
![](../../../ref-mesa/mesa/bounce.f3.png) ![](ours/bounce.f3.png) ![](diff/bounce.f3.png)

**bounce f20: PASS**  
![](../../../ref-mesa/mesa/bounce.f20.png) ![](ours/bounce.f20.png) ![](diff/bounce.f20.png)

**bounce f60: PASS**  
![](../../../ref-mesa/mesa/bounce.f60.png) ![](ours/bounce.f60.png) ![](diff/bounce.f60.png)

**spectex f3: PASS**  
![](../../../ref-mesa/mesa/spectex.f3.png) ![](ours/spectex.f3.png) ![](diff/spectex.f3.png)

**spectex f20: PASS**  
![](../../../ref-mesa/mesa/spectex.f20.png) ![](ours/spectex.f20.png) ![](diff/spectex.f20.png)

**spectex f60: PASS**  
![](../../../ref-mesa/mesa/spectex.f60.png) ![](ours/spectex.f60.png) ![](diff/spectex.f60.png)

**geartrain f3: PASS**  
![](../../../ref-mesa/mesa/geartrain.f3.png) ![](ours/geartrain.f3.png) ![](diff/geartrain.f3.png)

**geartrain f20: PASS**  
![](../../../ref-mesa/mesa/geartrain.f20.png) ![](ours/geartrain.f20.png) ![](diff/geartrain.f20.png)

**geartrain f60: PASS**  
![](../../../ref-mesa/mesa/geartrain.f60.png) ![](ours/geartrain.f60.png) ![](diff/geartrain.f60.png)

**ipers f3: PASS**  
![](../../../ref-mesa/mesa/ipers.f3.png) ![](ours/ipers.f3.png) ![](diff/ipers.f3.png)

**ipers f20: PASS**  
![](../../../ref-mesa/mesa/ipers.f20.png) ![](ours/ipers.f20.png) ![](diff/ipers.f20.png)

**ipers f60: PASS**  
![](../../../ref-mesa/mesa/ipers.f60.png) ![](ours/ipers.f60.png) ![](diff/ipers.f60.png)

**terrain f3: PASS**  
![](../../../ref-mesa/mesa/terrain.f3.png) ![](ours/terrain.f3.png) ![](diff/terrain.f3.png)

**terrain f20: PASS**  
![](../../../ref-mesa/mesa/terrain.f20.png) ![](ours/terrain.f20.png) ![](diff/terrain.f20.png)

**terrain f60: PASS**  
![](../../../ref-mesa/mesa/terrain.f60.png) ![](ours/terrain.f60.png) ![](diff/terrain.f60.png)

**tunnel f3: PASS**  
![](../../../ref-mesa/mesa/tunnel.f3.png) ![](ours/tunnel.f3.png) ![](diff/tunnel.f3.png)

**tunnel f20: PASS**  
![](../../../ref-mesa/mesa/tunnel.f20.png) ![](ours/tunnel.f20.png) ![](diff/tunnel.f20.png)

**tunnel f60: PASS**  
![](../../../ref-mesa/mesa/tunnel.f60.png) ![](ours/tunnel.f60.png) ![](diff/tunnel.f60.png)

**fire f3: PASS**  
![](../../../ref-mesa/mesa/fire.f3.png) ![](ours/fire.f3.png) ![](diff/fire.f3.png)

**fire f20: PASS**  
![](../../../ref-mesa/mesa/fire.f20.png) ![](ours/fire.f20.png) ![](diff/fire.f20.png)

**fire f60: PASS**  
![](../../../ref-mesa/mesa/fire.f60.png) ![](ours/fire.f60.png) ![](diff/fire.f60.png)

**teapot f3: PASS**  
![](../../../ref-mesa/mesa/teapot.f3.png) ![](ours/teapot.f3.png) ![](diff/teapot.f3.png)

**teapot f20: PASS**  
![](../../../ref-mesa/mesa/teapot.f20.png) ![](ours/teapot.f20.png) ![](diff/teapot.f20.png)

**teapot f60: PASS**  
![](../../../ref-mesa/mesa/teapot.f60.png) ![](ours/teapot.f60.png) ![](diff/teapot.f60.png)

**texcyl f3: EXACT**  
![](../../../ref-mesa/mesa/texcyl.f3.png) ![](ours/texcyl.f3.png) -

**texcyl f20: PASS**  
![](../../../ref-mesa/mesa/texcyl.f20.png) ![](ours/texcyl.f20.png) ![](diff/texcyl.f20.png)

**texcyl f60: PASS**  
![](../../../ref-mesa/mesa/texcyl.f60.png) ![](ours/texcyl.f60.png) ![](diff/texcyl.f60.png)

**isosurf f1: PASS**  
![](../../../ref-mesa/mesa/isosurf.f1.png) ![](ours/isosurf.f1.png) ![](diff/isosurf.f1.png)

**testgl f3: PASS**  
![](../../../ref-mesa/mesa/testgl.f3.png) ![](ours/testgl.f3.png) ![](diff/testgl.f3.png)

**testgl f20: PASS**  
![](../../../ref-mesa/mesa/testgl.f20.png) ![](ours/testgl.f20.png) ![](diff/testgl.f20.png)

**testgl f60: PASS**  
![](../../../ref-mesa/mesa/testgl.f60.png) ![](ours/testgl.f60.png) ![](diff/testgl.f60.png)

**testgl2 f3: PASS**  
![](../../../ref-mesa/mesa/testgl2.f3.png) ![](ours/testgl2.f3.png) ![](diff/testgl2.f3.png)

**testgl2 f20: PASS**  
![](../../../ref-mesa/mesa/testgl2.f20.png) ![](ours/testgl2.f20.png) ![](diff/testgl2.f20.png)

**testgl2 f60: PASS**  
![](../../../ref-mesa/mesa/testgl2.f60.png) ![](ours/testgl2.f60.png) ![](diff/testgl2.f60.png)

**rrootage f3: PASS**  
![](../../../ref-mesa/mesa/rrootage.f3.png) ![](ours/rrootage.f3.png) ![](diff/rrootage.f3.png)

**rrootage f60: PASS**  
![](../../../ref-mesa/mesa/rrootage.f60.png) ![](ours/rrootage.f60.png) ![](diff/rrootage.f60.png)

**rrootage f300: PASS**  
![](../../../ref-mesa/mesa/rrootage.f300.png) ![](ours/rrootage.f300.png) ![](diff/rrootage.f300.png)


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
