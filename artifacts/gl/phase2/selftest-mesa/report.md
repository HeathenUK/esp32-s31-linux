# glref report: phase2/selftest-mesa

Implementation under test: **mesa**; reference: Mesa llvmpipe (cached in `../ref-mesa`).
Xvfb 800x480x16, deterministic time (1/60 s per swap), LD_BIND_NOW=1. Thresholds: channel tolerance 16/255, tolerant-bad <= 1.00% to PASS (see tools/glref/compare.py for why).

**Apps: 22.** PASS 1, EXACT 21

| app | verdict | command |
|---|---|---|
| glxgears | **EXACT** | `$XD/glxgears -geometry 320x240+0+0` |
| glxinfo | **PASS** | `$XD/glxinfo` |
| glxheads | **EXACT** | `$XD/glxheads` |
| manywin | **EXACT** | `$XD/manywin 4` |
| multictx | **EXACT** | `$XD/multictx` |
| offset | **EXACT** | `$XD/offset` |
| glxgears_fbconfig | **EXACT** | `$XD/glxgears_fbconfig` |
| gears | **EXACT** | `$GD/gears -geometry 320x240+0+0` |
| morph3d | **EXACT** | `$GD/morph3d -geometry 320x240+0+0` |
| bounce | **EXACT** | `$GD/bounce -geometry 320x240+0+0` |
| spectex | **EXACT** | `$GD/spectex -geometry 320x240+0+0` |
| geartrain | **EXACT** | `$GD/geartrain -geometry 320x240+0+0` |
| ipers | **EXACT** | `$GD/ipers -geometry 320x240+0+0` |
| terrain | **EXACT** | `$GD/terrain -geometry 320x240+0+0` |
| tunnel | **EXACT** | `$GD/tunnel -geometry 320x240+0+0` |
| fire | **EXACT** | `$GD/fire -geometry 320x240+0+0` |
| teapot | **EXACT** | `$GD/teapot -geometry 320x240+0+0` |
| texcyl | **EXACT** | `$GD/texcyl -geometry 320x240+0+0` |
| isosurf | **EXACT** | `$GD/isosurf` |
| testgl | **EXACT** | `$SB/sdl12-test/testgl` |
| testgl2 | **EXACT** | `$SB/sdl2-test/testgl2 --geometry 320x240` |
| rrootage | **EXACT** | `$SB/rrootage/rr -lowres -window -nosound` |

## Per frame

| app | frame | verdict | strict bad % | tolerant bad % | max err | mean err | mesa run | mesa ref | notes |
|---|---|---|---|---|---|---|---|---|---|
| glxgears | 3 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| glxgears | 20 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| glxgears | 60 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| glxinfo | 0 | **PASS** | - | - | - | - | exit | exit | OpenGL vendor string: Mesa; OpenGL renderer string: llvmpipe (LLVM 19.1.7, 128 bits); OpenGL version string: 4.5 (Compatibility Profile) Mesa 25.0.7-2+deb13u1; direct rendering: Yes; GLX version: 1.4 |
| glxheads | 3 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| glxheads | 20 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| glxheads | 60 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| manywin | 4 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| manywin | 20 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| manywin | 60 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| multictx | 20 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| offset | 1 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| glxgears_fbconfig | 3 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| glxgears_fbconfig | 20 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| glxgears_fbconfig | 60 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| gears | 3 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| gears | 20 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| gears | 60 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| morph3d | 3 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| morph3d | 20 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| morph3d | 60 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| bounce | 3 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| bounce | 20 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| bounce | 60 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| spectex | 3 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| spectex | 20 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| spectex | 60 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| geartrain | 3 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| geartrain | 20 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| geartrain | 60 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| ipers | 3 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| ipers | 20 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| ipers | 60 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| terrain | 3 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| terrain | 20 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| terrain | 60 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| tunnel | 3 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| tunnel | 20 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| tunnel | 60 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| fire | 3 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| fire | 20 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| fire | 60 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| teapot | 3 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| teapot | 20 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| teapot | 60 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| texcyl | 3 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| texcyl | 20 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| texcyl | 60 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| isosurf | 1 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| testgl | 3 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| testgl | 20 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| testgl | 60 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| testgl2 | 3 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| testgl2 | 20 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| testgl2 | 60 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| rrootage | 3 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| rrootage | 60 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |
| rrootage | 300 | **EXACT** | 0.000 | 0.000 | 0 | 0.00 | ok | ok |  |

