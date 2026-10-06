# `wlc_sniffer` — ESP32-C5 BLE sensor firmware

The firmware every ESP32-C5 board in WLAN Commander runs. It sniffs 2.4/5 GHz frames — management
and control by default — wraps each in a synthesized radiotap header, and streams them out as BLE
GATT notifications. `core/src/wifi/c5_sensor.rs` is the other end.

**The board boots idle.** It advertises over BLE and does nothing else: no promiscuous mode, no
channel hopping, no transmission. `0x04 SET_RUN 1` starts it. This is v2 of the contract and it
replaces a v1 that captured and transmitted GAS requests from the instant it had power, whether or
not anything was connected — the change exists so a C5 behaves like every other capture device in
this project, told what to do and started and stopped explicitly.

> **An old app against new firmware sees nothing at all, and it looks exactly like a dead board.**
> The board advertises, connects, discovers, accepts the subscribe, and then streams zero frames,
> because nothing sent `0x04`. Its status line is the tell: it is still notifying, and it says
> `run=0`. A board that is genuinely dead notifies nothing. Both apps at time of writing send only
> `0x01`/`0x02`/`0x03`, so every one of them is an old app until it learns `0x04`.

## Why this directory exists

**This was very nearly lost.** It was written in a session scratchpad under `/private/tmp`,
which was reaped. Until 2026-08-26 the only copy on Earth outside the boards themselves was an
Arduino build cache at

    ~/Library/Caches/arduino/sketches/FA318DC4D542308C32A4F96BF8E8EE1A/

which `arduino-cli` deletes whenever it feels like it. `wlc_sniffer.ino` here is
`sketch/wlc_sniffer.ino.cpp.merged` from that cache with its single leading `#line` directive
removed — that file is otherwise the sketch verbatim, so this is the original source, not a
decompilation. Recovered source SHA-256 begins `cef6d4a169347387`; built 2026-08-23 16:26.

The cost of that gap was not hypothetical. `Architecture-Documentation/35-esp32-c5-ble-sensor.md`
had to be written by black-box probing a live board, concluded the control characteristic's
command set was unknowable without the firmware, and recorded that as a property of the device:
*"an unknown command set on a device that can reconfigure itself is not worth a blind poke."*
Reasonable, and wrong — the command set had been designed and hardware-proven two days earlier.
The app then modelled the board as un-steerable for two days on the strength of it.

## The LED says whether a board is alive — and now, whether it was started

A board pulses dim once a second: **green while capturing, blue while idle.** It flashes brighter
whenever an ANQP answer addressed to **this** board arrives. Three things are then readable across
a bench with no serial cable: which boards are alive, which have been told to run, and which are
getting replies.

The second colour arrived with `0x04`. Under v1 every powered board captured, so green meant
"working"; boards now boot idle, and one colour would have left an untasked fleet looking busy.

It is here because of a specific failure mode. A board with a half-written flash is silent on
serial, silent on BLE, and indistinguishable from one that is not plugged in — and port numbers do
not reliably identify boards (above). Making every working board announce itself turns "which one
is broken?" into "which one is dark?", which is answerable by looking, and that is how the board
to hold BOOT on was found.

Guarded on `RGB_BUILTIN`; a board without an addressable LED skips it. The write happens in
`loop()`, never in the promiscuous callback, which runs in IRAM.

## The BLE contract

Service `57574c43-0001-4a50-8000-0000c5000001`

| Characteristic | Properties | Carries |
|---|---|---|
| `…0002` | NOTIFY | Frames: `[u16 LE tag+length][payload]`, several records per notification |
| `…0003` | NOTIFY | Text status, see below |
| `…0004` | WRITE, WRITE_NR | Commands, see below |
| `…0005` | NOTIFY | 802.11mc FTM result (24-byte version 1 record) |

**Subscribing starts nothing.** It only gates forwarding — a subscribed board with `run=0` streams
no frames, and an unsubscribed board with `run=1` captures and discards. Both must hold. Starting
is `0x04 SET_RUN 1`, and "connected but idle" is the state a board boots into.

This claim used to read *"It streams on subscribe. Subscribing to `…0002` is the start command;
there is no start opcode and no 'connected but idle' state."* It was true of v1 and is now the
opposite of true.

### Frame records, and why one frame is not always one record

The top two bits of each record's length word are a **type**; the low fourteen are the byte
count of that record's payload.

| Tag | Meaning |
|---|---|
| `0x0000` | A whole frame. Every frame that fits in one notification, and the only shape the pre-2026-08-29 firmware emits. |
| `0x4000` | First fragment. Its payload opens with a `u16 LE` **total frame length**. |
| `0x8000` | Middle fragment. |
| `0xc000` | Last fragment. |

