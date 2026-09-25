# The rig's stock Debian mesa-utils glxgears under Mesa and ours, compared at
# three swap numbers with tools/glref (deterministic time). In s31-glref:
#   docker run --rm -v $REPO:/src -w /src s31-glref:latest sh gl/tests/run-sysgears.sh
export GLREF_RUN_DIR=/src/artifacts/gl/phase1/sysgears
dpkg-query -W -f 'mesa-utils ${Version}\n' mesa-utils 2>/dev/null
for f in 3 20 60; do
	for i in mesa ours; do
		sh tools/glref/run.sh $i sysgears $f /usr/bin/glxgears -geometry 320x240+0+0 >/dev/null
	done
	printf 'frame %s: ' $f
	python3 tools/glref/compare.py $GLREF_RUN_DIR/mesa/sysgears.f$f.png \
		$GLREF_RUN_DIR/ours/sysgears.f$f.png --diff $GLREF_RUN_DIR/diff.f$f.png
done
