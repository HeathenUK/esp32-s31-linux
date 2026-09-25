# glref report: phase1/suite

Implementation under test: **ours**; reference: Mesa llvmpipe (cached in `../ref-mesa`).
Xvfb 800x480x16, deterministic time (1/60 s per swap), LD_BIND_NOW=1. Thresholds: channel tolerance 16/255, tolerant-bad <= 1.00% to PASS (see tools/glref/compare.py for why).

**Apps: 18.** MISSING-SYMBOL 9, PASS 9

| app | verdict | command |
|---|---|---|
| glxgears | **PASS** | `$XD/glxgears -geometry 320x240+0+0` |
| glxinfo | **PASS** | `$XD/glxinfo` |
| glxheads | **PASS** | `$XD/glxheads` |
| manywin | **MISSING-SYMBOL** | `$XD/manywin 4` |
| offset | **PASS** | `$XD/offset` |
| glxgears_fbconfig | **PASS** | `$XD/glxgears_fbconfig` |
| gears | **PASS** | `$GD/gears -geometry 320x240+0+0` |
| morph3d | **PASS** | `$GD/morph3d -geometry 320x240+0+0` |
| bounce | **PASS** | `$GD/bounce -geometry 320x240+0+0` |
| spectex | **MISSING-SYMBOL** | `$GD/spectex -geometry 320x240+0+0` |
| geartrain | **PASS** | `$GD/geartrain -geometry 320x240+0+0` |
| ipers | **MISSING-SYMBOL** | `$GD/ipers -geometry 320x240+0+0` |
| terrain | **MISSING-SYMBOL** | `$GD/terrain -geometry 320x240+0+0` |
| tunnel | **MISSING-SYMBOL** | `$GD/tunnel -geometry 320x240+0+0` |
| fire | **MISSING-SYMBOL** | `$GD/fire -geometry 320x240+0+0` |
| teapot | **MISSING-SYMBOL** | `$GD/teapot -geometry 320x240+0+0` |
| texcyl | **MISSING-SYMBOL** | `$GD/texcyl -geometry 320x240+0+0` |
| isosurf | **MISSING-SYMBOL** | `$GD/isosurf` |

## Per frame

