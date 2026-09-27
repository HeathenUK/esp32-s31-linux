L=$1
zr() { [ -r /sys/block/zram0/mm_stat ] && echo "ZRAM mm_stat $(cat /sys/block/zram0/mm_stat) | io_stat $(cat /sys/block/zram0/io_stat 2>/dev/null)"; }
lite() { echo "== t=$1"; grep -E 'MemFree|MemAvailable|^Cached|SwapCached|Active|Inactive|AnonPages|Mapped|Shmem:|Slab|SUnreclaim|KernelStack|PageTables|SwapFree|CmaFree' /proc/meminfo | tr -s ' ' | tr '\n' ' '; echo; zr; tail -n +2 /proc/swaps; awk '/^(workingset_refault_anon|workingset_refault_file|pgsteal_kswapd|pgsteal_direct|pgscan_kswapd|pgscan_direct|allocstall_normal|pswpin|pswpout|pgmajfault|nr_file_pages|nr_anon_pages|nr_mapped|nr_shmem|nr_free_pages) /{printf "%s=%s ",$1,$2}' /proc/vmstat; echo; }
if [ -z "$CENSUS" ]; then sleep 120; lite 120; exit 0; fi
sleep 75
lite 75
echo "== procs (kB): name pid rss anon file shmem swap"
awk 'FNR==1{if(n!="")print n,p,r,a,f,s,w; n="";p="";r=0;a=0;f=0;s=0;w=0} /^Name:/{n=$2} /^Pid:/{p=$2} /^VmRSS/{r=$2} /^RssAnon/{a=$2} /^RssFile/{f=$2} /^RssShmem/{s=$2} /^VmSwap/{w=$2} END{print n,p,r,a,f,s,w}' /proc/[0-9]*/status | awk '$3>0||$7>0'
P=$(pidof quakespasm)
echo "== quakespasm $P smaps (Size Rss Pss ShCl ShDi PrCl PrDi Swap perm name), rss+swap>=16"
awk '/^[0-9a-f]+-[0-9a-f]+ /{if(h!="" && r+w>=16)print sz,r,ps,sc,sd,pc,pd,w,pm,nm; h=$1; pm=$2; nm=$6; sz=r=ps=sc=sd=pc=pd=w=0} /^Size:/{sz=$2} /^Rss:/{r=$2} /^Pss:/{ps=$2} /^Shared_Clean:/{sc=$2} /^Shared_Dirty:/{sd=$2} /^Private_Clean:/{pc=$2} /^Private_Dirty:/{pd=$2} /^Swap:/{w=$2} END{if(r+w>=16)print sz,r,ps,sc,sd,pc,pd,w,pm,nm}' /proc/$P/smaps
LV=$(pidof lvdesk.new lvdesk)
echo "== lvdesk $LV smaps rss+swap>=32"
awk '/^[0-9a-f]+-[0-9a-f]+ /{if(h!="" && r+w>=32)print sz,r,w,pm,nm; h=$1; pm=$2; nm=$6; sz=r=w=0} /^Size:/{sz=$2} /^Rss:/{r=$2} /^Swap:/{w=$2} END{if(r+w>=32)print sz,r,w,pm,nm}' /proc/$LV/smaps
echo "== meminfo"; cat /proc/meminfo
echo "== sysvipc shm"; cat /proc/sysvipc/shm 2>/dev/null
sleep 45
lite 120
