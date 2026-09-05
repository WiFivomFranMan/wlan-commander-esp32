// WLAN Commander — ESP32-C5 management/control-frame sniffer + ANQP, proof of concept.
//
// Promiscuous mode on 2.4 + 5 GHz, each frame wrapped in a synthesized radiotap header and
// streamed out as a single BLE GATT notification. The frame filter defaults to mgmt+ctrl — a 1x1
// low-power C5 is a mgmt sensor, not a full-capture rig — and 0x05 can widen it. 20/40 MHz is the
// chip's ceiling; there is no 80 or 160 to select here, at any opcode.
//
// **The board boots idle and stays idle until told otherwise.** No promiscuous, no hopping, no
// transmission — BLE advertising only. `0x04 SET_RUN 1` starts it and `0` stops it; subscribing to
// the frame characteristic starts nothing and never did anything but gate forwarding. This makes
// the C5 behave like every other capture device in the project, and replaces a v1 that swept and
// transmitted GAS from the instant it had power, unattended.
//
// ANQP: while running, the board auto-discovers Passpoint APs (802.11u / HS2.0 in beacons) and
// sends GAS/ANQP requests to them (a benign, standard pre-association client query — NOT an
// attack; no deauth, no injection of forged data frames). The response is captured by this same
// board and streamed like any other mgmt frame; the phone parses the ANQP. Poking is off by
// default and additionally requires `run=1`: `run=0` means no transmission, under any setting.
//
// **One board is enough.** This said the opposite until 2026-08-27, and the correction is worth
// keeping. Requests used to go out through `esp_wifi_80211_tx` — raw injection, straight past the
// MAC — and a board that did that never heard the reply, which was measured carefully and then
// generalised into "the C5's single radio is deaf to its own transmission" and a design that
// paired two boards co-channel. The radio was never the problem. `esp_wifi_action_tx_req` hands
// the frame to the MAC, which sends it from the real interface address, ACKs the reply, holds the
// channel for `wait_time_ms`, and lets the response reach the promiscuous callback like anything
// else. Measured: 58 responses from 4 APs on one board, every one addressed to it, zero
// listeners. `firmware/wlc_anqp_solo/` is the experiment and carries its numbers.
//
// Proven C5 recipe borrowed from CodeHedge/ESP32DualBandWardriver (MIT).

#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <NimBLEDevice.h>

// Uncomment to lock to one channel (bench test of poke + co-channel capture):
// #define LOCK_CH 11

static const char* WLC_SVC  = "57574c43-0001-4a50-8000-0000c5000001";
static const char* WLC_FRAME= "57574c43-0002-4a50-8000-0000c5000001"; // notify: radiotap+802.11
static const char* WLC_STAT = "57574c43-0003-4a50-8000-0000c5000001"; // notify: text stats
static const char* WLC_CMD  = "57574c43-0004-4a50-8000-0000c5000001"; // write: cmds

static const uint8_t DEFAULT_CH[] = {1,6,11,36,40,44,48,149,153,157,161,165};
static uint8_t activeCh[40];
static volatile int activeN = 0;
static uint8_t pendCh[40];
static volatile int pendN = -1;
static const uint32_t HOP_MS = 300;

// ---- commanded run / filter / width (0x04 / 0x05 / 0x06) ----
//
// `want*` is written only by the BLE callback, `isRun`/`applied*` only by `loop()`, which
// reconciles the two. Aligned word stores do not tear on this part, so the split needs no lock —
// and it keeps every wifi driver call on one task, in one place, in a fixed order.
static const uint32_t FILT_DEFAULT = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_CTRL;

// How long the link may stay down before the board stops itself. See `discAt` below.
//
// This bounds "has the link really gone", not how long anything may take to start — the
// distinction the app side has paid for three times. It only ever *stops* a capture, and only
// after the phone has been gone for the whole window, so a reading arriving late can never turn
// a working board off.
//
// 15 s rides out a reconnect: a supervision timeout is 4 s at these connection parameters and a
// re-advertise plus reconnect is well inside the rest. Shorter risks killing a capture over a
// walk behind a wall; longer leaves a board sniffing for no one, which is the whole bug.
static const uint32_t LINK_LOSS_GRACE_MS = 15000;

static volatile bool     wantRun   = false;                   // 0x04 — idle at boot, deliberately
static volatile uint32_t wantFilt  = FILT_DEFAULT;            // 0x05
static volatile uint8_t  wantWidth = WIFI_SECOND_CHAN_NONE;   // 0x06 — 0 HT20, 1 HT40+, 2 HT40-
static bool     isRun        = false;
static uint32_t appliedFilt  = FILT_DEFAULT;
static uint8_t  appliedWidth = WIFI_SECOND_CHAN_NONE;
// esp_wifi_set_channel refused — an HT40 pairing the channel cannot form is the way to get here.
// The hop then leaves `cur_ch` where it was, so without this the board would appear to ignore
// half its channel list for no stated reason. Loop-owned; the IRAM callback never touches it.
static uint32_t chfail = 0;