| app | frame | verdict | strict bad % | tolerant bad % | max err | mean err | ours run | mesa ref | notes |
|---|---|---|---|---|---|---|---|---|---|
| glxgears | 3 | **PASS** | 1.056 | 0.021 | 247 | 0.72 | ok | ok |  |
| glxgears | 20 | **PASS** | 1.047 | 0.047 | 247 | 0.75 | ok | ok |  |
| glxgears | 60 | **PASS** | 1.070 | 0.049 | 247 | 0.77 | ok | ok |  |
| glxinfo | 0 | **PASS** | - | - | - | - | exit | exit | OpenGL vendor string: s31; OpenGL renderer string: Software Rasterizer; OpenGL version string: 1.1 s31-tinygl; direct rendering: Yes; GLX version: 1.4 |
| glxheads | 3 | **PASS** | 0.316 | 0.000 | 132 | 5.76 | ok | ok |  |
| glxheads | 20 | **PASS** | 0.166 | 0.000 | 132 | 5.57 | ok | ok |  |
| glxheads | 60 | **PASS** | 0.232 | 0.000 | 132 | 5.66 | ok | ok |  |
| manywin | 4 | **MISSING-SYMBOL** | 2.265 | 2.259 | 255 | 3.22 | ok | ok | 1 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log:   GL_VERSION:  1.1 s31-tinygl; log:   GL_VENDOR:   s31; log:   GL_RENDERER: Software Rasterizer; log: glref: ok at swap 4 -> /src/artifacts/gl/phase1/suite/ours/manywin.f4 |
| manywin | 20 | **MISSING-SYMBOL** | 2.247 | 2.241 | 255 | 3.20 | ok | ok | 1 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log:   GL_VERSION:  1.1 s31-tinygl; log:   GL_VENDOR:   s31; log:   GL_RENDERER: Software Rasterizer; log: glref: ok at swap 20 -> /src/artifacts/gl/phase1/suite/ours/manywin.f20 |
| manywin | 60 | **MISSING-SYMBOL** | 2.228 | 2.226 | 255 | 3.17 | ok | ok | 1 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log:   GL_VERSION:  1.1 s31-tinygl; log:   GL_VENDOR:   s31; log:   GL_RENDERER: Software Rasterizer; log: glref: ok at swap 60 -> /src/artifacts/gl/phase1/suite/ours/manywin.f60 |
| offset | 1 | **PASS** | 2.454 | 0.420 | 255 | 9.88 | ok | ok |  |
| glxgears_fbconfig | 3 | **PASS** | 0.384 | 0.004 | 197 | 0.37 | ok | ok |  |
| glxgears_fbconfig | 20 | **PASS** | 0.414 | 0.003 | 197 | 0.28 | ok | ok |  |
| glxgears_fbconfig | 60 | **PASS** | 0.334 | 0.003 | 197 | 0.60 | ok | ok |  |
| gears | 3 | **PASS** | 1.077 | 0.035 | 247 | 0.73 | ok | ok |  |
| gears | 20 | **PASS** | 1.158 | 0.025 | 247 | 0.76 | ok | ok |  |
| gears | 60 | **PASS** | 1.188 | 0.048 | 247 | 0.81 | ok | ok |  |
| morph3d | 3 | **PASS** | 0.198 | 0.177 | 255 | 0.19 | ok | ok |  |
| morph3d | 20 | **PASS** | 0.191 | 0.184 | 255 | 0.21 | ok | ok |  |
| morph3d | 60 | **PASS** | 0.517 | 0.501 | 255 | 0.64 | ok | ok |  |
| bounce | 3 | **PASS** | 6.837 | 0.030 | 255 | 11.62 | ok | ok |  |
| bounce | 20 | **PASS** | 6.747 | 0.013 | 255 | 11.50 | ok | ok |  |
| bounce | 60 | **PASS** | 6.822 | 0.048 | 255 | 11.60 | ok | ok |  |
| spectex | 3 | **MISSING-SYMBOL** | 13.259 | 13.225 | 255 | 8.63 | ok | ok | 2 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log: libGL: context 3 created (pid 2077, comm spectex); log: libGL: unimplemented GL_SEPARATE_SPECULAR_COLOR; log: libGL: unimplemented texturing with GL_TEXTURE_ENV_MODE other than GL_REPLACE/GL_DECAL (drawn as GL_REPLACE); log: glref: ok at swap 3 -> /src/artifacts/gl/phase1/suite/ours/spectex.f3 |
| spectex | 20 | **MISSING-SYMBOL** | 13.309 | 13.280 | 255 | 8.94 | ok | ok | 2 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log: libGL: context 3 created (pid 2419, comm spectex); log: libGL: unimplemented GL_SEPARATE_SPECULAR_COLOR; log: libGL: unimplemented texturing with GL_TEXTURE_ENV_MODE other than GL_REPLACE/GL_DECAL (drawn as GL_REPLACE); log: glref: ok at swap 20 -> /src/artifacts/gl/phase1/suite/ours/spectex.f20 |
| spectex | 60 | **MISSING-SYMBOL** | 13.191 | 13.168 | 255 | 8.04 | ok | ok | 2 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log: libGL: context 3 created (pid 2503, comm spectex); log: libGL: unimplemented GL_SEPARATE_SPECULAR_COLOR; log: libGL: unimplemented texturing with GL_TEXTURE_ENV_MODE other than GL_REPLACE/GL_DECAL (drawn as GL_REPLACE); log: glref: ok at swap 60 -> /src/artifacts/gl/phase1/suite/ours/spectex.f60 |
| geartrain | 3 | **PASS** | 3.677 | 0.850 | 230 | 3.65 | ok | ok |  |
| geartrain | 20 | **PASS** | 3.577 | 0.790 | 230 | 3.60 | ok | ok |  |
| geartrain | 60 | **PASS** | 3.680 | 0.883 | 230 | 3.68 | ok | ok |  |
| ipers | 3 | **MISSING-SYMBOL** | 75.604 | 75.604 | 255 | 48.31 | ok | ok | 9 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log: libGL: unimplemented glBitmap; log: libGL: unimplemented glPopClientAttrib; log: libGL: unimplemented glEnable(GL_BLEND); log: glref: ok at swap 3 -> /src/artifacts/gl/phase1/suite/ours/ipers.f3 |
| ipers | 20 | **MISSING-SYMBOL** | 75.604 | 75.604 | 255 | 48.35 | ok | ok | 9 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log: libGL: unimplemented glBitmap; log: libGL: unimplemented glPopClientAttrib; log: libGL: unimplemented glEnable(GL_BLEND); log: glref: ok at swap 20 -> /src/artifacts/gl/phase1/suite/ours/ipers.f20 |
| ipers | 60 | **MISSING-SYMBOL** | 75.604 | 75.604 | 255 | 48.38 | ok | ok | 9 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log: libGL: unimplemented glBitmap; log: libGL: unimplemented glPopClientAttrib; log: libGL: unimplemented glEnable(GL_BLEND); log: glref: ok at swap 60 -> /src/artifacts/gl/phase1/suite/ours/ipers.f60 |
| terrain | 3 | **MISSING-SYMBOL** | 86.156 | 86.156 | 255 | 69.07 | ok | ok | 7 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log: libGL: unimplemented glPushClientAttrib; log: libGL: unimplemented glBitmap; log: libGL: unimplemented glPopClientAttrib; log: glref: ok at swap 3 -> /src/artifacts/gl/phase1/suite/ours/terrain.f3 |
| terrain | 20 | **MISSING-SYMBOL** | 86.217 | 86.217 | 255 | 69.78 | ok | ok | 7 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log: libGL: unimplemented glPushClientAttrib; log: libGL: unimplemented glBitmap; log: libGL: unimplemented glPopClientAttrib; log: glref: ok at swap 20 -> /src/artifacts/gl/phase1/suite/ours/terrain.f20 |
| terrain | 60 | **MISSING-SYMBOL** | 86.232 | 86.232 | 255 | 69.25 | ok | ok | 7 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log: libGL: unimplemented glPushClientAttrib; log: libGL: unimplemented glBitmap; log: libGL: unimplemented glPopClientAttrib; log: glref: ok at swap 60 -> /src/artifacts/gl/phase1/suite/ours/terrain.f60 |
| tunnel | 3 | **MISSING-SYMBOL** | 82.428 | 80.171 | 255 | 37.51 | ok | ok | 8 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log: libGL: unimplemented glPushClientAttrib; log: libGL: unimplemented glBitmap; log: libGL: unimplemented glPopClientAttrib; log: glref: ok at swap 3 -> /src/artifacts/gl/phase1/suite/ours/tunnel.f3 |
| tunnel | 20 | **MISSING-SYMBOL** | 80.725 | 78.168 | 255 | 37.48 | ok | ok | 8 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log: libGL: unimplemented glPushClientAttrib; log: libGL: unimplemented glBitmap; log: libGL: unimplemented glPopClientAttrib; log: glref: ok at swap 20 -> /src/artifacts/gl/phase1/suite/ours/tunnel.f20 |
| tunnel | 60 | **MISSING-SYMBOL** | 81.853 | 79.775 | 255 | 40.88 | ok | ok | 8 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log: libGL: unimplemented glPushClientAttrib; log: libGL: unimplemented glBitmap; log: libGL: unimplemented glPopClientAttrib; log: glref: ok at swap 60 -> /src/artifacts/gl/phase1/suite/ours/tunnel.f60 |
| fire | 3 | **MISSING-SYMBOL** | 88.979 | 88.846 | 255 | 88.17 | ok | ok | 9 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log: libGL: unimplemented glPushClientAttrib; log: libGL: unimplemented glBitmap; log: libGL: unimplemented glPopClientAttrib; log: glref: ok at swap 3 -> /src/artifacts/gl/phase1/suite/ours/fire.f3 |
| fire | 20 | **MISSING-SYMBOL** | 89.376 | 89.243 | 255 | 92.72 | ok | ok | 9 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log: libGL: unimplemented glPushClientAttrib; log: libGL: unimplemented glBitmap; log: libGL: unimplemented glPopClientAttrib; log: glref: ok at swap 20 -> /src/artifacts/gl/phase1/suite/ours/fire.f20 |
| fire | 60 | **MISSING-SYMBOL** | 89.151 | 89.018 | 255 | 89.68 | ok | ok | 9 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log: libGL: unimplemented glPushClientAttrib; log: libGL: unimplemented glBitmap; log: libGL: unimplemented glPopClientAttrib; log: glref: ok at swap 60 -> /src/artifacts/gl/phase1/suite/ours/fire.f60 |
| teapot | 3 | **MISSING-SYMBOL** | 85.803 | 85.803 | 255 | 50.72 | ok | ok | 9 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log: libGL: unimplemented glBitmap; log: libGL: unimplemented glPopClientAttrib; log: libGL: unimplemented glEnable(GL_BLEND); log: glref: ok at swap 3 -> /src/artifacts/gl/phase1/suite/ours/teapot.f3 |
| teapot | 20 | **MISSING-SYMBOL** | 85.829 | 85.829 | 255 | 50.58 | ok | ok | 9 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log: libGL: unimplemented glBitmap; log: libGL: unimplemented glPopClientAttrib; log: libGL: unimplemented glEnable(GL_BLEND); log: glref: ok at swap 20 -> /src/artifacts/gl/phase1/suite/ours/teapot.f20 |
| teapot | 60 | **MISSING-SYMBOL** | 85.646 | 85.646 | 255 | 51.32 | ok | ok | 9 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: FAIL); log: libGL: unimplemented glBitmap; log: libGL: unimplemented glPopClientAttrib; log: libGL: unimplemented glEnable(GL_BLEND); log: glref: ok at swap 60 -> /src/artifacts/gl/phase1/suite/ours/teapot.f60 |
| texcyl | 3 | **MISSING-SYMBOL** | 0.164 | 0.000 | 73 | 0.30 | ok | ok | 1 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: PASS); log: libGL: unimplemented glTexGeni; log: libGL: context 2 created (pid 3642, comm texcyl); log: libGL: context 3 created (pid 3642, comm texcyl); log: glref: ok at swap 3 -> /src/artifacts/gl/phase1/suite/ours/texcyl.f3 |
| texcyl | 20 | **MISSING-SYMBOL** | 0.212 | 0.004 | 61 | 0.49 | ok | ok | 1 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: PASS); log: libGL: unimplemented glTexGeni; log: libGL: context 2 created (pid 3731, comm texcyl); log: libGL: context 3 created (pid 3731, comm texcyl); log: glref: ok at swap 20 -> /src/artifacts/gl/phase1/suite/ours/texcyl.f20 |
| texcyl | 60 | **MISSING-SYMBOL** | 0.236 | 0.000 | 69 | 0.33 | ok | ok | 1 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: PASS); log: libGL: unimplemented glTexGeni; log: libGL: context 2 created (pid 3794, comm texcyl); log: libGL: context 3 created (pid 3794, comm texcyl); log: glref: ok at swap 60 -> /src/artifacts/gl/phase1/suite/ours/texcyl.f60 |
| isosurf | 1 | **MISSING-SYMBOL** | 0.351 | 0.035 | 197 | 0.58 | ok | ok | 3 'libGL: unimplemented' lines; counted as MISSING-SYMBOL (image alone: PASS); log: Compiled vertex arrays not supported by this renderer; log: Nr unique vertex/normal pairs: 2723; log: num_tri_verts: 21531; log: glref: ok at swap 1 -> /src/artifacts/gl/phase1/suite/ours/isosurf.f1 |