A GAS response runs to hostapd's 1400-byte `gas_frag_limit` while a BLE notification carries a
few hundred bytes, so a frame that large is split across records and core reassembles it in
`append_pcap_records`. **A record is never split across notifications**, so a notification still
begins and ends on a record boundary — the property `endsOnFrameBoundary` checks on Android.

Two properties worth keeping:

- **Fragments own whole notifications.** Nothing else shares a notification with a fragment, so
  an older core reads the flagged length as absurd, stops that batch, and loses exactly the
  oversized frames it was already losing — no negotiation, no capability bit.
- **The first fragment declares the total.** Without it a lost middle fragment would concatenate
  into a shorter frame that still parses. That is the failure this encoding exists to prevent,
  and `a_lost_middle_fragment_is_counted_not_silently_shortened` is the test that holds it.

A **two-bit type, not `MORE`/`FIRST` flags.** With flags a *last* fragment carries neither and is
indistinguishable from a whole frame; that was the first encoding tried and the reassembly test
rejected it.

### The notification budget is the negotiated MTU, not a constant

`MAX_NOTIFY` used to be a hardcoded 500 while `setMTU(517)` set only a *preference* — the
negotiated value is the minimum of both peers, and NimBLE's `notify()` silently sends
`min(len, MTU-3)` and returns nothing useful. Against any peer that negotiated below 503 every
batch was chopped mid-record. The budget now comes from `getPeerMTU()` each drain pass and the
board prints it as `mtu=`.

### Commands, written to `…0004`

| Opcode | Payload | Effect |
|---|---|---|
| `0x01` SET_CHANNELS | `[0x01][ch][ch]…` | Up to 40 channels. A byte is kept iff `1..=14` or `36..=177` and **silently dropped otherwise**. Applied only if at least one survives — an all-invalid write is a no-op and leaves the previous set running. Adopted in `loop()` (`pendCh`→`activeCh`, `chIdx = 0`), then hopped at `HOP_MS = 300` per channel **while running**. Accepted at any time; a board that is stopped adopts the list and hops it on the next `0x04`. |
| `0x02` ADD_POKE | `[0x02]` then repeating `[6-byte BSSID][1 byte channel]` | Adds ANQP poke targets, deduplicated, `PP_MAX = 24`. |
| `0x03` SET_POKE_ENABLE | `[0x03][0 or 1]` | **Defaults to 0.** Arms ANQP poking; poking additionally requires `run=1`, so this alone transmits nothing. Deliberately *not* cleared by a stop — it is a standing request that survives a stop/start. |
| `0x04` SET_RUN | `[0x04][0 or 1]` | **Defaults to 0.** `1` applies the filter, enables promiscuous, lands on `activeCh[0]` at the current width and begins hopping. `0` disables promiscuous, stops hopping and stops poking. |
| `0x05` SET_FILTER | `[0x05][u32 little-endian mask]` | Passed to `esp_wifi_set_promiscuous_filter`. Defaults to `MGMT\|CTRL` = `0x03`. Re-applied to the driver immediately if the board is already running, otherwise at the next start. Bits below. |
| `0x06` SET_WIDTH | `[0x06][0, 1 or 2]` | `0` = HT20, `1` = HT40 with the secondary channel **above**, `2` = HT40 **below**. Defaults to `0`. Applied on every `esp_wifi_set_channel`, and immediately on change while running. Any other value is a no-op. |
| `0x07` FTM_START | `[0x07][u16 request ID LE][six BSSID bytes][primary channel]` | Starts one 802.11mc initiator session while capture is stopped. A busy board returns status 128. |
| `0x08` FTM_CANCEL | `[0x08][u16 request ID LE]` | Cancels the matching active session. |

The FTM result notification carries version, status, request ID, BSSID, channel, frame
count, raw RTT in ns, estimated RTT in ns, and distance in cm. Status `0` means the
driver completed a session; consumers require a nonzero count and estimated RTT before
using its distance. Status `128` means busy, `129` invalid channel, `130` timeout,
`131` cancelled, and `132` start error. See `tools/c5-ftm/README.md` for the complete
field trial procedure and Linux client. This is 802.11mc, not 802.11az.

Short writes are ignored: `0x04`/`0x06` need 2 bytes, `0x05` needs 5,
`0x07` needs 10, and `0x08` needs 3. The width values map 1:1 onto
`wifi_second_chan_t` (`NONE`/`ABOVE`/`BELOW` = 0/1/2), which is not a coincidence — it is why the
opcode needs no translation table.