// ---- ANQP poke state ----
// Default **off**, which is also what a board with no phone attached now does. v1 defaulted this
// on and gated the poke loop on nothing but a timer, so eight boards transmitted GAS unattended
// from power-on. Requires `isRun` as well — see the poke block in `loop()`.
static volatile bool pokeEnable = false;          // phone toggles with CMD 0x03
// Re-poke each AP at most this often — and, with nothing here marking a target as answered,
// **forever**. `ppap[]` records when an AP was last asked and never that it replied, so a target
// loaded once is re-asked for the life of the power cycle. That is a policy, not a cadence: the
// interval itself is fine (a GAS answer arrives in tens of milliseconds; asking faster buys
// nothing), but an AP that has given a complete answer should stop being asked at all.
//
// The board cannot decide that — only whoever assembles the ANQP elements knows an answer is
// complete — and `0x02` appends with no opcode to remove an entry, so the app retires a target by
// taking its **channel** out of the `0x01` list once nothing on that channel is outstanding: a
// channel the board never visits is a target it cannot re-ask. See
// `DeviceManager::plan_c5_passpoint_start`. An answered AP co-channel with an unanswered one is
// still re-asked; closing that residual is the one thing that would need a change here.
static const uint32_t POKE_EACH_MS = 4000;
#define PP_MAX 24
// `tok` varies per poke because core keys a GAS transaction on (bssid, peer, dialog_token) and
// silently drops a fragment id it already holds. With the old constant 0x01 the second poll of
// an AP reused a live key, and its fragment 0 was discarded on arrival.
// `qset` alternates the two query sets — see build_gas.
// `lastTok`/`lastFrag` are the dedup key for *foreign* responses — see promisc_cb.
struct PPAp { uint8_t bssid[6]; uint8_t ch; uint8_t tok; uint8_t qset;
              uint8_t lastTok; uint8_t lastFrag;
              uint32_t lastPoke; uint32_t lastRespFwd; };
static PPAp ppap[PP_MAX];
static volatile int ppN = 0;
static volatile uint32_t poked = 0, gasResp = 0;  // pokes sent; GAS responses captured (any DA)

// Dialog tokens must vary: core keys a transaction on (bssid, peer, dialog_token) and drops a
// fragment id it already holds, so a constant token made every AP's second poll a no-op.
static uint8_t nextTok = 1;
static inline uint8_t alloc_tok(){ uint8_t t = nextTok++; if (nextTok == 0) nextTok = 1; return t; }

static bool pp_seen(const uint8_t* b){ for(int i=0;i<ppN;i++) if(!memcmp(ppap[i].bssid,b,6)) return true; return false; }
static void pp_add(const uint8_t* b, uint8_t ch, uint32_t now){
  if (ppN >= PP_MAX) return;
  memcpy(ppap[ppN].bssid,b,6); ppap[ppN].ch=ch; ppap[ppN].tok=0; ppap[ppN].qset=0;
  ppap[ppN].lastTok=0; ppap[ppN].lastFrag=0;
  // Seeded to `now - interval`, not 0. Unsigned wrap makes the *difference* right from the
  // first millisecond; with 0 the board dropped every GAS response for the first 3 s of uptime
  // as a duplicate, and left a freshly-discovered AP un-poked for up to 4 s.
  ppap[ppN].lastPoke    = now - POKE_EACH_MS;   // due immediately, as was always intended
  ppap[ppN].lastRespFwd = now - 3000u;          // nothing suppressed on arrival
  ppN++;
}

// A hostapd GAS Initial Response runs to `gas_frag_limit` (1400 B by default) and each
// GAS Comeback fragment is separately that size. The old 96 x 600 ring refused anything over
// 568 B, so a full-size ANQP answer could not enter the buffer at all — it was counted in
// `dropped` alongside genuine ring-full, which is why it never looked like a ceiling.
// 36 x 1602 = 57,672 B against the old 96 x 602 = 57,792 B: RAM is flat, depth is the cost.
// A **byte-oriented** ring, not an array of fixed slots. A 1400-byte GAS fragment and a
// 60-byte ACK each cost what they are, so the same ~56 KB holds roughly 190 average frames *and*
// a full-size ANQP answer. The 36 x 1600 slot array this replaced was measured losing 29% of
// captured frames on a busy channel — `rdrop=2488` against `fwd=1000`, depth pegged at 34 of 36 —
// because a fixed 1600-byte slot spends all of it on a 300-byte beacon.
//
// Records are `[u16 LE len][radiotap + 802.11]` and **never straddle the end of the pool**: one
// that will not fit contiguously leaves a zero-length wrap marker and restarts at 0. That keeps
// every payload a single contiguous span, so the BLE stage can hand a pointer straight to
// `emit_fragments` with no gather step.
//
// Single producer (`promisc_cb`, IRAM) writes `rhead` and nothing else; single consumer (`loop`)
// writes `rtail` and nothing else. On this unicore part that needs no lock, provided each index
// is published *after* the bytes it describes.
#define POOL_BYTES 57344
#define MAX_FRAME  1568                // above hostapd's 1400 B gas_frag_limit
static uint8_t pool[POOL_BYTES];
static volatile uint32_t rhead = 0, rtail = 0;

// Free bytes for a new record. One byte is always left unused so `rhead == rtail` can only ever
// mean empty, never full.
static inline uint32_t pool_free(uint32_t h, uint32_t t) {
  return (t + POOL_BYTES - h - 1) % POOL_BYTES;
}

static bool IRAM_ATTR pool_push(const uint8_t* rt, int rtlen, const uint8_t* f, int flen) {
  const uint32_t n = (uint32_t)(rtlen + flen);
  const uint32_t need = 2 + n;
  uint32_t h = rhead;
  const uint32_t t = rtail;
  if (need > pool_free(h, t)) return false;

  if (h + need > POOL_BYTES) {
    // No contiguous room before the end. Restart at 0 — but only if the consumer has moved past
    // 0, or we would overwrite the very record it is about to read. `pool_free` cannot express
    // that case: with `t == 0` the space before the tail is empty, not the whole pool.
    if (t == 0 || need > t - 1) return false;
    if (POOL_BYTES - h >= 2) { pool[h] = 0; pool[h + 1] = 0; }   // wrap marker
    h = 0;
  }
  pool[h] = n & 0xff; pool[h + 1] = (n >> 8) & 0xff;
  memcpy(pool + h + 2, rt, rtlen);
  memcpy(pool + h + 2 + rtlen, f, flen);
  const uint32_t nh = h + need;
  rhead = (nh >= POOL_BYTES) ? 0 : nh;          // published last
  return true;
}