## Images

Mesa reference, mesa, diff (grey = reference, red = tolerant-bad, yellow = edge-only). EXACT frames have no diff image.

**glxgears f3: EXACT**  
![](../../ref-mesa/mesa/glxgears.f3.png) ![](mesa/glxgears.f3.png) -

**glxgears f20: EXACT**  
![](../../ref-mesa/mesa/glxgears.f20.png) ![](mesa/glxgears.f20.png) -

**glxgears f60: EXACT**  
![](../../ref-mesa/mesa/glxgears.f60.png) ![](mesa/glxgears.f60.png) -

**glxheads f3: EXACT**  
![](../../ref-mesa/mesa/glxheads.f3.png) ![](mesa/glxheads.f3.png) -

**glxheads f20: EXACT**  
![](../../ref-mesa/mesa/glxheads.f20.png) ![](mesa/glxheads.f20.png) -

**glxheads f60: EXACT**  
![](../../ref-mesa/mesa/glxheads.f60.png) ![](mesa/glxheads.f60.png) -

**manywin f4: EXACT**  
![](../../ref-mesa/mesa/manywin.f4.png) ![](mesa/manywin.f4.png) -

**manywin f20: EXACT**  
![](../../ref-mesa/mesa/manywin.f20.png) ![](mesa/manywin.f20.png) -

**manywin f60: EXACT**  
![](../../ref-mesa/mesa/manywin.f60.png) ![](mesa/manywin.f60.png) -

**multictx f20: EXACT**  
![](../../ref-mesa/mesa/multictx.f20.png) ![](mesa/multictx.f20.png) -

**offset f1: EXACT**  
![](../../ref-mesa/mesa/offset.f1.png) ![](mesa/offset.f1.png) -

**glxgears_fbconfig f3: EXACT**  
![](../../ref-mesa/mesa/glxgears_fbconfig.f3.png) ![](mesa/glxgears_fbconfig.f3.png) -

**glxgears_fbconfig f20: EXACT**  
![](../../ref-mesa/mesa/glxgears_fbconfig.f20.png) ![](mesa/glxgears_fbconfig.f20.png) -

**glxgears_fbconfig f60: EXACT**  
![](../../ref-mesa/mesa/glxgears_fbconfig.f60.png) ![](mesa/glxgears_fbconfig.f60.png) -

**gears f3: EXACT**  
![](../../ref-mesa/mesa/gears.f3.png) ![](mesa/gears.f3.png) -

**gears f20: EXACT**  
![](../../ref-mesa/mesa/gears.f20.png) ![](mesa/gears.f20.png) -

**gears f60: EXACT**  
![](../../ref-mesa/mesa/gears.f60.png) ![](mesa/gears.f60.png) -

**morph3d f3: EXACT**  
![](../../ref-mesa/mesa/morph3d.f3.png) ![](mesa/morph3d.f3.png) -

**morph3d f20: EXACT**  
![](../../ref-mesa/mesa/morph3d.f20.png) ![](mesa/morph3d.f20.png) -

**morph3d f60: EXACT**  
![](../../ref-mesa/mesa/morph3d.f60.png) ![](mesa/morph3d.f60.png) -

**bounce f3: EXACT**  
![](../../ref-mesa/mesa/bounce.f3.png) ![](mesa/bounce.f3.png) -

**bounce f20: EXACT**  
![](../../ref-mesa/mesa/bounce.f20.png) ![](mesa/bounce.f20.png) -

**bounce f60: EXACT**  
![](../../ref-mesa/mesa/bounce.f60.png) ![](mesa/bounce.f60.png) -

**spectex f3: EXACT**  
![](../../ref-mesa/mesa/spectex.f3.png) ![](mesa/spectex.f3.png) -

**spectex f20: EXACT**  
![](../../ref-mesa/mesa/spectex.f20.png) ![](mesa/spectex.f20.png) -

**spectex f60: EXACT**  
![](../../ref-mesa/mesa/spectex.f60.png) ![](mesa/spectex.f60.png) -

**geartrain f3: EXACT**  
![](../../ref-mesa/mesa/geartrain.f3.png) ![](mesa/geartrain.f3.png) -

