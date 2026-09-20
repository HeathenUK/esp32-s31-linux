f=/sys/devices/virtual/workqueue/cpumask
[ -w $f ] && echo 2 > $f
echo "wq_cpumask=$(cat $f 2>/dev/null)"