// The record at the tail, or 0 when empty. `*out` is a contiguous payload of the returned length.
static inline uint16_t pool_peek(const uint8_t** out) {
  uint32_t t = rtail;
  const uint32_t h = rhead;
  if (t == h) return 0;
  // Fewer than two bytes before the end cannot hold a header, so that is an implicit wrap and
  // needs no marker — which is what makes the `POOL_BYTES - h >= 2` guard above safe.
  if (t + 2 > POOL_BYTES) { t = 0; rtail = 0; if (t == h) return 0; }
  uint16_t n = (uint16_t)(pool[t] | (pool[t + 1] << 8));
  if (n == 0) {                                  // explicit wrap marker
    t = 0; rtail = 0;
    if (t == h) return 0;
    n = (uint16_t)(pool[t] | (pool[t + 1] << 8));
    if (n == 0) return 0;                        // two markers in a row cannot happen
  }
  *out = pool + t + 2;
  return n;
}

static inline void pool_pop(uint16_t n) {
  const uint32_t t = rtail + 2 + n;
  rtail = (t >= POOL_BYTES) ? 0 : t;
}

static inline uint32_t pool_used(void) {
  return (rhead + POOL_BYTES - rtail) % POOL_BYTES;
}
static volatile uint32_t dropped = 0, captured = 0, cap24 = 0, cap5 = 0;
static volatile uint32_t fwd = 0, ddrop = 0, events = 0;
// Each of these was an unnamed discard that incremented `fwd` on its way out. A drop with no
// counter reads as a working sensor, which is exactly how the ANQP loss stayed invisible.
static volatile uint32_t odrop = 0;    // refused at capture: frame larger than MAX_FRAME
static volatile uint32_t bdrop = 0;    // refused at the BLE stage
static volatile uint32_t nfail = 0;    // notify() returned false — link full, frame kept
static volatile uint32_t fragged = 0;  // frames sent as a run of fragments
static volatile uint32_t poolmax = 0;  // ring high-water in BYTES, out of POOL_BYTES

#define DEDUP_SLOTS 512
#define DEDUP_BEACON_MS 5000u
struct BeaconSeen { uint8_t mac[6]; uint32_t last; bool used; };
static BeaconSeen seen[DEDUP_SLOTS];
static inline bool beacon_due(const uint8_t* bssid, uint32_t now) {
  uint16_t h = ((bssid[4] << 8) | bssid[5]) & (DEDUP_SLOTS - 1);
  BeaconSeen& e = seen[h];
  if (e.used && memcmp(e.mac, bssid, 6) == 0) {
    if (now - e.last < DEDUP_BEACON_MS) return false;
    e.last = now; return true;
  }
  memcpy(e.mac, bssid, 6); e.last = now; e.used = true; return true;
}

static NimBLECharacteristic* frameChar = nullptr;
static NimBLECharacteristic* statChar  = nullptr;
static volatile bool subscribed = false;

class SubCB : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t val) override { subscribed = (val != 0); }
};
static volatile uint16_t connH = 0xFFFF;   // live connection handle, for the MTU lookup

// millis() at which the link went away, or 0 while connected. `loop()` turns a long enough gap
// into `wantRun = false`; nothing here touches the wifi driver, per the rule above.
//
// **This is the backstop that makes stopping reliable, and it belongs here rather than in the
// app.** The app's own stop — `0x04 SET_RUN 0` before it drops the link — is still the clean
// path and still runs first. But it can only work when the app gets to run: it cannot cover a
// crash, a phone that runs out of battery, an iOS app killed while already suspended (which
// never receives willTerminate), or the phone simply walking out of range. Every one of those
// left a board sniffing indefinitely, which is what was observed on three boards on 2026-09-03.
//
// Stopping costs nothing real, because a board with no central **captures and discards** — the
// ring is drained to `rtail = rhead` while unsubscribed. An unconnected board burns power, heat
// and airtime to produce frames that reach no one.
static volatile uint32_t discAt = 0;
static uint32_t linkStops = 0;             // times the grace expired and stopped a live capture

class SrvCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    connH = info.getConnHandle();
    discAt = 0;                            // reconnected inside the grace: the capture survives
    s->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    connH = 0xFFFF;
    // 0 means "connected", so a disconnect landing exactly on millis() == 0 must not read as
    // one. Nudging to 1 costs a millisecond of grace and removes the case entirely.
    uint32_t t = millis();
    discAt = t ? t : 1;
    NimBLEDevice::startAdvertising();
  }
};
// Phone -> board: 0x01=SET_CHANNELS(+chan bytes); 0x02=ADD_POKE(+ [6 bssid,1 ch] records);
// 0x03=SET_POKE_ENABLE(+1 byte 0/1); 0x04=SET_RUN(+1 byte 0/1); 0x05=SET_FILTER(+u32 LE mask);
// 0x06=SET_WIDTH(+1 byte 0/1/2).
//
// Nothing here calls the wifi driver. 0x01 has always staged into `pendCh` for `loop()` to adopt,
// and the three new opcodes follow it rather than acting: they set a `want*` and `loop()`
// reconciles. That is what keeps the start ordering — filter, then channel+width, then
// promiscuous — in one place, and off the NimBLE host task.
class CmdCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    NimBLEAttValue v = c->getValue();
    const uint8_t* d = v.data(); size_t n = v.length();
    if (n >= 2 && d[0] == 0x01) {
      int k = 0;
      for (size_t i = 1; i < n && k < 40; i++) {
        uint8_t ch = d[i];
        if ((ch >= 1 && ch <= 14) || (ch >= 36 && ch <= 177)) pendCh[k++] = ch;
      }
      if (k > 0) pendN = k;
    } else if (n >= 8 && d[0] == 0x02) {          // ADD_POKE targets
      size_t i = 1;
      while (i + 7 <= n) {
        if (!pp_seen(d + i)) pp_add(d + i, d[i + 6], (uint32_t)millis());
        i += 7;
      }
    } else if (n >= 2 && d[0] == 0x03) {          // SET_POKE_ENABLE
      pokeEnable = (d[1] != 0);
    } else if (n >= 2 && d[0] == 0x04) {          // SET_RUN
      wantRun = (d[1] != 0);
    } else if (n >= 5 && d[0] == 0x05) {          // SET_FILTER, u32 little-endian
      wantFilt = (uint32_t)d[1] | ((uint32_t)d[2] << 8)
               | ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 24);
    } else if (n >= 2 && d[0] == 0x06 && d[1] <= 2) {   // SET_WIDTH; out of range is a no-op,
      wantWidth = d[1];                                 // same as 0x01's invalid-channel rule
    }
  }
};

static int build_radiotap(uint8_t* out, int8_t rssi, uint8_t ch) {
  uint16_t freq = (ch <= 14) ? (ch == 14 ? 2484 : 2407 + ch*5) : (5000 + ch*5);
  uint16_t chflags = (ch <= 14) ? 0x00A0 : 0x0140;
  out[0]=0; out[1]=0;
  uint32_t present = (1u<<1)|(1u<<2)|(1u<<3)|(1u<<5);
  out[4]=present&0xff; out[5]=(present>>8)&0xff; out[6]=(present>>16)&0xff; out[7]=(present>>24)&0xff;
  int o = 8;
  out[o++] = 0x10;
  out[o++] = 2;
  out[o++] = freq & 0xff; out[o++] = (freq>>8)&0xff;
  out[o++] = chflags & 0xff; out[o++] = (chflags>>8)&0xff;
  out[o++] = (uint8_t)rssi;
  uint16_t len = o;
  out[2]=len&0xff; out[3]=(len>>8)&0xff;
  return len;
}

// GAS Initial Request for ANQP — the action **body** only, category onwards.
//
// The driver writes the 802.11 header now, which is the whole point: the source address is the
// real interface MAC, so the reply is a directed, ACKed frame instead of something to be fished
// out of the air.
//
// **Two query sets, alternating per AP, which is what the WLANPi does.** wpa_supplicant's
// FETCH_ANQP builds a fixed list carrying no location element, so core asks for those
// separately per BSSID (`ANQP_LOCATION_QUERY_IDS`, core/src/device/manager.rs). Sending all
// fourteen at once was the wrong shape and the dangerous one: it is the request most likely to
// exceed hostapd's ANQP response buffer, whose overflow is a wpabuf abort that kills the
// daemon — an unauthenticated pre-association crash, and we re-poke every 4 s.
static const uint16_t QIDS_PASSPOINT[] = {257,258,260,261,262,263,264,268,277};
static const uint16_t QIDS_LOCATION[]  = {259,265,266,267,269,271};
// Only what core decodes. `parse_hs20_vendor` handles 3 (Operator Friendly Name), 4 (WAN
// Metrics) and 5 (Connection Capability); anything else would be answered and thrown away.
static const uint8_t HS20_SUBTYPES[] = {3,4,5};

static int build_gas(uint8_t* b, uint8_t tok, bool location_set) {
  const uint16_t* ids = location_set ? QIDS_LOCATION : QIDS_PASSPOINT;
  int k = location_set ? (int)(sizeof(QIDS_LOCATION)/2) : (int)(sizeof(QIDS_PASSPOINT)/2);

  int n=0;
  b[n++]=0x04; b[n++]=0x0a; b[n++]=tok;               // Public, GAS Initial Request, dialog token
  // Query Response Length Limit is **0 in a request**. 0x7f is the responder's value — hostapd's
  // gas.c says so outright ("0 for request and 1-0x7f for response") and this firmware sent the
  // responder's value in every request it ever made.
  b[n++]=108; b[n++]=2; b[n++]=0x00; b[n++]=0x00;     // limit 0 / PAME-BI 0; Adv Proto 0 = ANQP

  int qlen_at = n; n += 2;                             // Query Request Length, back-filled
  int q0 = n;
  b[n++]=0x00; b[n++]=0x01;                            // ANQP Query List, Info ID 256
  uint16_t ib=(uint16_t)(k*2); b[n++]=ib&0xff; b[n++]=ib>>8;
  for (int i=0;i<k;i++){ b[n++]=ids[i]&0xff; b[n++]=ids[i]>>8; }

  if (!location_set) {                                 // Vendor Specific 56797 -> HS2.0 query list
    b[n++]=0xdd; b[n++]=0xdd;
    int vlen_at = n; n += 2;
    int v0 = n;
    b[n++]=0x50; b[n++]=0x6f; b[n++]=0x9a;             // WFA OUI
    b[n++]=0x11;                                        // HS2.0 ANQP element type
    b[n++]=0x01;                                        // subtype 1 = Query List
    b[n++]=0x00;                                        // reserved
    for (unsigned i=0;i<sizeof(HS20_SUBTYPES);i++) b[n++]=HS20_SUBTYPES[i];
    uint16_t vl=(uint16_t)(n-v0); b[vlen_at]=vl&0xff; b[vlen_at+1]=vl>>8;
  }

  uint16_t ql=(uint16_t)(n-q0); b[qlen_at]=ql&0xff; b[qlen_at+1]=ql>>8;
  return n;
}