## Images

Mesa reference, ours, diff (grey = reference, red = tolerant-bad, yellow = edge-only). EXACT frames have no diff image.

**glxgears f3: PASS**  
![](../../ref-mesa/mesa/glxgears.f3.png) ![](ours/glxgears.f3.png) ![](diff/glxgears.f3.png)

**glxgears f20: PASS**  
![](../../ref-mesa/mesa/glxgears.f20.png) ![](ours/glxgears.f20.png) ![](diff/glxgears.f20.png)

**glxgears f60: PASS**  
![](../../ref-mesa/mesa/glxgears.f60.png) ![](ours/glxgears.f60.png) ![](diff/glxgears.f60.png)

**glxheads f3: PASS**  
![](../../ref-mesa/mesa/glxheads.f3.png) ![](ours/glxheads.f3.png) ![](diff/glxheads.f3.png)

**glxheads f20: PASS**  
![](../../ref-mesa/mesa/glxheads.f20.png) ![](ours/glxheads.f20.png) ![](diff/glxheads.f20.png)

**glxheads f60: PASS**  
![](../../ref-mesa/mesa/glxheads.f60.png) ![](ours/glxheads.f60.png) ![](diff/glxheads.f60.png)

**manywin f4: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/manywin.f4.png) ![](ours/manywin.f4.png) ![](diff/manywin.f4.png)

