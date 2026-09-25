# lvdesk-ctxmenu-test.sh - QoL B2's ship blocker: a context menu must never
# run a row the user did not select. Run ON the board against the RUNNING
# desktop (the kbdmenu test leaves /root/lvdesk.new up). The menu is the
# shape of xfiles' delete confirm: row 0 is the destructive one.
#  X1 Backspace, then Enter, with nothing selected -> reply is empty (dismissed)
#  X2 held Enter (repeats) with nothing selected    -> reply is empty
#  X3 Down then Enter                               -> reply is row 0
#  X4 Esc                                           -> reply is empty
# Prints one RESULT line. ~40 s.
U=/root/uinject; F=/tmp/ctxtest.fifo
ask() {	# ask <keys...>: open the menu, press keys, read one reply line
	rm -f $F; mkfifo $F; exec 3<>$F
	echo "menu $F Confirm delete (3)|Cancel" > /tmp/lvdesk.ctl; sleep 1
	for c in "$@"; do case $c in H) $U hold 28 1200 >/dev/null 2>&1;; *) $U key $c >/dev/null 2>&1;; esac; done
	sleep 1; echo pop > /tmp/lvdesk.ctl; sleep 1
	# no `timeout` on this board: a bounded background read instead. The
	# menu writes its reply (a label, or an empty line on dismissal) once.
	dd bs=128 count=1 <&3 >/tmp/ctxtest.out 2>/dev/null & rp=$!
	sleep 2; kill $rp 2>/dev/null; exec 3<&-; rm -f $F
	r=$(head -n 1 /tmp/ctxtest.out 2>/dev/null); rm -f /tmp/ctxtest.out
	printf '%s' "$r"
}
x1=$(ask 14 28); x1s=$(grep -a "lvdesk: pop" /var/log/lvdesk.log | tail -1 | awk '{print $3}')
[ "$x1s" = open ] && $U key 1 >/dev/null 2>&1 && sleep 1
x2=$(ask H); x2s=$(grep -a "lvdesk: pop" /var/log/lvdesk.log | tail -1 | awk '{print $3}')
[ "$x2s" = open ] && $U key 1 >/dev/null 2>&1 && sleep 1
x3=$(ask 108 28)
x4=$(ask 1)
echo "X1 backspace+enter: '$x1' (menu $x1s)"
echo "X2 held enter: '$x2' (menu $x2s)"
echo "X3 down+enter: '$x3'"
echo "X4 esc: '$x4'"
ok=1; [ -z "$x1$x2$x4" ] || ok=0; [ "$x3" = "Confirm delete (3)" ] || ok=0
echo "RESULT ok=$ok"
