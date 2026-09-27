set -e
. /home/user/esp32-s31-linux/tools/cloud/env.sh
cd /home/user/esp32-s31-linux/build/cloud/probes/safety/order
CC="s31-cc -fPIC"
for L in B C E F; do $CC -shared -o lib$L.so lib$L.c; done
$CC -shared -o libA.so libA.c -L. -lB
$CC -shared -o libD.so libD.c -L. -lE -lF
$CC -shared -o libpre.so pre.c
$CC -o app app.c -L. -lA -lC -Wl,-rpath,.
$CC -o app2 app.c -L. -lC -lA -Wl,-rpath,.
