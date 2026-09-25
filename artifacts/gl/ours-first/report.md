# glref report: ours-first

Implementation under test: **ours**; reference: Mesa llvmpipe (cached in `ref-mesa`).
Xvfb 800x480x16, deterministic time (1/60 s per swap), LD_BIND_NOW=1. Thresholds: channel tolerance 16/255, tolerant-bad <= 1.00% to PASS (see tools/glref/compare.py for why).

**Apps: 18.** MISSING-SYMBOL 18

| app | verdict | command |
|---|---|---|
| glxgears | **MISSING-SYMBOL** | `$XD/glxgears -geometry 320x240+0+0` |
| glxinfo | **MISSING-SYMBOL** | `$XD/glxinfo` |
| glxheads | **MISSING-SYMBOL** | `$XD/glxheads` |
| manywin | **MISSING-SYMBOL** | `$XD/manywin 4` |
| offset | **MISSING-SYMBOL** | `$XD/offset` |
| glxgears_fbconfig | **MISSING-SYMBOL** | `$XD/glxgears_fbconfig` |
| gears | **MISSING-SYMBOL** | `$GD/gears -geometry 320x240+0+0` |
| morph3d | **MISSING-SYMBOL** | `$GD/morph3d -geometry 320x240+0+0` |
| bounce | **MISSING-SYMBOL** | `$GD/bounce -geometry 320x240+0+0` |
| spectex | **MISSING-SYMBOL** | `$GD/spectex -geometry 320x240+0+0` |
| geartrain | **MISSING-SYMBOL** | `$GD/geartrain -geometry 320x240+0+0` |
| ipers | **MISSING-SYMBOL** | `$GD/ipers -geometry 320x240+0+0` |
| terrain | **MISSING-SYMBOL** | `$GD/terrain -geometry 320x240+0+0` |
| tunnel | **MISSING-SYMBOL** | `$GD/tunnel -geometry 320x240+0+0` |
| fire | **MISSING-SYMBOL** | `$GD/fire -geometry 320x240+0+0` |
| teapot | **MISSING-SYMBOL** | `$GD/teapot -geometry 320x240+0+0` |
| texcyl | **MISSING-SYMBOL** | `$GD/texcyl -geometry 320x240+0+0` |
| isosurf | **MISSING-SYMBOL** | `$GD/isosurf` |

## GL entry points the apps import that ours does not export (24)

| symbol | apps |
|---|---|
| `glXChooseFBConfig` | bounce, fire, gears, geartrain, glxgears_fbconfig, glxinfo, ipers, isosurf, morph3d, spectex, teapot, terrain, texcyl, tunnel |
| `glXChooseVisual` | glxgears, glxheads, glxinfo, manywin, offset |
| `glXCreateContext` | glxgears, glxheads, glxinfo, manywin, offset |
| `glXCreateNewContext` | bounce, fire, gears, geartrain, glxgears_fbconfig, glxinfo, ipers, isosurf, morph3d, spectex, teapot, terrain, texcyl, tunnel |
| `glXCreatePbuffer` | glxgears_fbconfig |
| `glXDestroyContext` | bounce, fire, gears, geartrain, glxgears, glxgears_fbconfig, glxinfo, ipers, isosurf, manywin, morph3d, spectex, teapot, terrain, texcyl, tunnel |
| `glXDestroyPbuffer` | glxgears_fbconfig |
| `glXGetClientString` | glxinfo |
| `glXGetConfig` | glxinfo |
| `glXGetCurrentContext` | bounce, fire, gears, geartrain, ipers, isosurf, morph3d, spectex, teapot, terrain, texcyl, tunnel |
| `glXGetFBConfigAttrib` | bounce, fire, gears, geartrain, glxgears_fbconfig, glxinfo, ipers, isosurf, morph3d, spectex, teapot, terrain, texcyl, tunnel |
| `glXGetFBConfigs` | glxgears_fbconfig, glxinfo |
| `glXGetProcAddress` | glxgears_fbconfig, glxinfo |
| `glXGetProcAddressARB` | bounce, fire, gears, geartrain, glxgears, glxgears_fbconfig, glxinfo, ipers, isosurf, morph3d, spectex, teapot, terrain, texcyl, tunnel |
| `glXGetVisualFromFBConfig` | bounce, fire, gears, geartrain, glxgears_fbconfig, glxinfo, ipers, isosurf, morph3d, spectex, teapot, terrain, texcyl, tunnel |
| `glXIsDirect` | bounce, fire, gears, geartrain, glxinfo, ipers, isosurf, morph3d, spectex, teapot, terrain, texcyl, tunnel |
| `glXMakeContextCurrent` | bounce, fire, gears, geartrain, ipers, isosurf, morph3d, spectex, teapot, terrain, texcyl, tunnel |
| `glXMakeCurrent` | glxgears, glxgears_fbconfig, glxheads, glxinfo, manywin, offset |
| `glXQueryDrawable` | glxgears |
| `glXQueryExtension` | bounce, fire, gears, geartrain, ipers, isosurf, morph3d, spectex, teapot, terrain, texcyl, tunnel |
| `glXQueryExtensionsString` | bounce, fire, gears, geartrain, glxgears, glxgears_fbconfig, glxinfo, ipers, isosurf, morph3d, spectex, teapot, terrain, texcyl, tunnel |
| `glXQueryServerString` | glxgears_fbconfig, glxinfo |
| `glXQueryVersion` | glxgears_fbconfig, glxinfo |
| `glXSwapBuffers` | bounce, fire, gears, geartrain, glxgears, glxgears_fbconfig, glxheads, ipers, isosurf, manywin, morph3d, offset, spectex, teapot, terrain, texcyl, tunnel |

