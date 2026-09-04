# Bluetooth tray: plan (2026-09-04)

A tray icon and popover for Bluetooth in the shape of the Wi-Fi one: status
at a glance, paired devices one tap from connected, pairing without a
terminal. Scope is what this board can actually carry: **A2DP audio sinks**
(headphones, speakers) and **HID** (keyboards, mice, game controllers).

## What is already true

- Controller in dual mode (BR/EDR + LE) since the June controller blob was
  replaced; `features[4] & 0x20` is honest. Kernel: `BT`, `BT_BREDR`,
  `BT_LE`, `BT_HIDP`. BlueZ 5.79 with the client, audio, hid and hog
  plugins. bluetoothd runs when `/etc/s31-bluetooth-on` exists (S46), `-n`,
  no plugin allow-list (the `-p gap` trap is recorded).
- A2DP source is ours: `rootfs/s31-a2dp.c`, libdbus + libsbc, no glib,
  0.6% of the core idle, coex hint on acquire. It registers a
  `MediaEndpoint1`, finds the transport itself (BlueZ may never call
  `SetConfiguration`), and today plays **a file** it is given. It is not
  started at boot and there is no ALSA route into it.
- One device is paired: the soundcore Liberty 4 NC (E8:26:CF:C1:96:0A).
  Pairing has only ever been done from `bluetoothctl` with the classic
  transport filter (memory: `s31-bluetooth-classic`).
- No `/etc/bluetooth/main.conf` on the card: BlueZ defaults, dual mode.
- lvdesk: single thread, no glib, links libasound only. The Wi-Fi tray talks
  to wpa_supplicant over its control socket, which sits in lvdesk's poll set
  (`wifi_ev_poll`), and renders a popover (`popover_open`, `wifi_render`,
  `wifi_show_results`, `wifi_join`). `LV_SYMBOL_BLUETOOTH` exists in the
  built-in font.

## Compatibility, honestly

| device class | path | state |
|---|---|---|
| A2DP sink (headphones, speakers) | s31-a2dp source over BlueZ Media | **works**, verified by ear and by numbers; needs an ALSA route to be the desktop's output |
| Classic BR/EDR HID (older keyboards/mice, PS4/PS5/Xbox controllers, most "gaming" pads) | BlueZ input plugin -> kernel HIDP -> evdev, picked up by lvdesk's inotify rescan | stack present, **never tried on this board** |
| BLE HID / HID over GATT (most 2020+ mice and keyboards, Apple Magic devices) | BlueZ hog plugin -> **uhid**, which is `CONFIG_UHID=n` today | one config flip plus the LE pairing path, which last failed in `smp.c` before the blob fix and has not been retried since |
| HFP/HSP (headset microphone, calls) | SCO over VHCI | unproven, out of scope |
| AVRCP (play/pause/volume from the headphones) | BlueZ avrcp plugin, needs a MediaPlayer1 | optional, later |

So "headphones/speakers and HID": yes for audio and classic HID, and BLE HID
is a kernel option away with one known risk to retire.

## Architecture: one Bluetooth daemon, a line protocol, lvdesk stays small

Do not put libdbus into lvdesk. `Pair` and `Connect` block for seconds;
lvdesk is single-threaded and the compositor would stall. Instead grow
`s31-a2dp` into **`s31-bt`**, the one process that owns the D-Bus
connection to bluetoothd:

- the A2DP source it already is (endpoint, transport, SBC, coex hint);
- the **pairing agent** (`org.bluez.Agent1`, capability `DisplayYesNo`):
  `RequestConfirmation` for headphones and mice (just-works), `DisplayPasskey`
  for keyboards (the user types the six digits on the keyboard being
  paired), `RequestAuthorization` for reconnects of trusted devices;
- **device management** over `org.bluez.Adapter1` / `Device1` with an
  `ObjectManager` cache kept current from `PropertiesChanged`;
- a **control socket** for lvdesk, in the wpa_supplicant shape lvdesk already
  speaks: `/var/run/s31-bt.ctl`, one command per line, events pushed on the
  same connection so it joins lvdesk's poll set like the wpa socket does.

Protocol (text, one line each):

    -> list                     <- DEV <addr> paired=1 conn=0 trusted=1 class=audio "Name"  (per device)
    -> scan on|off              <- SCAN on|off ; then DEV lines as they appear (rssi=)
    -> pair <addr>              <- PAIRING <addr> ; PASSKEY <addr> 123456 | CONFIRM <addr> 123456 ; PAIRED <addr> | FAIL <addr> <reason>
    -> confirm <addr> yes|no
    -> connect <addr> | disconnect <addr> | forget <addr>
    -> power on|off
    <- STATE powered=1 discoverable=0 connected=<addr>|-   (on change)

