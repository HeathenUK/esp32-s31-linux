# Background contention: fetch a large file over Wi-Fi in a loop, to nowhere.
# The host must be serving it (python3 -m http.server in a dir with big.bin).
URL=${TD_LOAD_URL:-http://192.168.1.10:8000/big.bin}
setsid sh -c "while :; do wget -q -O /dev/null $URL || sleep 1; done" </dev/null >/dev/null 2>&1 &
echo "wifi_load=$URL pid=$!"