**20/40 MHz is the chip's hard ceiling.** There is no 80 and no 160 on a C5, at any opcode. If a
width is asked for that the current channel cannot form — HT40-below on channel 1, say —
`esp_wifi_set_channel` refuses, the board stays where it was and `chfail=` counts it. The channel
is skipped rather than silently visited at the wrong width.

Filter mask bits, from `esp_wifi_types_generic.h`:

| Bit | Value | Packets |
|---|---|---|
| `MGMT` | `1` | management |
| `CTRL` | `1<<1` | control |
| `DATA` | `1<<2` | data |
| `MISC` | `1<<3` | misc |
| `DATA_MPDU` | `1<<4` | data MPDU |
| `DATA_AMPDU` | `1<<5` | data AMPDU |
| `FCSFAIL` | `1<<6` | FCS failures — "do not open it in general", says the header |

Widening past `MGMT|CTRL` is offered, not recommended: a 1x1 C5 forwarding data frames over a
BLE link will find the ring's floor quickly, and `rdrop` is where that shows up.

None of the three new opcodes calls the wifi driver from the BLE write callback. Like `0x01`, they
stage a value that `loop()` reconciles, which keeps the start order — filter, then channel and
width, then promiscuous — in one place and off the NimBLE host task.

Until first written, channels are `DEFAULT_CH = {1,6,11,36,40,44,48,149,153,157,161,165}` — which
is why four unmanaged boards would duplicate each other's coverage instead of dividing it. Under
v2 an unmanaged board hops nowhere at all, because it is not running.

### Status, notified on `…0003`

One line of text, roughly every second:

    run=… poke=… filt=… width=… cap=… fwd=… evt=… gasResp=… gasMine=… pp=… poked=… ddrop=…
    rdrop=… odrop=… bdrop=… nfail=… frag=… slotmax=… chfail=… heap=… mtu=… 2.4=… 5G=… ch=… sub=…

(one line on the wire; wrapped here). `ch=` is the board's current channel — the only direct
read-back of where it is listening — and `run=` is the only read-back of whether it is listening
at all.

The four control fields lead the line on purpose. `snprintf` truncates from the tail in silence,
so the fields a human reads first sit where truncation cannot reach them. The buffer is 448 bytes
against a measured worst case of 363 (every `uint32` counter at 4294967295); a realistic line is
about 225. **The pre-`0x04` line measured 307 against a 256-byte buffer**, so it was already 51
bytes short of its own worst case before any field was added — it never truncated only because no
counter ever got near saturation.

**Read the fields by key, never by position.** Two images are in the field and their field lists
differ; both apps already do this.

Every discard has its own name, which it did not before. That is the point of them: each of
these was once an anonymous `continue` that ran *after* `fwd` had counted the frame delivered,
so the board reported a healthy stream while throwing ANQP away.

| Field | Means |
|---|---|
| `rdrop` | ring full — the phone is not draining fast enough |
| `odrop` | frame larger than `MAX_FRAME` (1568), refused at capture |
| `bdrop` | refused at the BLE stage; only reachable on a degenerate MTU |
| `ddrop` | suppressed by the beacon or GAS dedup |
| `nfail` | `notify()` returned false — link full. The frames are **kept** and retried |
| `frag` | frames sent as a run of fragments (i.e. bigger than one notification) |
| `slotmax` | ring high-water, out of `SLOTS`. Near `SLOTS` means the ring is too shallow |
| `mtu` | the **negotiated** ATT MTU, which decides the notification budget |
| `gasMine` | GAS responses addressed to this board's own MAC — see the caution below |
| `run` | promiscuous is on, hopping, and free to poke. `0` is a board nobody has started |
| `poke` | ANQP poking is armed. It still needs `run=1` to transmit, so `poke=1 run=0` is silent |
| `filt` | the commanded promiscuous filter mask, in **hex**. `3` is the `MGMT\|CTRL` default |
| `width` | `0` HT20, `1` HT40 above, `2` HT40 below. There is no wider value on a C5 |
| `chfail` | `esp_wifi_set_channel` refused — usually an HT40 pairing the channel cannot form |

`run` and `chfail` are the two to read when a board looks dead. `run=0` means nobody started it;
`run=1` with `chfail` climbing and `ch=` stuck means the width is unusable on part of the list.

`filt` and `width` report the **commanded** value rather than the driver's, so a write made while
stopped reads back on the very next line instead of after the next start. `run` reports the driver.

Useful triples:

- `odrop` should sit at ~0. A climbing `odrop` means frames are being refused before the ring.
- `frag` non-zero is the proof that large ANQP answers are now getting through at all.
- `rdrop` climbing while `cap` is steady, with `slotmax` pegged, means 36 slots is too few.

