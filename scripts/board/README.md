# Talking to the board

These exist because they have each been re-written from scratch several times,
badly, in the middle of doing something else. Use them.

    scripts/board/runsh.py  <script.sh> [timeout] [boot_wait]
    scripts/board/deploy_bin.py <file.b64> <dest-on-board>
    scripts/board/reset.py
    scripts/board/screenshot.py <out.png>

`runsh.py` ships a shell script to the board as a file and runs it. It does NOT
flatten the script into a one-liner - `for x; do` becomes `do;` that way and the
shell errors out, producing empty results that look like a hardware fault.

It also survives the login race: the LCD driver prints its mode-set messages at
exactly the moment getty shows its prompt, so a matcher that expects `login:`
last reports NO_SHELL on a perfectly healthy board. If you get NO_SHELL, retry
before concluding anything.

`screenshot.py` reads the live scanout buffer over the serial console and writes
a PNG. The scanout address is **allocated, not fixed** - it takes it from dmesg
rather than hardcoding it.