**manywin f20: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/manywin.f20.png) ![](ours/manywin.f20.png) ![](diff/manywin.f20.png)

**manywin f60: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/manywin.f60.png) ![](ours/manywin.f60.png) ![](diff/manywin.f60.png)

**offset f1: PASS**  
![](../../ref-mesa/mesa/offset.f1.png) ![](ours/offset.f1.png) ![](diff/offset.f1.png)

**glxgears_fbconfig f3: PASS**  
![](../../ref-mesa/mesa/glxgears_fbconfig.f3.png) ![](ours/glxgears_fbconfig.f3.png) ![](diff/glxgears_fbconfig.f3.png)

**glxgears_fbconfig f20: PASS**  
![](../../ref-mesa/mesa/glxgears_fbconfig.f20.png) ![](ours/glxgears_fbconfig.f20.png) ![](diff/glxgears_fbconfig.f20.png)

**glxgears_fbconfig f60: PASS**  
![](../../ref-mesa/mesa/glxgears_fbconfig.f60.png) ![](ours/glxgears_fbconfig.f60.png) ![](diff/glxgears_fbconfig.f60.png)

**gears f3: PASS**  
![](../../ref-mesa/mesa/gears.f3.png) ![](ours/gears.f3.png) ![](diff/gears.f3.png)