**It is gated on `subscribed`, which is set by `…0002`'s subscribe callback.** Subscribing to
`…0003` alone yields nothing, which is exactly what the 2026-08-24 probe saw and recorded as
"purpose not yet established". Subscribe to the frame stream first.

**It is *not* gated on `run`**, deliberately. An idle board still reports, which is the only thing
that distinguishes one nobody started from one that is broken — and it is what makes `run=0` a
readable diagnosis rather than a silence.

## Two design constraints that are not obvious

- **One board is enough — this said the opposite until 2026-08-27.** The old claim was that a
  transmitting C5 cannot hear the reply to its own transmission, proven with a two-board wiretap
  on ch 11 where the prober caught zero of 15+ answers. That measurement was real and the
  conclusion drawn from it was too broad: every request in it went out through
  `esp_wifi_80211_tx`, raw injection straight past the MAC. `esp_wifi_action_tx_req` hands the
  frame to the MAC instead, which sends it from the real interface address, ACKs the reply and
  holds the channel — and the response reaches the promiscuous callback like anything else.
  Measured 2026-08-27: 58 responses from 4 APs on one board, every one addressed to it, no
  listener. The deafness was a property of raw injection, never of the radio.
- **`WiFi.setBandMode(WIFI_BAND_MODE_AUTO)` must precede `esp_wifi_set_channel`** on this
  dual-band part, or the channel lands in the wrong band and the board hears beacons but none of
  the ones you asked for. Already done in `setup()`; do not remove it.

## Building and flashing

Verified working 2026-08-27 — these are the exact commands, not a reconstruction. The toolchain
now lives in `arduino-cli`'s default location (`~/Library/Arduino15`), deliberately: the previous
one lived in a session scratchpad under `/private/tmp` and was reaped along with the only copy of
this source.

```bash
export ARDUINO_BOARD_MANAGER_ADDITIONAL_URLS=https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core install esp32:esp32@3.3.11      # ~800 MB
arduino-cli lib install "NimBLE-Arduino"          # 2.5.1

FQBN="esp32:esp32:esp32c5:PartitionScheme=min_spiffs,CDCOnBoot=cdc,DebugLevel=none"
arduino-cli compile --fqbn "$FQBN" firmware/wlc_sniffer
arduino-cli upload  --fqbn "$FQBN" --port /dev/cu.usbmodem11401 firmware/wlc_sniffer
```

Arduino CLI, ESP32 core 3.3.11, NimBLE-Arduino. FQBN from `build/build.options.json`:

    esp32:esp32:esp32c5:PartitionScheme=min_spiffs,CDCOnBoot=cdc,DebugLevel=none

`build/` holds the exact image the boards were flashed with on 2026-08-23, so a board can be
restored without reproducing the toolchain. `build/flash_args` carries the offsets:

    esptool.py --chip esp32c5 write_flash \
      0x2000 wlc_sniffer.ino.bootloader.bin \
      0x8000 wlc_sniffer.ino.partitions.bin \
      0xe000 boot_app0.bin \
      0x10000 wlc_sniffer.ino.bin

### Identify a board by its USB serial number, never by its port

Port numbers say where a board is plugged in, not which board it is, and an earlier revision of
this file drew exactly the wrong conclusion from that. A flash failed on `usbmodem11101`, a port
called `usbmodem143201` appeared, and it was recorded here as the same board re-enumerating in
download mode. It was not. It was a **different board** — a Seeed unit — plugged in minutes
earlier, and it was flashed by accident.

The USB serial number settles it in one command:

```bash
ioreg -r -c IOUSBHostDevice -l -w0 | grep -E '"USB Serial Number"|"IOCalloutDevice"'
```

Here: `usbmodem143201` → `10:BD:A3:CA:1F:90` (Seeed), `usbmodem11101` → `D0:CF:13:EB:53:70`
(Espressif). Two devices, plus a dead port node lingering for a board that had stopped booting.

### The board with the wrong crystal

One unit — `D0:CF:13:EB:53:70`, the one the 2026-08-22 session logged as "the board that wouldn't
flash (111xx)" — fails with `Failed to write to target RAM (0107: Checksum error)` or
`Failed to start stub flasher`, and its ROM output arrives with every character tripled.

`esptool` names the cause:

```
Warning: Detected crystal freq 23.96 MHz is quite different to normalized freq 26 MHz.
```

`esptool` reports its crystal as ~24 MHz where the others read 26. That warning is real and
measured; how much of the rest it explains is less clear than it first appeared, because once the
board was healthy its ROM output read cleanly at 115200 again. Treat the crystal as *a* fact about
this board, not as the explanation for everything it does.

