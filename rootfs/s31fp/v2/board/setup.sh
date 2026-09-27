# unpack the test bundle (deployed as /root/afp2.tgz); nothing outside /root/afp2
mkdir -p /root/afp2 && cd /root/afp2 && gzip -dc /root/afp2.tgz | tar xf - && rm /root/afp2.tgz
chmod +x oplbench-* v2check-board v2bench-board scanbench oncpu sdltone1
md5sum * | head -20; df -h /root | tail -1