**geartrain f20: EXACT**  
![](../../ref-mesa/mesa/geartrain.f20.png) ![](mesa/geartrain.f20.png) -

**geartrain f60: EXACT**  
![](../../ref-mesa/mesa/geartrain.f60.png) ![](mesa/geartrain.f60.png) -

**ipers f3: EXACT**  
![](../../ref-mesa/mesa/ipers.f3.png) ![](mesa/ipers.f3.png) -

**ipers f20: EXACT**  
![](../../ref-mesa/mesa/ipers.f20.png) ![](mesa/ipers.f20.png) -

**ipers f60: EXACT**  
![](../../ref-mesa/mesa/ipers.f60.png) ![](mesa/ipers.f60.png) -

**terrain f3: EXACT**  
![](../../ref-mesa/mesa/terrain.f3.png) ![](mesa/terrain.f3.png) -

**terrain f20: EXACT**  
![](../../ref-mesa/mesa/terrain.f20.png) ![](mesa/terrain.f20.png) -

**terrain f60: EXACT**  
![](../../ref-mesa/mesa/terrain.f60.png) ![](mesa/terrain.f60.png) -

**tunnel f3: EXACT**  
![](../../ref-mesa/mesa/tunnel.f3.png) ![](mesa/tunnel.f3.png) -

**tunnel f20: EXACT**  
![](../../ref-mesa/mesa/tunnel.f20.png) ![](mesa/tunnel.f20.png) -

**tunnel f60: EXACT**  
![](../../ref-mesa/mesa/tunnel.f60.png) ![](mesa/tunnel.f60.png) -

**fire f3: EXACT**  
![](../../ref-mesa/mesa/fire.f3.png) ![](mesa/fire.f3.png) -

**fire f20: EXACT**  
![](../../ref-mesa/mesa/fire.f20.png) ![](mesa/fire.f20.png) -

**fire f60: EXACT**  
![](../../ref-mesa/mesa/fire.f60.png) ![](mesa/fire.f60.png) -

**teapot f3: EXACT**  
![](../../ref-mesa/mesa/teapot.f3.png) ![](mesa/teapot.f3.png) -

**teapot f20: EXACT**  
![](../../ref-mesa/mesa/teapot.f20.png) ![](mesa/teapot.f20.png) -

**teapot f60: EXACT**  
![](../../ref-mesa/mesa/teapot.f60.png) ![](mesa/teapot.f60.png) -

**texcyl f3: EXACT**  
![](../../ref-mesa/mesa/texcyl.f3.png) ![](mesa/texcyl.f3.png) -

**texcyl f20: EXACT**  
![](../../ref-mesa/mesa/texcyl.f20.png) ![](mesa/texcyl.f20.png) -

**texcyl f60: EXACT**  
![](../../ref-mesa/mesa/texcyl.f60.png) ![](mesa/texcyl.f60.png) -

**isosurf f1: EXACT**  
![](../../ref-mesa/mesa/isosurf.f1.png) ![](mesa/isosurf.f1.png) -

**testgl f3: EXACT**  
![](../../ref-mesa/mesa/testgl.f3.png) ![](mesa/testgl.f3.png) -

**testgl f20: EXACT**  
![](../../ref-mesa/mesa/testgl.f20.png) ![](mesa/testgl.f20.png) -

**testgl f60: EXACT**  
![](../../ref-mesa/mesa/testgl.f60.png) ![](mesa/testgl.f60.png) -

**testgl2 f3: EXACT**  
![](../../ref-mesa/mesa/testgl2.f3.png) ![](mesa/testgl2.f3.png) -

**testgl2 f20: EXACT**  
![](../../ref-mesa/mesa/testgl2.f20.png) ![](mesa/testgl2.f20.png) -

**testgl2 f60: EXACT**  
![](../../ref-mesa/mesa/testgl2.f60.png) ![](mesa/testgl2.f60.png) -

**rrootage f3: EXACT**  
![](../../ref-mesa/mesa/rrootage.f3.png) ![](mesa/rrootage.f3.png) -

**rrootage f60: EXACT**  
![](../../ref-mesa/mesa/rrootage.f60.png) ![](mesa/rrootage.f60.png) -

**rrootage f300: EXACT**  
![](../../ref-mesa/mesa/rrootage.f300.png) ![](mesa/rrootage.f300.png) -