Filtering: the scan list shows only devices whose class or UUIDs say
audio sink or HID; everything else is noise on a 800x480 panel. Names come
from `Name` or `Alias`; nameless BLE advertisers are dropped.

Why this and not `bluetoothctl` under the hood: every action would be a
fork+exec of a 1.2 MB client plus parsing its interactive output, and the
pairing prompts are exactly the part `bluetoothctl` handles in its own
readline loop. The daemon costs one resident process (~100-150 KB) only
while Bluetooth is on, the same gate as bluetoothd.

## The tray and popover (lvdesk)

- Icon: `LV_SYMBOL_BLUETOOTH`, dim when powered off or no daemon, normal when
  on, bright when a device is connected, with the Wi-Fi tray's "lit
  fraction" trick unnecessary here. Click toggles the popover.
- Popover (same `popover_open` anchoring, same list theme as Wi-Fi):
  - status line: "Bluetooth off" / "On, nothing connected" / "Connected: soundcore Liberty 4 NC";
  - **Paired** section: one row per known device, name plus a small
    class glyph (audio / keyboard / mouse / controller); tap connects or
    disconnects; a long press (or the existing context-menu route) offers
    Forget;
  - **Scan** button; while scanning, a **Found** section fills in, tap
    pairs; the daemon's `PASSKEY` shows the six digits in the popover
    ("Type 123456 on the keyboard"); `CONFIRM` shows Yes/No;
  - power toggle at the bottom (touches `/etc/s31-bluetooth-on` and starts
    or stops S46 + the daemon, so it survives reboot).
- Nothing blocks: every button sends one line and returns; state changes
  arrive as events and re-render, exactly as the Wi-Fi popover does with
  `CTRL-EVENT-*`.

## Audio route for headphones as the desktop's output

Today the only thing that reaches the earbuds is a file the daemon plays.
For apps to play through them:

- `CONFIG_SND_ALOOP=y` (snd-aloop, a few KB): apps write to the loopback's
  playback side; `s31-bt` opens the capture side as its PCM source and
  streams it as A2DP.
- The volume popover gains an output selector: **Speakers** / **<paired
  device>**. Selecting a device rewrites `pcm.!default` in `/etc/asound.conf`
  to the loopback and asks the daemon to `connect` + stream; back to
  speakers reverses it. Apps pick the route up on their next open (the
  ALSA reality; no mixing daemon here by design).
- Idle cost: the loopback carries silence only while selected; the daemon
  stops the transport and drops the coex hint when nothing is playing.

## Kernel and image changes

- `CONFIG_UHID=y` for BLE HID (tiny). `CONFIG_SND_ALOOP=y` for the route.
- `s31-bt` replaces `s31-a2dp` in XIP_ROOTS (same partition, ~+40 KB).
- S46 starts `s31-bt` after bluetoothd; the `power on` quirk after a manual
  bluetoothd restart goes into the daemon's start-up (`Powered=true`).

## Things the record says to expect

- Paging and discovery are where coex bites: connects can take a while with
  Wi-Fi busy; the daemon should not surface that as an error before ~20 s.
- A2DP under Wi-Fi is solved by the streaming coex bit; keep it in the daemon.
- A hosted-transport reset wedges hci0's MGMT adoption; reboot, do not
  restart bluetoothd. The tray should say "adapter lost - reboot" rather than
  retry.
- Never rebind or restart bluetoothd from the UI as a "fix".

## Order of work

1. `s31-bt`: agent + device cache + control socket, around the existing
   A2DP code (~700 lines of C; libdbus already in XIP). Verify with a
   socket client from the console: list, scan, pair the earbuds fresh.
2. lvdesk popover + tray (~400 lines, mirroring the Wi-Fi code). Verify
   pairing the earbuds from the panel with a finger and no console.
3. Classic HID: pair whatever BR/EDR keyboard or controller is to hand;
   confirm evdev nodes appear and lvdesk picks them up.
4. `CONFIG_UHID`, BLE HID: pair a BLE mouse; retire or record the `smp.c`
   "security requested but not available" failure.
5. Audio route: aloop + output selector; music from the terminal through
   the earbuds; measure the cost against today's ~30% file playback.

Effort: 1-2 is a day; 3-4 depends on what devices exist to test with; 5 is
another half day plus the measurements.
