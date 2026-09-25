# glx_prims (gl/tests/glx_prims.c) under Mesa and ours, compared by tools/glref;
# side-by-side (Mesa | ours | diff) in artifacts/gl/phase1/prims/sbs.png. In s31-glref:
#   docker run --rm -v $REPO:/src -w /src s31-glref:latest sh gl/tests/run-prims.sh
export GLREF_RUN_DIR=/src/artifacts/gl/phase1/prims
sh tools/glref/run.sh mesa prims 2 /src/gl/out-host/glx_prims >/dev/null
sh tools/glref/run.sh ours prims 2 /src/gl/out-host/glx_prims >/dev/null
python3 tools/glref/compare.py artifacts/gl/phase1/prims/mesa/prims.f2.png artifacts/gl/phase1/prims/ours/prims.f2.png --diff /src/artifacts/gl/phase1/prims/diff.png
cd artifacts/gl/phase1/prims; convert mesa/prims.f2.png ours/prims.f2.png diff.png -scale 200% +append sbs.png