**What actually fixed it: split the image and write it in pieces.**

Short sessions always worked on this board — `chip-id` returned its MAC on the first try, every
time, at either baud. Long ones never did. A single 1.39 MB app write died at 41.8%, then at other
points, at 115200 and at 57600, with the stub and without it. The failure scales with transfer
length, so the fix is to stop doing long transfers:

```bash
# split the app image into 128 KB pieces
python3 -c "
d=open('build/wlc_sniffer.ino.bin','rb').read(); C=128*1024
[open(f'/tmp/part_{i:02d}.bin','wb').write(d[o:o+C]) for i,o in enumerate(range(0,len(d),C))]"

# write each at 0x10000 + n*0x20000, retrying per piece
```

Eleven pieces, each its own short session, retried up to three times: **11/11 written**. Three of
them needed a second or third attempt — exactly the ones a single long write would have died on.

The bootloader, partition table and `boot_app0` are small enough to write normally.

**A board sitting in download mode looks identical to a dead one.** After the write it stayed
silent, and the reason was in its own boot banner:

```
rst:0x15 (USB_UART_HPSYS), boot:0x8 (DOWNLOAD(UART0/USB))
waiting for download
```

`boot:0x8` and `waiting for download` mean the BOOT strap is still held — release it and reset,
and the board comes up running. Read the banner before concluding anything about a silent board.

**After any reflash a board is idle again**, and its channel list is `DEFAULT_CH` again until the
app assigns one. On v1 that meant a freshly-flashed fleet duplicated coverage until something
connected; on v2 it means a freshly-flashed fleet does nothing until something connects. Both are
the firmware default doing its job, not the assignment failing.

**What is flashed on a given board is not proven by anything here** — and measurably is not the
same across boards. Read over USB on 2026-08-26, four boards attached:

| Port | Status fields | Build |
|---|---|---|
| `cu.usbmodem11101` | `cap fwd evt ddrop rdrop 2.4 5G ch sub` | plain sniffer — no ANQP |
| `cu.usbmodem11201` | …plus `gasResp=11412 pp=14 poked=1917` | ANQP build |
| `cu.usbmodem11301` | …plus `gasResp=2052 pp=13 poked=166` | ANQP build |
| `cu.usbmodem11401` | `cap fwd evt ddrop rdrop 2.4 5G ch sub` | plain sniffer — no ANQP |

**Resolved 2026-08-27:** all four were flashed from this source, so the split is gone and every
board now asks and hears on its own. Measured over 60 s, each alone, no partner and no phone
(`sub=0`), using `gasMine` — responses addressed to that board's own MAC:

| Board | poked | gasMine | gasResp |
|---|---|---|---|
| `D0:CF:13:ED:35:5C` | 160 | **104** | 123 |
| `D0:CF:13:EB:7D:20` | 145 | **90** | 175 |
| `D0:CF:13:ED:33:90` | 112 | **93** | 123 |
| `10:BD:A3:CA:1F:90` (Seeed 8 MB) | 140 | **162** | 164 |
| `D0:CF:13:EB:53:70` (recovered) | 36 | **42** | 46 |

**`gasResp` alone flatters a board.** The first row captured 161 GAS responses and only 80 were
answers to its own requests; the rest is these APs serving real clients. A board that had stopped
asking entirely would still show a healthy `gasResp`. That is what `gasMine` is for.

The **Seeed 8 MB** unit runs this image unchanged — same FQBN, same partition scheme.

The table is kept because the lesson is not: **two images can be in service without anything
noticing**, and both apps therefore detect a board's capabilities rather than assuming them.

Consequences for the app, all of them already handled rather than assumed:

- **Steerability is per board, detected not declared.** An image without `…0004` streams
  perfectly and cannot be told anything. Both apps look for the control characteristic during
  discovery and report the board as steerable only if it is there.
- **Poke state is per board too.** `0x03` is meaningless to an image with no poke code, so a
  write that appears to succeed changes nothing on half the fleet.
- **`0x04` is the same trap, one step worse.** A v1 image ignores it and streams anyway, so the
  app cannot tell a started board from one that never needed starting — and a v2 image that never
  receives it streams nothing. `run=` on the status line answers both: v1 does not emit the key at
  all, v1-with-nothing-sent is indistinguishable from working, and v2 says `run=0` outright.
- The status line's field list differs between the two builds, which is why `ch=` is parsed **by
  key and never by position**.

Two cheap checks, in order: read the status line over USB to see which build a board carries,
then write `0x01` with a known set and watch whether `ch=` follows it.

Flashing every board from `build/` puts them all on this source and removes the split.