## Per frame

| app | frame | verdict | strict bad % | tolerant bad % | max err | mean err | ours run | mesa ref | notes |
|---|---|---|---|---|---|---|---|---|---|
| glxgears | 3 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (8): glXChooseVisual glXCreateContext glXDestroyContext glXGetProcAddressARB glXMakeCurrent glXQueryDrawable glXQueryExtensionsString glXSwapBuffers; missing: glXDestroyContext; log: /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxgears: symbol lookup error: /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxgears: undefined symbol: glXDestroyContext |
| glxgears | 20 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (8): glXChooseVisual glXCreateContext glXDestroyContext glXGetProcAddressARB glXMakeCurrent glXQueryDrawable glXQueryExtensionsString glXSwapBuffers; missing: glXDestroyContext; log: /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxgears: symbol lookup error: /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxgears: undefined symbol: glXDestroyContext |
| glxgears | 60 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (8): glXChooseVisual glXCreateContext glXDestroyContext glXGetProcAddressARB glXMakeCurrent glXQueryDrawable glXQueryExtensionsString glXSwapBuffers; missing: glXDestroyContext; log: /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxgears: symbol lookup error: /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxgears: undefined symbol: glXDestroyContext |
| glxinfo | 0 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | exit | all unresolved GL imports (17): glXChooseFBConfig glXChooseVisual glXCreateContext glXCreateNewContext glXDestroyContext glXGetClientString glXGetConfig glXGetFBConfigAttrib glXGetFBConfigs glXGetProcAddress glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeCurrent glXQueryExtensionsString glXQueryServerString glXQueryVersion; missing: glXGetConfig; log: /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxinfo: symbol lookup error: /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxinfo: undefined symbol: glXGetConfig |
| glxheads | 3 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (4): glXChooseVisual glXCreateContext glXMakeCurrent glXSwapBuffers; missing: glXCreateContext; log: /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxheads: symbol lookup error: /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxheads: undefined symbol: glXCreateContext |
| glxheads | 20 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (4): glXChooseVisual glXCreateContext glXMakeCurrent glXSwapBuffers; missing: glXCreateContext; log: /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxheads: symbol lookup error: /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxheads: undefined symbol: glXCreateContext |
| glxheads | 60 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (4): glXChooseVisual glXCreateContext glXMakeCurrent glXSwapBuffers; missing: glXCreateContext; log: /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxheads: symbol lookup error: /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxheads: undefined symbol: glXCreateContext |
| manywin | 4 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (5): glXChooseVisual glXCreateContext glXDestroyContext glXMakeCurrent glXSwapBuffers; missing: glXDestroyContext; log: /src/gl/ref-apps/build/mesa-demos/src/xdemos/manywin: symbol lookup error: /src/gl/ref-apps/build/mesa-demos/src/xdemos/manywin: undefined symbol: glXDestroyContext |
| manywin | 20 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (5): glXChooseVisual glXCreateContext glXDestroyContext glXMakeCurrent glXSwapBuffers; missing: glXDestroyContext; log: /src/gl/ref-apps/build/mesa-demos/src/xdemos/manywin: symbol lookup error: /src/gl/ref-apps/build/mesa-demos/src/xdemos/manywin: undefined symbol: glXDestroyContext |
| manywin | 60 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (5): glXChooseVisual glXCreateContext glXDestroyContext glXMakeCurrent glXSwapBuffers; missing: glXDestroyContext; log: /src/gl/ref-apps/build/mesa-demos/src/xdemos/manywin: symbol lookup error: /src/gl/ref-apps/build/mesa-demos/src/xdemos/manywin: undefined symbol: glXDestroyContext |
| offset | 1 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (4): glXChooseVisual glXCreateContext glXMakeCurrent glXSwapBuffers; missing: glXCreateContext; log: /src/gl/ref-apps/build/mesa-demos/src/xdemos/offset: symbol lookup error: /src/gl/ref-apps/build/mesa-demos/src/xdemos/offset: undefined symbol: glXCreateContext |
| glxgears_fbconfig | 3 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (15): glXChooseFBConfig glXCreateNewContext glXCreatePbuffer glXDestroyContext glXDestroyPbuffer glXGetFBConfigAttrib glXGetFBConfigs glXGetProcAddress glXGetProcAddressARB glXGetVisualFromFBConfig glXMakeCurrent glXQueryExtensionsString glXQueryServerString glXQueryVersion glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxgears_fbconfig: symbol lookup error: /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxgears_fbconfig: undefined symbol: glXChooseFBConfig |
| glxgears_fbconfig | 20 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (15): glXChooseFBConfig glXCreateNewContext glXCreatePbuffer glXDestroyContext glXDestroyPbuffer glXGetFBConfigAttrib glXGetFBConfigs glXGetProcAddress glXGetProcAddressARB glXGetVisualFromFBConfig glXMakeCurrent glXQueryExtensionsString glXQueryServerString glXQueryVersion glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxgears_fbconfig: symbol lookup error: /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxgears_fbconfig: undefined symbol: glXChooseFBConfig |
| glxgears_fbconfig | 60 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (15): glXChooseFBConfig glXCreateNewContext glXCreatePbuffer glXDestroyContext glXDestroyPbuffer glXGetFBConfigAttrib glXGetFBConfigs glXGetProcAddress glXGetProcAddressARB glXGetVisualFromFBConfig glXMakeCurrent glXQueryExtensionsString glXQueryServerString glXQueryVersion glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxgears_fbconfig: symbol lookup error: /src/gl/ref-apps/build/mesa-demos/src/xdemos/glxgears_fbconfig: undefined symbol: glXChooseFBConfig |
| gears | 3 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/gears: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| gears | 20 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/gears: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| gears | 60 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/gears: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| morph3d | 3 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/morph3d: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| morph3d | 20 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/morph3d: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| morph3d | 60 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/morph3d: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| bounce | 3 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/bounce: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| bounce | 20 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/bounce: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| bounce | 60 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/bounce: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| spectex | 3 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/spectex: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| spectex | 20 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/spectex: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| spectex | 60 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/spectex: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| geartrain | 3 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/geartrain: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| geartrain | 20 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/geartrain: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| geartrain | 60 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/geartrain: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| ipers | 3 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/ipers: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| ipers | 20 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/ipers: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| ipers | 60 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/ipers: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| terrain | 3 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/terrain: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| terrain | 20 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/terrain: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| terrain | 60 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/terrain: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| tunnel | 3 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/tunnel: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| tunnel | 20 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/tunnel: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| tunnel | 60 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/tunnel: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| fire | 3 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/fire: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| fire | 20 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/fire: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| fire | 60 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/fire: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| teapot | 3 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/teapot: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| teapot | 20 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/teapot: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| teapot | 60 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/teapot: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| texcyl | 3 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/texcyl: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| texcyl | 20 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/texcyl: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| texcyl | 60 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/texcyl: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |
| isosurf | 1 | **MISSING-SYMBOL** | - | - | - | - | missing-symbol | ok | all unresolved GL imports (12): glXChooseFBConfig glXCreateNewContext glXDestroyContext glXGetCurrentContext glXGetFBConfigAttrib glXGetProcAddressARB glXGetVisualFromFBConfig glXIsDirect glXMakeContextCurrent glXQueryExtension glXQueryExtensionsString glXSwapBuffers; missing: glXChooseFBConfig; log: /src/gl/ref-apps/build/mesa-demos/src/demos/isosurf: symbol lookup error: /src/gl/ref-apps/prefix/lib/libglut.so.3: undefined symbol: glXChooseFBConfig |

