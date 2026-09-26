#!/bin/sh
# apprand.sh CMD... - run CMD with gl/tests/apprand.c's app-only rand()
# added to the LD_PRELOAD tools/glref/run.sh set ($PRE: the .so). s31, MIT.
LD_PRELOAD="$LD_PRELOAD ${PRE:-/src/gl/out-host/apprand.so}" exec "$@"
