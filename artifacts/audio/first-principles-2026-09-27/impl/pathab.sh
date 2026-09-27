# Implementation phase, item 3: s31route's own converter vs the old plug: path
# (S31ROUTE_PLUG=1), same plugin, same boot, interleaved, light load.
# Detached; results in /root/afp/pathab-<tag>.txt. usage: sh pathab.sh <tag> <so> <secs> <reps>
TAG=$1; SO=$2; SECS=$3; REPS=$4
cat > /root/afp/pathab-inner-$TAG.sh <<IN
cd /root/afp
echo "== uname \$(uname -v) so $SO"
printf 'pcm_type.s31route { lib "$SO" }\n' > /root/.asoundrc
for rep in \$(seq $REPS); do
 for g in "36000 1 1024" "36000 2 1024" "22050 1 1024" "44100 1 2048" "12000 1 512" "48000 2 2048"; do
  for arm in new plug; do
   E=""; [ \$arm = plug ] && E="S31ROUTE_PLUG=1"
   env HOME=/root S31ROUTE_DEBUG=1 \$E ./sdltone1 \$g $SECS 0 0 >o-$TAG.txt 2>e-$TAG.txt &
   sleep 3; hw=\$(grep -a -E '^(access|channels|rate|period_size)' /proc/asound/card0/pcm0p/sub0/hw_params | tr '\n' ' '); wait
   echo "ARM \$arm geom \$g rep \$rep | xr \$(grep -a -c -i -E 'occurred|underrun' e-$TAG.txt) | \$(grep -a 'path' o-$TAG.txt | sed 's/.*path/path/') | \$(grep -a 'calls' o-$TAG.txt | sed 's/.*calls/calls/' | cut -d' ' -f1-4) | \$hw| \$(grep -a 'conv' e-$TAG.txt | tail -1 | sed 's/.*conv/conv/')"
  done
 done
done
rm -f /root/.asoundrc
echo PATHAB_DONE
IN
setsid sh /root/afp/pathab-inner-$TAG.sh </dev/null >/root/afp/pathab-$TAG.txt 2>&1 &
echo PATHAB_STARTED
