# T4: s31route knobs (prototype plugin via /root/.asoundrc, removed at the end)
# against geometries, 2 spinner threads + a ~3 us/sample mixer load.
# usage: sh arms.sh <tag> <iters> <arm...>   arm = ship | fix | fix,ENV=V,ENV=V
TAG=$1; IT=$2; shift 2
cat > /root/afp/arms-inner-$TAG.sh <<IN
cd /root/afp
echo "== uname \$(uname -v) iters $IT"
for arm in $*; do
  rm -f /root/.asoundrc
  case \$arm in fix*) printf 'pcm_type.s31route { lib "/root/afp/libasound_module_pcm_s31route.so" }\n' > /root/.asoundrc ;; esac
  envs=\$(echo \$arm | cut -s -d, -f2- | tr ',' ' ')
  for g in "44100 2 512" "48000 2 512" "44100 1 2048" "36000 1 512" "22050 1 512"; do
    echo "== ARM \$arm geom \$g"
    b0=\$(cat /sys/module/kernel/parameters/esp32s31_pie_bounces)
    env HOME=/root S31ROUTE_DEBUG=1 \$envs ./sdltone1 \$g 12 $IT 2 2>err-$TAG.txt
    echo "underruns \$(grep -a -c -i -E 'occurred|underrun' err-$TAG.txt) bounces \$((\$(cat /sys/module/kernel/parameters/esp32s31_pie_bounces)-b0))"
    grep -a -E 's31route' err-$TAG.txt | tail -2
    grep -a -v -E 's31route|underrun|^$' err-$TAG.txt | head -2 | sed 's/^/ERR /'
  done
done
rm -f /root/.asoundrc
echo ARMS_DONE
IN
setsid sh /root/afp/arms-inner-$TAG.sh </dev/null >/root/afp/arms-$TAG.txt 2>&1 &
echo ARMS_STARTED
