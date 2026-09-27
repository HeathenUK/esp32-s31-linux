# how far the detached runs are (short)
for f in /root/afp2/b1.txt /root/afp2/b4.txt /root/afp2/tyr-*.txt /root/afp2/reg-*.txt; do [ -e $f ] && { echo "--- $f $(wc -l < $f) lines"; tail -2 $f; }; done