**gears f20: PASS**  
![](../../ref-mesa/mesa/gears.f20.png) ![](ours/gears.f20.png) ![](diff/gears.f20.png)

**gears f60: PASS**  
![](../../ref-mesa/mesa/gears.f60.png) ![](ours/gears.f60.png) ![](diff/gears.f60.png)

**morph3d f3: PASS**  
![](../../ref-mesa/mesa/morph3d.f3.png) ![](ours/morph3d.f3.png) ![](diff/morph3d.f3.png)

**morph3d f20: PASS**  
![](../../ref-mesa/mesa/morph3d.f20.png) ![](ours/morph3d.f20.png) ![](diff/morph3d.f20.png)

**morph3d f60: PASS**  
![](../../ref-mesa/mesa/morph3d.f60.png) ![](ours/morph3d.f60.png) ![](diff/morph3d.f60.png)

**bounce f3: PASS**  
![](../../ref-mesa/mesa/bounce.f3.png) ![](ours/bounce.f3.png) ![](diff/bounce.f3.png)

**bounce f20: PASS**  
![](../../ref-mesa/mesa/bounce.f20.png) ![](ours/bounce.f20.png) ![](diff/bounce.f20.png)

**bounce f60: PASS**  
![](../../ref-mesa/mesa/bounce.f60.png) ![](ours/bounce.f60.png) ![](diff/bounce.f60.png)