## Images

Mesa reference, ours, diff (grey = reference, red = tolerant-bad, yellow = edge-only). EXACT frames have no diff image.

**glxgears f3: MISSING-SYMBOL**  
![](../ref-mesa/mesa/glxgears.f3.png) - -

**glxgears f20: MISSING-SYMBOL**  
![](../ref-mesa/mesa/glxgears.f20.png) - -

**glxgears f60: MISSING-SYMBOL**  
![](../ref-mesa/mesa/glxgears.f60.png) - -

**glxheads f3: MISSING-SYMBOL**  
![](../ref-mesa/mesa/glxheads.f3.png) - -

**glxheads f20: MISSING-SYMBOL**  
![](../ref-mesa/mesa/glxheads.f20.png) - -

**glxheads f60: MISSING-SYMBOL**  
![](../ref-mesa/mesa/glxheads.f60.png) - -

**manywin f4: MISSING-SYMBOL**  
![](../ref-mesa/mesa/manywin.f4.png) - -

**manywin f20: MISSING-SYMBOL**  
![](../ref-mesa/mesa/manywin.f20.png) - -

**manywin f60: MISSING-SYMBOL**  
![](../ref-mesa/mesa/manywin.f60.png) - -

**offset f1: MISSING-SYMBOL**  
![](../ref-mesa/mesa/offset.f1.png) - -