static uint8_t cur_ch = 1;
static uint8_t myMac[6];

// GAS responses addressed to **this board**, as opposed to every GAS response on the channel.
//
// `gasResp` counts what the radio captured, which on a busy channel is mostly other people's
// traffic — these APs answer real clients constantly. Only a reply whose destination is our own
// address answers a question *we* asked, so this is the number that says the board pokes and
// hears by itself. Without it, a board that had stopped asking entirely would still show a
// healthy `gasResp` and nobody would notice.
static volatile uint32_t gasMine = 0;

static void IRAM_ATTR promisc_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_CTRL) return;
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  int flen = p->rx_ctrl.sig_len;
  if (flen < 24) { dropped++; return; }   // `flen` is signed, so this covers <= 0 too
  if (flen > MAX_FRAME)       { odrop++;   return; }   // oversize is its own fact, not ring-full
  const uint8_t* f = p->payload;
  uint8_t ftype = (f[0] >> 2) & 0x3;
  uint8_t fsub  = (f[0] >> 4) & 0xf;
  captured++;
  if (cur_ch <= 14) cap24++; else cap5++;

  bool is_beacon = (ftype == 0 && fsub == 8);
  bool is_proberesp = (ftype == 0 && fsub == 5);
  // Note GAS responses for stats (they carry ANQP). f[0]==0xD0 action, cat public, action 0b/0d.
  if (ftype == 0 && fsub == 13 && flen >= 27 && f[24] == 0x04 && (f[25] == 0x0b || f[25] == 0x0d)) {
    gasResp++;
    bool mine = !memcmp(f + 4, myMac, 6);         // addr1 == us: an answer to our own request
    if (mine) gasMine++;
    // **A response addressed to this board is never deduped.** It answers a request this board
    // sent, so there is no such thing as a redundant one — and a GAS Comeback Response carries
    // a *fragment* the phone cannot reassemble without. The old rule dropped any response from
    // a BSSID within 3 s regardless of who it was addressed to, which cost two distinct things:
    // on an AP serving real clients our own answer was routinely suppressed by somebody else's,
    // and every comeback fragment after the first was discarded as a "duplicate retransmit"
    // because fragments of one transaction arrive milliseconds apart.
    if (!mine) {
      uint8_t rtok = f[26];
      uint8_t rfrg = (f[25] == 0x0d && flen >= 30) ? (f[29] & 0x7f) : 0;
      uint32_t nowm = (uint32_t)millis();
      for (int i = 0; i < ppN; i++) {
        if (memcmp(ppap[i].bssid, f + 16, 6) == 0) {
          // Keyed on (dialog token, fragment) as well as time, and only one deep — so two
          // interleaved foreign transactions with one AP defeat it and both are forwarded.
          // That is the right direction to fail: forward too much, never drop a distinct
          // fragment.
          if (rtok == ppap[i].lastTok && rfrg == ppap[i].lastFrag &&
              nowm - ppap[i].lastRespFwd < 3000u) { ddrop++; return; }
          ppap[i].lastTok = rtok; ppap[i].lastFrag = rfrg; ppap[i].lastRespFwd = nowm;
          break;
        }
      }
    }
  }

  if (is_beacon) {
    const uint8_t* bssid = f + 16;
    if (!beacon_due(bssid, (uint32_t)millis())) { ddrop++; return; }
    // beacon passed dedup (<=1 per AP per window): cheap to scan its IEs for Passpoint markers
    if (ppN < PP_MAX && !pp_seen(bssid)) {
      bool pp = false;
      uint8_t dsch = 0;
      int i = 36;
      while (i + 2 <= flen) {
        uint8_t eid = f[i], ln = f[i+1];
        if (i + 2 + ln > flen) break;
        const uint8_t* val = f + i + 2;
        if (eid == 107) pp = true;                                    // 802.11u Interworking
        if (eid == 221 && ln >= 4 && val[0]==0x50 && val[1]==0x6f && val[2]==0x9a && val[3]==0x10) pp = true; // HS2.0
        if (eid == 3  && ln >= 1) dsch = val[0];                      // DS Parameter Set
        if (eid == 61 && ln >= 1 && dsch == 0) dsch = val[0];         // HT Operation primary ch
        i += 2 + ln;
      }
      // The beacon names its own channel; `cur_ch` only names where we were standing. Adjacent
      // channel leakage means those differ — a 5 GHz AP logged on 149 while actually on 100 is
      // the case that put an AP in the list under a channel it would never be poked on again.
      if (pp) pp_add(bssid, dsch ? dsch : cur_ch, (uint32_t)millis());
    }
  } else {
    events++;
  }

  uint8_t rtbuf[16];
  const int rt = build_radiotap(rtbuf, p->rx_ctrl.rssi, cur_ch);
  if (!pool_push(rtbuf, rt, f, flen)) { dropped++; return; }
  fwd++;
  { const uint32_t u = pool_used(); if (u > poolmax) poolmax = u; }
  (void)is_proberesp;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  NimBLEDevice::init("WLC-C5-Sniffer");
  NimBLEDevice::setMTU(517);
  NimBLEServer* srv = NimBLEDevice::createServer();
  static SrvCB srvcb; srv->setCallbacks(&srvcb);
  NimBLEService* svc = srv->createService(WLC_SVC);
  frameChar = svc->createCharacteristic(WLC_FRAME, NIMBLE_PROPERTY::NOTIFY);
  statChar  = svc->createCharacteristic(WLC_STAT,  NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* cmdChar = svc->createCharacteristic(WLC_CMD, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  static CmdCB cmdcb; cmdChar->setCallbacks(&cmdcb);
  static SubCB subcb; frameChar->setCallbacks(&subcb);
  svc->start();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(WLC_SVC);
  adv->setName("WLC-C5-Sniffer");
  NimBLEDevice::startAdvertising();

  WiFi.mode(WIFI_MODE_STA);
  WiFi.setBandMode(WIFI_BAND_MODE_AUTO);
  WiFi.disconnect();
  // The callback is registered now; promiscuous stays **off** until 0x04 SET_RUN 1. The filter is
  // applied from `wantFilt` at every start, so there is nothing to set here either.
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(&promisc_cb);
#ifdef LOCK_CH
  activeCh[0] = LOCK_CH; activeN = 1;
#else
  memcpy(activeCh, DEFAULT_CH, sizeof(DEFAULT_CH));
  activeN = sizeof(DEFAULT_CH);
#endif
  cur_ch = activeCh[0];
  esp_wifi_set_channel(cur_ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_get_mac(WIFI_IF_STA, myMac);

  Serial.println("WLC-C5-Sniffer up: idle. BLE advertising; awaiting 0x04 SET_RUN 1.");
#ifdef RGB_BUILTIN
  rgbLedWrite(RGB_BUILTIN, 0, 0, 40);          // blue = idle, matching the heartbeat below
#endif
}

static uint32_t lastHop = 0, lastStat = 0, lastPokeChk = 0;
static int chIdx = 0;

// The onboard LED as a liveness signal, and as a way to find one board among several.
//
// A board pulses dim once a second — green while capturing, blue while idle — and flashes brighter
// for a moment whenever an ANQP answer addressed to *this* board arrives. Three things are readable
// from across a bench without a serial cable: which boards are alive, which have been started, and
// which are actually getting replies.
//
// The two colours arrived with 0x04. Under v1 every powered board captured, so green meant
// "working"; boards now boot idle, and one colour would have made an untasked fleet look busy.
//
// It earns its place from a specific failure. A board whose flash is half-written is silent on
// serial, silent on BLE, and identical from the outside to one that is simply not plugged in —
// and the port it appears on is not a reliable way to tell boards apart (see the README). Making
// every working board announce itself turns "which one is broken?" into "which one is dark?",
// which is answerable by looking. Guarded on `RGB_BUILTIN`, so a board without an addressable LED
// simply skips it.
static void led_tick(uint32_t now) {
#ifdef RGB_BUILTIN
  static uint32_t lastBeat = 0, litUntil = 0, seenMine = 0;
  static bool lit = false;

  // A fresh answer to our own request: brighter, briefly. Read in `loop` rather than the
  // promiscuous callback, which runs in IRAM and must not touch the LED driver.
  if (gasMine != seenMine) {
    seenMine = gasMine;
    rgbLedWrite(RGB_BUILTIN, 0, 90, 30);
    litUntil = now + 120;
    lit = true;
    return;
  }
  if (lit && now >= litUntil) { rgbLedWrite(RGB_BUILTIN, 0, 0, 0); lit = false; }

  if (now - lastBeat >= 1000) {
    lastBeat = now;
    if (isRun) rgbLedWrite(RGB_BUILTIN, 0, 12, 0);   // dim green: capturing
    else       rgbLedWrite(RGB_BUILTIN, 0, 0, 14);   // dim blue: alive, idle. Dark still = broken
    litUntil = now + 60;
    lit = true;
  }
#else
  (void)now;
#endif
}

// The largest notification payload this link carries: the negotiated ATT MTU less its 3-byte
// header. Read fresh every pass, because the MTU exchange can complete after subscribe.
#define BLE_BUF 544
static uint16_t notify_cap() {
  uint16_t mtu = 23;                                   // the floor every LE link must support
  NimBLEServer* srv = NimBLEDevice::getServer();
  if (srv && connH != 0xFFFF) { uint16_t m = srv->getPeerMTU(connH); if (m > mtu) mtu = m; }
  // `mtu - 3` is the ATT arithmetic (opcode + handle + value) and it is **four bytes too
  // optimistic in practice**. Measured 2026-08-31 against macOS at a negotiated MTU of 517:
  // a 514-byte notification is delivered to the central as a **zero-length** value — not
  // truncated, not refused, empty — while 510 arrives intact. Every notification in an hour of
  // capture tops out at 510.
  //
  // That cost the whole fragmentation path silently: the first fragment is the only record
  // built right up to `cap`, so every fragmented frame lost its head, core saw an orphan
  // continuation and discarded it, and the board still counted `frag=21`. Losing the largest
  // frames is exactly the bug fragmentation exists to fix.
  //
  // Empirical, not derived — I have no first-principles account of the missing four bytes, so
  // the margin is set from the measurement and named as such.
  uint16_t cap = (mtu > 7) ? (uint16_t)(mtu - 7) : 16;
  if (cap > BLE_BUF) cap = BLE_BUF;
  return cap;
}

// notify() returns false when NimBLE has no mbufs left. That return was discarded and `tail`
// had already advanced, so those frames were counted in `fwd` and dropped without a trace.
static bool emit(const uint8_t* p, int n) {
  frameChar->setValue(p, n);
  if (frameChar->notify()) return true;
  nfail++;                        // the frames stay in the ring and go out on a later pass
  return false;
}

// One oversized frame as a run of fragment records, each owning a whole notification. The top
// two bits of the length word are a record type; the low 14 hold the bytes in this record:
//
//   0x0000 whole frame        (unchanged — what every frame that fits still looks like)
//   0x4000 first fragment     (payload opens with the u16 total frame length)
//   0x8000 middle fragment
//   0xc000 last fragment
//
// A two-bit type rather than MORE/FIRST flags, because with flags a *last* fragment carries
// neither and is indistinguishable from a whole frame — which is exactly what the reassembly
// test caught. The first fragment's declared total is what makes a lost middle detectable
// rather than silently concatenated into a shorter frame that still parses.
//
// Nothing else shares a notification with a fragment. That is what keeps an older core safe:
// it reads a flagged length as absurd, breaks out of that batch, and so loses exactly the
// oversized frames it was already losing and not one small frame more.
static bool emit_fragments(const uint8_t* p, uint16_t total, uint16_t cap) {
  static uint8_t fb[BLE_BUF];
  if (cap < 8) { bdrop++; return true; }     // link too small to carry it; don't spin on it
  uint16_t off = 0;
  bool first = true;
  while (off < total) {
    uint16_t room = cap - 2 - (first ? 2 : 0);
    uint16_t n = (uint16_t)(total - off);
    if (n > room) n = room;
    bool more = (uint16_t)(off + n) < total;
    uint16_t rec = n + (first ? 2 : 0);
    uint16_t w = rec | (first ? 0x4000 : (more ? 0x8000 : 0xc000));
    int o = 0;
    fb[o++] = w & 0xff; fb[o++] = (w >> 8) & 0xff;
    if (first) { fb[o++] = total & 0xff; fb[o++] = (total >> 8) & 0xff; }
    memcpy(fb + o, p + off, n); o += n;
    // A mid-run failure is recoverable: the ring slot is untouched, so the next pass restarts
    // the run, and core discards its half-built partial when the second FIRST arrives.
    if (!emit(fb, o)) return false;
    off += n; first = false;
  }
  return true;
}

void loop() {
  uint32_t now = millis();
  led_tick(now);

  if (pendN >= 0) {
    int k = pendN; for (int i = 0; i < k; i++) activeCh[i] = pendCh[i];
    activeN = k; pendN = -1; chIdx = 0;
  }
  // Dead man's switch. The phone has been gone for the whole grace window, so stop — expressed
  // as `wantRun = false` rather than a driver call so it goes through the one reconcile below,
  // in the same order as every other stop. `discAt` is cleared so a board that reconnects and is
  // started again is not immediately stopped by a stale timestamp.
  //
  // Unsigned arithmetic makes the millis() rollover at ~49 days a non-event: the subtraction
  // wraps to the true elapsed value.
  if (wantRun && discAt != 0 && (uint32_t)(now - discAt) >= LINK_LOSS_GRACE_MS) {
    wantRun = false;
    discAt  = 0;
    linkStops++;
  }

  // Reconcile what the phone asked for. Every wifi driver call in this sketch is reached from
  // here, so the start order — filter, then channel+width, then promiscuous — is stated once.
  if (wantRun != isRun) {
    isRun = wantRun;
    if (isRun) {
      appliedFilt = wantFilt;
      wifi_promiscuous_filter_t f = { .filter_mask = appliedFilt };
      esp_wifi_set_promiscuous_filter(&f);
      esp_wifi_set_promiscuous(true);
      appliedWidth = wantWidth;
      if (activeN > 0) chIdx = 0;
      uint8_t want = (activeN > 0) ? activeCh[chIdx] : cur_ch;
      if (esp_wifi_set_channel(want, (wifi_second_chan_t)appliedWidth) == ESP_OK) cur_ch = want;
      else chfail++;
      lastHop = now;                       // a whole dwell on the channel we just landed on
    } else {
      esp_wifi_set_promiscuous(false);     // the poke block is gated on isRun, so TX stops with it
    }
  }
  if (isRun) {
    if (wantFilt != appliedFilt) {
      appliedFilt = wantFilt;
      wifi_promiscuous_filter_t f = { .filter_mask = appliedFilt };
      esp_wifi_set_promiscuous_filter(&f);
    }
    // A width change only takes effect through a set_channel, and deferring it to the next hop
    // would leave a board holding a single channel on the old width for as long as it holds it.
    if (wantWidth != appliedWidth) {
      appliedWidth = wantWidth;
      if (esp_wifi_set_channel(cur_ch, (wifi_second_chan_t)appliedWidth) != ESP_OK) chfail++;
    }
  }

  if (isRun && activeN > 0 && now - lastHop >= HOP_MS) {
    lastHop = now;
    chIdx = (chIdx + 1) % activeN;
    uint8_t want = activeCh[chIdx];
    // 20/40 only — the C5 has no wider option to pass here.
    if (esp_wifi_set_channel(want, (wifi_second_chan_t)appliedWidth) == ESP_OK) cur_ch = want;
    else chfail++;   // e.g. HT40 on a channel with no partner: skipped, and said so
  }

  // ANQP poke: while dwelling on cur_ch, send a GAS request to one due co-channel Passpoint AP.
  // Sparse (one per check, each AP every POKE_EACH_MS) so it barely dents our own sniffing.
  // Gated on `isRun` as well: run=0 is no transmission whatever poke says. A stop deliberately
  // does *not* clear `pokeEnable` — that is the phone's standing request, and clearing it would
  // lose the setting across a stop/start the phone never asked to change.
  if (isRun && pokeEnable && now - lastPokeChk >= 100) {
    lastPokeChk = now;
    for (int i = 0; i < ppN; i++) {
      if (ppap[i].ch == cur_ch && now - ppap[i].lastPoke >= POKE_EACH_MS) {
        ppap[i].tok = alloc_tok();
        uint8_t body[96]; int blen = build_gas(body, ppap[i].tok, ppap[i].qset != 0);
        // `data[0]` is a flexible member, so request and payload are one allocation.
        uint8_t buf[sizeof(wifi_action_tx_req_t) + sizeof(body)];
        memset(buf, 0, sizeof(buf));
        wifi_action_tx_req_t* req = (wifi_action_tx_req_t*)buf;
        req->ifx = WIFI_IF_STA;
        memcpy(req->dest_mac, ppap[i].bssid, 6);
        req->type = WIFI_OFFCHAN_TX_REQ;
        req->channel = cur_ch;
        req->sec_channel = WIFI_SECOND_CHAN_NONE;
        // Shorter than the hop dwell on purpose. The response arrives in tens of milliseconds and
        // the promiscuous path picks it up regardless; a long hold here would stall the sweep.
        req->wait_time_ms = 250;
        req->no_ack = false;                 // let the MAC do its job — this is the difference
        req->rx_cb = NULL;                   // the promiscuous callback already sees the reply
        req->data_len = blen;
        memcpy(req->data, body, blen);
        if (esp_wifi_action_tx_req(req) == ESP_OK) { poked++; ppap[i].lastPoke = now; ppap[i].qset ^= 1; }
        break;
      }
    }
  }

  // Forwarded only while somebody is listening *and* capture was asked for. Everything else is
  // discarded below rather than left to age in the ring, so a start never opens with frames
  // captured before it.
  if (subscribed && isRun && frameChar) {
    static uint8_t batch[BLE_BUF];
    uint16_t cap = notify_cap();
    int blen = 0, budget = 96;
    uint32_t mark = rtail;                   // start of what is staged but not yet delivered
    const uint8_t* rec = nullptr;
    uint16_t fl;
    while ((fl = pool_peek(&rec)) != 0 && budget-- > 0) {
      if ((uint32_t)fl + 2 > (uint32_t)cap) {
        // Larger than one notification. This used to be a silent `continue` — the frame was
        // skipped after `fwd++` had already counted it delivered, and it appeared in no drop
        // counter. It is the reason a full-size ANQP answer never reached the phone.
        if (blen > 0) { if (!emit(batch, blen)) { rtail = mark; break; } blen = 0; }
        if (!emit_fragments(rec, fl, cap)) break;   // kept in the ring; retried next pass
        fragged++;
        pool_pop(fl); mark = rtail;
        continue;
      }

      if (blen + 2 + fl > cap) {
        if (!emit(batch, blen)) { rtail = mark; break; }
        blen = 0; mark = rtail;
      }
      batch[blen++] = fl & 0xff; batch[blen++] = (fl >> 8) & 0xff;
      memcpy(batch + blen, rec, fl); blen += fl;
      pool_pop(fl);
    }
    if (blen > 0 && !emit(batch, blen)) rtail = mark;
  } else {
    rtail = rhead;   // discard whatever was captured while stopped or unsubscribed
  }

  if (now - lastStat >= 2000) {
    lastStat = now;
    // 448, not 256. Measured worst case — every uint32 counter at 4294967295 — was 363 bytes with
    // NUL before `linkstop=` was added, and that field adds at most 19 more, so 382. The same
    // measurement puts the *pre-0x04* line at 307, so 256 was already 51 bytes short of its own
    // worst case before a field was added. snprintf truncates from the tail in silence,
    // and the tail is where `ch=` and `sub=` live. A realistic line is ~225.
    //
    // The control state leads, because it is what a human reads first and what no truncation can
    // reach. `run=` is the driver's actual state; `filt=`/`width=` are the *commanded* values, so
    // that a write made while stopped reads back immediately instead of after the next start.
    char msg[448];
    snprintf(msg, sizeof(msg),
             "run=%d poke=%d filt=%lx width=%u "
             "cap=%lu fwd=%lu evt=%lu gasResp=%lu gasMine=%lu pp=%d poked=%lu ddrop=%lu rdrop=%lu "
             "odrop=%lu bdrop=%lu nfail=%lu frag=%lu poolmax=%lu chfail=%lu heap=%lu mtu=%u "
             "2.4=%lu 5G=%lu ch=%u sub=%d linkstop=%lu",
             isRun ? 1 : 0, pokeEnable ? 1 : 0, (unsigned long)wantFilt, (unsigned)wantWidth,
             (unsigned long)captured, (unsigned long)fwd, (unsigned long)events, (unsigned long)gasResp,
             (unsigned long)gasMine,
             ppN, (unsigned long)poked, (unsigned long)ddrop, (unsigned long)dropped,
             (unsigned long)odrop, (unsigned long)bdrop, (unsigned long)nfail, (unsigned long)fragged,
             (unsigned long)poolmax, (unsigned long)chfail,
             (unsigned long)esp_get_free_heap_size(), (unsigned)notify_cap() + 3,
             (unsigned long)cap24, (unsigned long)cap5, cur_ch, subscribed?1:0,
             (unsigned long)linkStops);
    Serial.println(msg);
    if (subscribed && statChar) { statChar->setValue((uint8_t*)msg, strlen(msg)); statChar->notify(); }
  }
}