**spectex f3: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/spectex.f3.png) ![](ours/spectex.f3.png) ![](diff/spectex.f3.png)

**spectex f20: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/spectex.f20.png) ![](ours/spectex.f20.png) ![](diff/spectex.f20.png)

**spectex f60: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/spectex.f60.png) ![](ours/spectex.f60.png) ![](diff/spectex.f60.png)

**geartrain f3: PASS**  
![](../../ref-mesa/mesa/geartrain.f3.png) ![](ours/geartrain.f3.png) ![](diff/geartrain.f3.png)

**geartrain f20: PASS**  
![](../../ref-mesa/mesa/geartrain.f20.png) ![](ours/geartrain.f20.png) ![](diff/geartrain.f20.png)

**geartrain f60: PASS**  
![](../../ref-mesa/mesa/geartrain.f60.png) ![](ours/geartrain.f60.png) ![](diff/geartrain.f60.png)

**ipers f3: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/ipers.f3.png) ![](ours/ipers.f3.png) ![](diff/ipers.f3.png)

**ipers f20: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/ipers.f20.png) ![](ours/ipers.f20.png) ![](diff/ipers.f20.png)

**ipers f60: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/ipers.f60.png) ![](ours/ipers.f60.png) ![](diff/ipers.f60.png)

**terrain f3: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/terrain.f3.png) ![](ours/terrain.f3.png) ![](diff/terrain.f3.png)

**terrain f20: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/terrain.f20.png) ![](ours/terrain.f20.png) ![](diff/terrain.f20.png)

**terrain f60: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/terrain.f60.png) ![](ours/terrain.f60.png) ![](diff/terrain.f60.png)

**tunnel f3: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/tunnel.f3.png) ![](ours/tunnel.f3.png) ![](diff/tunnel.f3.png)

**tunnel f20: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/tunnel.f20.png) ![](ours/tunnel.f20.png) ![](diff/tunnel.f20.png)

**tunnel f60: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/tunnel.f60.png) ![](ours/tunnel.f60.png) ![](diff/tunnel.f60.png)

**fire f3: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/fire.f3.png) ![](ours/fire.f3.png) ![](diff/fire.f3.png)

**fire f20: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/fire.f20.png) ![](ours/fire.f20.png) ![](diff/fire.f20.png)

**fire f60: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/fire.f60.png) ![](ours/fire.f60.png) ![](diff/fire.f60.png)

**teapot f3: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/teapot.f3.png) ![](ours/teapot.f3.png) ![](diff/teapot.f3.png)

**teapot f20: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/teapot.f20.png) ![](ours/teapot.f20.png) ![](diff/teapot.f20.png)

**teapot f60: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/teapot.f60.png) ![](ours/teapot.f60.png) ![](diff/teapot.f60.png)

**texcyl f3: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/texcyl.f3.png) ![](ours/texcyl.f3.png) ![](diff/texcyl.f3.png)

**texcyl f20: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/texcyl.f20.png) ![](ours/texcyl.f20.png) ![](diff/texcyl.f20.png)

**texcyl f60: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/texcyl.f60.png) ![](ours/texcyl.f60.png) ![](diff/texcyl.f60.png)

**isosurf f1: MISSING-SYMBOL**  
![](../../ref-mesa/mesa/isosurf.f1.png) ![](ours/isosurf.f1.png) ![](diff/isosurf.f1.png)