**glxgears_fbconfig f3: MISSING-SYMBOL**  
![](../ref-mesa/mesa/glxgears_fbconfig.f3.png) - -

**glxgears_fbconfig f20: MISSING-SYMBOL**  
![](../ref-mesa/mesa/glxgears_fbconfig.f20.png) - -

**glxgears_fbconfig f60: MISSING-SYMBOL**  
![](../ref-mesa/mesa/glxgears_fbconfig.f60.png) - -

**gears f3: MISSING-SYMBOL**  
![](../ref-mesa/mesa/gears.f3.png) - -

**gears f20: MISSING-SYMBOL**  
![](../ref-mesa/mesa/gears.f20.png) - -

**gears f60: MISSING-SYMBOL**  
![](../ref-mesa/mesa/gears.f60.png) - -

**morph3d f3: MISSING-SYMBOL**  
![](../ref-mesa/mesa/morph3d.f3.png) - -

**morph3d f20: MISSING-SYMBOL**  
![](../ref-mesa/mesa/morph3d.f20.png) - -

**morph3d f60: MISSING-SYMBOL**  
![](../ref-mesa/mesa/morph3d.f60.png) - -

**bounce f3: MISSING-SYMBOL**  
![](../ref-mesa/mesa/bounce.f3.png) - -

**bounce f20: MISSING-SYMBOL**  
![](../ref-mesa/mesa/bounce.f20.png) - -

**bounce f60: MISSING-SYMBOL**  
![](../ref-mesa/mesa/bounce.f60.png) - -

**spectex f3: MISSING-SYMBOL**  
![](../ref-mesa/mesa/spectex.f3.png) - -

**spectex f20: MISSING-SYMBOL**  
![](../ref-mesa/mesa/spectex.f20.png) - -

**spectex f60: MISSING-SYMBOL**  
![](../ref-mesa/mesa/spectex.f60.png) - -

**geartrain f3: MISSING-SYMBOL**  
![](../ref-mesa/mesa/geartrain.f3.png) - -

**geartrain f20: MISSING-SYMBOL**  
![](../ref-mesa/mesa/geartrain.f20.png) - -

**geartrain f60: MISSING-SYMBOL**  
![](../ref-mesa/mesa/geartrain.f60.png) - -

**ipers f3: MISSING-SYMBOL**  
![](../ref-mesa/mesa/ipers.f3.png) - -

**ipers f20: MISSING-SYMBOL**  
![](../ref-mesa/mesa/ipers.f20.png) - -

**ipers f60: MISSING-SYMBOL**  
![](../ref-mesa/mesa/ipers.f60.png) - -

**terrain f3: MISSING-SYMBOL**  
![](../ref-mesa/mesa/terrain.f3.png) - -

**terrain f20: MISSING-SYMBOL**  
![](../ref-mesa/mesa/terrain.f20.png) - -

**terrain f60: MISSING-SYMBOL**  
![](../ref-mesa/mesa/terrain.f60.png) - -

**tunnel f3: MISSING-SYMBOL**  
![](../ref-mesa/mesa/tunnel.f3.png) - -

**tunnel f20: MISSING-SYMBOL**  
![](../ref-mesa/mesa/tunnel.f20.png) - -

**tunnel f60: MISSING-SYMBOL**  
![](../ref-mesa/mesa/tunnel.f60.png) - -

**fire f3: MISSING-SYMBOL**  
![](../ref-mesa/mesa/fire.f3.png) - -

**fire f20: MISSING-SYMBOL**  
![](../ref-mesa/mesa/fire.f20.png) - -

**fire f60: MISSING-SYMBOL**  
![](../ref-mesa/mesa/fire.f60.png) - -

**teapot f3: MISSING-SYMBOL**  
![](../ref-mesa/mesa/teapot.f3.png) - -

**teapot f20: MISSING-SYMBOL**  
![](../ref-mesa/mesa/teapot.f20.png) - -

**teapot f60: MISSING-SYMBOL**  
![](../ref-mesa/mesa/teapot.f60.png) - -

**texcyl f3: MISSING-SYMBOL**  
![](../ref-mesa/mesa/texcyl.f3.png) - -

**texcyl f20: MISSING-SYMBOL**  
![](../ref-mesa/mesa/texcyl.f20.png) - -

**texcyl f60: MISSING-SYMBOL**  
![](../ref-mesa/mesa/texcyl.f60.png) - -

**isosurf f1: MISSING-SYMBOL**  
![](../ref-mesa/mesa/isosurf.f1.png) - -

