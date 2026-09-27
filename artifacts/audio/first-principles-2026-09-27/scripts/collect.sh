# print a results file if its end marker is there; otherwise say how far it got
F=$1; M=$2
if grep -q "$M" "$F" 2>/dev/null; then cat "$F"; echo COLLECT_OK; else echo "NOT_DONE lines=$(wc -l < $F 2>/dev/null)"; tail -2 "$F" 2>/dev/null; fi
