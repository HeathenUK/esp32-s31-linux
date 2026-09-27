# T3: the path under an SDL client of negligible cost, per rate/channels/period,
# with N CPU-bound threads beside it (a game + a desktop). Detached; results in
# /root/afp/tone-<tag>.txt. Each arm 12 s. Volume untouched (DAC 143).
# usage: sh tone.sh <tag> <sdltone1|sdltone2> <hog> <iters> <samples...>
TAG=$1; BIN=$2; HOG=$3; IT=$4; shift 4
cat > /root/afp/tone-inner-$TAG.sh <<IN
cd /root/afp
echo "== uname \$(uname -v)"
for n in $*; do for ch in 1 2; do for r in 22050 36000 44100 48000; do
  echo "== ARM $BIN rate \$r ch \$ch samples \$n hog $HOG iters $IT"
  S31ROUTE_DEBUG=1 ./$BIN \$r \$ch \$n 12 $IT $HOG 2>err-$TAG.txt &
  sleep 6
  echo "hw \$(tr '\n' ' ' < /proc/asound/card0/pcm0p/sub0/hw_params)"
  wait
  echo "underruns \$(grep -a -c -i -E 'occurred|underrun' err-$TAG.txt)"
  grep -a s31route err-$TAG.txt | tail -1
  grep -a -v s31route err-$TAG.txt | head -2 | sed 's/^/ERR /'
done; done; done
echo TONE_DONE
IN
setsid sh /root/afp/tone-inner-$TAG.sh </dev/null >/root/afp/tone-$TAG.txt 2>&1 &
echo TONE_STARTED
