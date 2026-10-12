// SPDX-License-Identifier: MIT
#include "wlc_survey/survey.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

#include "internal.h"

namespace wlc {

using detail::IeWalker;
using detail::le16;
using detail::mac_eq;
using detail::MacIndex;

namespace {

constexpr int8_t kNoRssi = -128;

// Everything one beacon / probe response says, gathered before the table lock is taken.
struct BssObservation {
  const uint8_t* ssid = nullptr;
  uint8_t ssid_len = 0;
  bool ssid_present = false;
  bool hidden = false;
  bool complete = true;  // IE walk reached the end; absence of an element is then meaningful
  uint16_t interval = 0, capability = 0;

  uint8_t ds_channel = 0, ht_primary = 0, he6_primary = 0;
  int8_t ht_sec_offset = -1;
  bool ht_sta_width = false;
  int8_t vht_width = -1;
  uint8_t vht_ccfs1 = 0;
  int8_t he6_width = -1;
  int8_t eht_width = -1;

  uint8_t phy = 0;
  bool rsn = false, wpa = false;
  uint32_t akm_mask = 0;
  uint8_t wpa_akm_mask = 0;
  uint16_t pairwise = 0;
  uint8_t group = 0;
  bool mfpc = false, mfpr = false;

  bool bss_load = false;
  uint16_t sta_count = 0;
  uint8_t ch_util = 0;
  char country[3] = {0, 0, 0};

  bool hs20 = false;
  uint8_t hs20_release = 0;
  bool hs20_dgaf_disabled = false;
  bool interworking = false;
  uint8_t ant = 0;
  bool internet = false, asra = false, esr = false, uesa = false;
  bool has_venue = false;
  uint8_t venue_group = 0, venue_type = 0;
  bool has_hessid = false;
  uint8_t hessid[6] = {0};
  bool rc = false;
  uint8_t rc_anqp_count = 0;
  uint8_t rc_n = 0;
  OI rc_ois[3] = {};
};

bool valid_channel(Band band, unsigned ch) {
  switch (band) {
    case Band::B2G4:
      return ch >= 1 && ch <= 14;
    case Band::B5G:
      return (ch >= 32 && ch <= 177 && ch % 4 == 0) || (ch >= 149 && ch <= 177 && ch % 4 == 1);
    case Band::B6G:
      return ch == 2 || (ch >= 1 && ch <= 233 && ch % 4 == 1);
  }
  return false;
}

uint16_t cipher_bit(const uint8_t* suite) {
  if (suite[0] != 0x00 || suite[1] != 0x0f || suite[2] != 0xac || suite[3] > 15) return 0;
  return static_cast<uint16_t>(1u << suite[3]);
}

void parse_rsn(BssObservation& o, const uint8_t* d, size_t n) {
  o.rsn = true;
  if (n < 2) return;
  size_t pos = 2;  // version
  if (pos + 4 > n) return;
  o.group = (d[pos] == 0x00 && d[pos + 1] == 0x0f && d[pos + 2] == 0xac) ? d[pos + 3] : 0;
  pos += 4;
  if (pos + 2 > n) return;
  size_t count = le16(d + pos);
  pos += 2;
  for (size_t i = 0; i < count; ++i, pos += 4) {
    if (pos + 4 > n) return;
    o.pairwise |= cipher_bit(d + pos);
  }
  if (pos + 2 > n) return;
  count = le16(d + pos);
  pos += 2;
  for (size_t i = 0; i < count; ++i, pos += 4) {
    if (pos + 4 > n) return;
    if (d[pos] == 0x00 && d[pos + 1] == 0x0f && d[pos + 2] == 0xac && d[pos + 3] < 32) {
      o.akm_mask |= 1u << d[pos + 3];
    }
  }
  if (pos + 2 > n) return;
  const uint16_t caps = le16(d + pos);
  o.mfpr = (caps & 0x0040) != 0;
  o.mfpc = (caps & 0x0080) != 0;
}

void parse_wpa1(BssObservation& o, const uint8_t* d, size_t n) {
  // d points past OUI+type: version(2) group(4) pairwise count + list, AKM count + list
  o.wpa = true;
  size_t pos = 2 + 4;
  if (pos + 2 > n) return;
  size_t count = le16(d + pos);
  pos += 2 + 4 * count;
  if (pos + 2 > n) return;
  count = le16(d + pos);
  pos += 2;
  for (size_t i = 0; i < count && pos + 4 <= n; ++i, pos += 4) {
    if (d[pos] == 0x00 && d[pos + 1] == 0x50 && d[pos + 2] == 0xf2 && d[pos + 3] < 8) {
      o.wpa_akm_mask = static_cast<uint8_t>(o.wpa_akm_mask | (1u << d[pos + 3]));
    }
  }
}

void parse_he_operation(BssObservation& o, const uint8_t* d, size_t n) {
  if (n < 6) return;
  const bool vht_info = (d[1] & 0x40) != 0;
  const bool cohosted = (d[1] & 0x80) != 0;
  const bool six_ghz = (d[2] & 0x02) != 0;
  size_t pos = 6;
  if (vht_info) {
    if (pos + 3 > n) return;
    // Embedded VHT Operation Information; only wins when it says something wider.
    if (o.vht_width < 0 || d[pos] > o.vht_width) {
      o.vht_width = static_cast<int8_t>(d[pos]);
      o.vht_ccfs1 = d[pos + 2];
    }
    pos += 3;
  }
  if (cohosted) pos += 1;
  if (six_ghz && pos + 5 <= n) {
    o.he6_primary = d[pos];
    o.he6_width = static_cast<int8_t>(d[pos + 1] & 0x03);
  }
}

void parse_vendor(BssObservation& o, const uint8_t* d, size_t n) {
  if (n < 4) return;
  if (d[0] == 0x50 && d[1] == 0x6f && d[2] == 0x9a && d[3] == 0x10) {  // HS2.0 Indication
    o.hs20 = true;
    if (n >= 5) {
      o.hs20_release = static_cast<uint8_t>((d[4] >> 4) + 1);
      o.hs20_dgaf_disabled = (d[4] & 0x01) != 0;
    }
  } else if (d[0] == 0x00 && d[1] == 0x50 && d[2] == 0xf2 && d[3] == 0x01) {  // WPA1
    parse_wpa1(o, d + 4, n - 4);
  }
}

void parse_interworking(BssObservation& o, const uint8_t* d, size_t n) {
  if (n < 1) return;
  o.interworking = true;
  o.ant = d[0] & 0x0f;
  o.internet = (d[0] & 0x10) != 0;
  o.asra = (d[0] & 0x20) != 0;
  o.esr = (d[0] & 0x40) != 0;
  o.uesa = (d[0] & 0x80) != 0;
  // The optional fields are told apart by length alone: 3 venue, 7 HESSID, 9 both.
  if (n == 3 || n == 9) {
    o.has_venue = true;
    o.venue_group = d[1];
    o.venue_type = d[2];
  }
  if (n == 7 || n == 9) {
    o.has_hessid = true;
    std::memcpy(o.hessid, d + n - 6, 6);
  }
}

void parse_roaming_consortium(BssObservation& o, const uint8_t* d, size_t n) {
  if (n < 2) return;
  o.rc = true;
  o.rc_anqp_count = d[0];
  const size_t l1 = d[1] & 0x0f, l2 = d[1] >> 4;
  size_t pos = 2;
  auto add = [&](size_t len) {
    if (len == 0 || pos + len > n || o.rc_n >= 3) return false;
    OI& oi = o.rc_ois[o.rc_n++];
    oi.len = static_cast<uint8_t>(len < sizeof(oi.b) ? len : sizeof(oi.b));
    std::memcpy(oi.b, d + pos, oi.len);
    pos += len;
    return true;
  };
  if (add(l1) && add(l2) && pos < n) add(n - pos);  // OI #3 is whatever remains
}

void parse_bss_ies(BssObservation& o, const uint8_t* ies, size_t n) {
  IeWalker w(ies, n);
  uint8_t id, len;
  const uint8_t* d;
  while (w.next(&id, &d, &len)) {
    switch (id) {
      case 0:
        if (!o.ssid_present) {
          o.ssid_present = true;
          o.ssid = d;
          o.ssid_len = len > 32 ? 32 : len;
          bool all_nul = true;
          for (size_t i = 0; i < len; ++i) all_nul = all_nul && d[i] == 0;
          o.hidden = all_nul;  // also true for len == 0
        }
        break;
      case 3:
        if (len == 1) o.ds_channel = d[0];
        break;
      case 7:
        if (len >= 2 && d[0] >= 0x20 && d[0] < 0x7f && d[1] >= 0x20 && d[1] < 0x7f) {
          o.country[0] = static_cast<char>(d[0]);
          o.country[1] = static_cast<char>(d[1]);
        }
        break;
      case 11:
        if (len >= 5) {
          o.bss_load = true;
          o.sta_count = le16(d);
          o.ch_util = d[2];
        }
        break;
      case 45:
        o.phy |= PHY_HT;
        break;
      case 48:
        parse_rsn(o, d, len);
        break;
      case 61:
        o.phy |= PHY_HT;
        if (len >= 22) o.ht_primary = d[0];
        if (len >= 2) {
          o.ht_sec_offset = static_cast<int8_t>(d[1] & 0x03);
          o.ht_sta_width = (d[1] & 0x04) != 0;
        }
        break;
      case 107:
        parse_interworking(o, d, len);
        break;
      case 111:
        parse_roaming_consortium(o, d, len);
        break;
      case 191:
        o.phy |= PHY_VHT;
        break;
      case 192:
        o.phy |= PHY_VHT;
        if (len >= 1 && (o.vht_width < 0 || d[0] > o.vht_width)) {
          o.vht_width = static_cast<int8_t>(d[0]);
          o.vht_ccfs1 = len >= 3 ? d[2] : 0;
        }
        break;
      case 221:
        parse_vendor(o, d, len);
        break;
      case 255:
        if (len < 1) break;
        switch (d[0]) {
          case 35:
            o.phy |= PHY_HE;
            break;
          case 36:
            o.phy |= PHY_HE;
            parse_he_operation(o, d + 1, len - 1u);
            break;
          case 106:
            o.phy |= PHY_EHT;
            // Parameters(1), Basic EHT-MCS and NSS Set(4), then Control when info is present.
            if (len >= 1 + 1 + 4 + 3 && (d[1] & 0x01)) o.eht_width = static_cast<int8_t>(d[6] & 0x07);
            break;
          case 108:
            o.phy |= PHY_EHT;
            break;
          default:
            break;
        }
        break;
      default:
        break;
    }
  }
  o.complete = !w.overrun;
}

Width resolve_width(const BssObservation& o) {
  if (o.eht_width >= 0) {
    static const Width m[] = {Width::W20, Width::W40, Width::W80, Width::W160, Width::W320};
    return o.eht_width <= 4 ? m[o.eht_width] : Width::W20;
  }
  if (o.he6_width >= 0) {
    static const Width m[] = {Width::W20, Width::W40, Width::W80, Width::W160};
    return m[o.he6_width & 3];
  }
  // VHT width 0 defers to HT; 1 is 80 unless CCFS1 names a 160/80+80 centre.
  if (o.vht_width == 1) return o.vht_ccfs1 ? Width::W160 : Width::W80;
  if (o.vht_width == 2 || o.vht_width == 3) return Width::W160;
  if ((o.ht_sec_offset == 1 || o.ht_sec_offset == 3) && o.ht_sta_width) return Width::W40;
  return Width::W20;
}

uint8_t resolve_channel(const BssObservation& o, const RxMeta& m) {
  unsigned adv = 0;
  if (m.band == Band::B6G) {
    adv = o.he6_primary;
  } else if (o.ds_channel && o.ht_primary) {
    adv = o.ds_channel == o.ht_primary ? o.ds_channel : 0;  // conflicting IEs: trust neither
  } else {
    adv = o.ds_channel ? o.ds_channel : o.ht_primary;
  }
  return (adv && valid_channel(m.band, adv)) ? static_cast<uint8_t>(adv) : m.channel;
}

constexpr uint32_t kAkmEnterprise = (1u << 1) | (1u << 3) | (1u << 5) | (1u << 11) | (1u << 12) |
                                    (1u << 13) | (1u << 14) | (1u << 15) | (1u << 16) | (1u << 17) |
                                    (1u << 22) | (1u << 23);
constexpr uint32_t kAkmSuiteB = (1u << 11) | (1u << 12);
constexpr uint32_t kAkmPsk = (1u << 2) | (1u << 4) | (1u << 6) | (1u << 19) | (1u << 20);
constexpr uint32_t kAkmSae = (1u << 8) | (1u << 9) | (1u << 24) | (1u << 25);
constexpr uint32_t kAkmOwe = 1u << 18;

Security resolve_security(const BssObservation& o) {
  if (o.rsn) {
    if (o.akm_mask & kAkmEnterprise) return Security::Enterprise;
    if ((o.akm_mask & kAkmSae) && (o.akm_mask & kAkmPsk)) return Security::WPA2WPA3;
    if (o.akm_mask & kAkmSae) return Security::WPA3SAE;
    if (o.akm_mask & kAkmPsk) return Security::WPA2Personal;
    if (o.akm_mask & kAkmOwe) return Security::OWE;
    return o.wpa ? Security::WPA : Security::Unknown;
  }
  if (o.wpa) return Security::WPA;
  if (o.capability & 0x0010) return Security::WEP;  // Privacy bit with no RSN/WPA element
  return Security::Open;
}

void rssi_fold(int8_t rssi, int16_t* q4, int8_t* last, int8_t* avg, int8_t* peak, uint8_t shift) {
  if (rssi >= 0) return;
  *last = rssi;
  if (*peak == kNoRssi || rssi > *peak) *peak = rssi;
  const int target = rssi * 16;
  if (*q4 == INT16_MIN) {
    *q4 = static_cast<int16_t>(target);
  } else {
    *q4 = static_cast<int16_t>(*q4 + (target - *q4) / (1 << shift));
  }
  const int v = *q4 >= 0 ? (*q4 + 8) / 16 : -((-*q4 + 8) / 16);
  *avg = static_cast<int8_t>(v);
}

int ascii_casecmp(const uint8_t* a, size_t an, const uint8_t* b, size_t bn) {
  const size_t n = an < bn ? an : bn;
  for (size_t i = 0; i < n; ++i) {
    int ca = a[i], cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca += 32;
    if (cb >= 'A' && cb <= 'Z') cb += 32;
    if (ca != cb) return ca - cb;
  }
  return static_cast<int>(an) - static_cast<int>(bn);
}

}  // namespace

// ---------------------------------------------------------------------------------------------

struct Survey::Impl {
  struct ApEntry {
    ApInfo info;
    int16_t rssi_q4;
    int16_t anqp;  // slot in anqp[], -1 none
    bool used;
  };
  struct ClientEntry {
    ClientInfo info;
    int16_t rssi_q4;
    bool used;
  };
  enum : uint8_t { SLOT_FREE = 0, SLOT_ATTACHED = 1, SLOT_PENDING = 2 };
  struct AnqpSlot {
    AnqpRecord rec;
    uint8_t owner[6];
    uint8_t state;
    uint32_t touched_ms;
  };
  struct GasTxn {
    bool used;
    uint8_t sa[6];
    uint8_t token;
    uint8_t bssid[6];
    uint8_t next_frag;
    uint16_t len;
    uint32_t touched_ms;
    uint8_t* buf;
  };

  Config cfg;
  NullLock null_lock;
  Lock* lock = nullptr;
  void* block = nullptr;

  ApEntry* aps = nullptr;
  MacIndex ap_index;
  uint16_t n_aps = 0;
  ClientEntry* clients = nullptr;
  MacIndex client_index;
  uint16_t n_clients = 0;
  AnqpSlot* anqp = nullptr;
  uint16_t n_pending = 0;
  GasTxn* gas = nullptr;
  uint16_t* order = nullptr;  // snapshot sort scratch; only touched under the lock
  AnqpRecord* scratch = nullptr;
  Stats st = {};

  struct Guard {
    Lock* l;
    explicit Guard(Lock* lk) : l(lk) { l->lock(); }
    ~Guard() { l->unlock(); }
  };

  // --- layout -------------------------------------------------------------------------------
  struct Layout {
    size_t aps, ap_idx, clients, cl_idx, anqp, gas, gas_buf, order, scratch, total;
  };
  static size_t align(size_t v) { return (v + 15) & ~static_cast<size_t>(15); }
  static Layout layout(const Config& c) {
    Layout l{};
    size_t off = 0;
    auto take = [&](size_t bytes) {
      const size_t at = off;
      off = align(off + bytes);
      return at;
    };
    l.aps = take(sizeof(ApEntry) * c.max_aps);
    l.ap_idx = take(sizeof(MacIndex::Slot) * MacIndex::slots_for(c.max_aps));
    l.clients = take(sizeof(ClientEntry) * c.max_clients);
    l.cl_idx = take(sizeof(MacIndex::Slot) * MacIndex::slots_for(c.max_clients));
    l.anqp = take(sizeof(AnqpSlot) * c.max_anqp);
    l.gas = take(sizeof(GasTxn) * c.max_gas);
    l.gas_buf = take(static_cast<size_t>(c.gas_buf_bytes) * c.max_gas);
    l.order = take(sizeof(uint16_t) * (c.max_aps > c.max_clients ? c.max_aps : c.max_clients));
    l.scratch = take(sizeof(AnqpRecord));
    l.total = off;
    return l;
  }

  void clear_all() {
    for (uint16_t i = 0; i < cfg.max_aps; ++i) aps[i].used = false;
    for (uint16_t i = 0; i < cfg.max_clients; ++i) clients[i].used = false;
    for (uint16_t i = 0; i < cfg.max_anqp; ++i) anqp[i].state = SLOT_FREE;
    for (uint16_t i = 0; i < cfg.max_gas; ++i) gas[i].used = false;
    ap_index.clear();
    client_index.clear();
    n_aps = n_clients = n_pending = 0;
    st = Stats{};
  }

  // --- ANQP pool ----------------------------------------------------------------------------
  void free_slot(int idx) {
    AnqpSlot& s = anqp[idx];
    if (s.state == SLOT_ATTACHED) {
      const int ap = ap_index.find(s.owner);
      if (ap >= 0) {
        aps[ap].anqp = -1;
        aps[ap].info.has_anqp = false;
      }
    } else if (s.state == SLOT_PENDING) {
      --n_pending;
    }
    s.state = SLOT_FREE;
  }

  int alloc_slot() {
    int oldest = -1;
    for (uint16_t i = 0; i < cfg.max_anqp; ++i) {
      if (anqp[i].state == SLOT_FREE) return i;
      if (oldest < 0 || static_cast<int32_t>(anqp[i].touched_ms - anqp[oldest].touched_ms) < 0) oldest = i;
    }
    if (oldest >= 0) {
      free_slot(oldest);
      ++st.anqp_evictions;
    }
    return oldest;
  }

  int find_pending(const uint8_t* bssid) const {
    for (uint16_t i = 0; i < cfg.max_anqp; ++i) {
      if (anqp[i].state == SLOT_PENDING && mac_eq(anqp[i].owner, bssid)) return i;
    }
    return -1;
  }

  void store_anqp(int slot, const uint8_t* owner, uint8_t state, const AnqpRecord& rec, uint32_t now) {
    AnqpSlot& s = anqp[slot];
    if (s.state == SLOT_FREE) {
      s.rec.clear();
      s.rec.first_ms = now;
      std::memcpy(s.owner, owner, 6);
      if (state == SLOT_PENDING) ++n_pending;
    }
    s.state = state;
    merge_anqp(&s.rec, rec);
    s.rec.updated_ms = now;
    ++s.rec.responses;
    s.touched_ms = now;
  }

  void deliver_anqp(const uint8_t* bssid, const AnqpRecord& rec, uint32_t now) {
    ++st.anqp_completed;
    const int ap = ap_index.find(bssid);
    if (ap >= 0) {
      ApEntry& e = aps[ap];
      if (e.anqp < 0) {
        const int s = alloc_slot();
        if (s < 0) return;
        e.anqp = static_cast<int16_t>(s);
      }
      store_anqp(e.anqp, bssid, SLOT_ATTACHED, rec, now);
      e.info.has_anqp = true;
      e.info.anqp_updated_ms = now;
      return;
    }
    ++st.anqp_pending;
    int s = find_pending(bssid);
    if (s < 0) {
      if (n_pending >= cfg.max_pending) {
        int oldest = -1;
        for (uint16_t i = 0; i < cfg.max_anqp; ++i) {
          if (anqp[i].state != SLOT_PENDING) continue;
          if (oldest < 0 || static_cast<int32_t>(anqp[i].touched_ms - anqp[oldest].touched_ms) < 0) oldest = i;
        }
        if (oldest >= 0) {
          free_slot(oldest);
          ++st.pending_evictions;
        }
      }
      s = alloc_slot();
      if (s < 0) return;
    }
    store_anqp(s, bssid, SLOT_PENDING, rec, now);
  }

  // --- AP table -----------------------------------------------------------------------------
  template <typename Entry>
  static int lru_victim(const Entry* t, uint16_t cap) {
    int v = -1;
    for (uint16_t i = 0; i < cap; ++i) {
      if (!t[i].used) continue;
      if (v < 0 || static_cast<int32_t>(t[i].info.last_seen_ms - t[v].info.last_seen_ms) < 0) v = i;
    }
    return v;
  }

  void evict_ap(int i) {
    ApEntry& e = aps[i];
    if (e.anqp >= 0) {
      anqp[e.anqp].state = SLOT_FREE;
      e.anqp = -1;
    }
    ap_index.erase(e.info.bssid);
    e.used = false;
    --n_aps;
  }

  void evict_client(int i) {
    client_index.erase(clients[i].info.mac);
    clients[i].used = false;
    --n_clients;
  }

  int find_or_add_ap(const uint8_t* bssid, const RxMeta& m) {
    int i = ap_index.find(bssid);
    if (i >= 0) return i;
    if (n_aps >= cfg.max_aps) {
      const int v = lru_victim(aps, cfg.max_aps);
      if (v < 0) return -1;
      evict_ap(v);
      ++st.ap_evictions;
    }
    for (uint16_t k = 0; k < cfg.max_aps; ++k) {
      if (aps[k].used) continue;
      ApEntry& e = aps[k];
      std::memset(static_cast<void*>(&e.info), 0, sizeof(e.info));
      std::memcpy(e.info.bssid, bssid, 6);
      e.info.rssi_last = e.info.rssi_avg = e.info.rssi_peak = kNoRssi;
      e.info.first_seen_ms = m.now_ms;
      e.rssi_q4 = INT16_MIN;
      e.anqp = -1;
      e.used = true;
      ++n_aps;
      ap_index.insert(bssid, k);
      // A BSSID is not a client; drop any entry learned before its first beacon.
      const int c = client_index.find(bssid);
      if (c >= 0) evict_client(c);
      const int p = find_pending(bssid);
      if (p >= 0) {
        anqp[p].state = SLOT_ATTACHED;
        --n_pending;
        e.anqp = static_cast<int16_t>(p);
        e.info.has_anqp = true;
        e.info.anqp_updated_ms = anqp[p].rec.updated_ms;
      }
      return k;
    }
    return -1;
  }

  void apply_bss(const uint8_t* bssid, const BssObservation& o, bool beacon, const RxMeta& m) {
    const bool existed = ap_index.find(bssid) >= 0;
    const int i = find_or_add_ap(bssid, m);
    if (i < 0) return;
    ApInfo& a = aps[i].info;
    a.last_seen_ms = m.now_ms;
    a.band = m.band;
    a.rx_channel = m.channel;
    rssi_fold(m.rssi, &aps[i].rssi_q4, &a.rssi_last, &a.rssi_avg, &a.rssi_peak, cfg.rssi_avg_shift);
    if (beacon) {
      ++a.beacons;
    } else {
      ++a.probe_resps;
    }
    a.beacon_interval = o.interval;
    a.capability = o.capability;
    if (o.ssid_present && !o.hidden) {
      std::memcpy(a.ssid, o.ssid, o.ssid_len);
      a.ssid_len = o.ssid_len;
    }
    if (o.ssid_present && beacon) a.hidden = o.hidden;
    a.channel = resolve_channel(o, m);

    // Fields inferred from an element's absence only update from a fully walked frame.
    if (!o.complete && existed) return;
    a.width = resolve_width(o);
    a.phy = o.phy;
    a.security = resolve_security(o);
    a.suite_b = (o.akm_mask & kAkmSuiteB) != 0;
    a.akm_mask = o.akm_mask;
    a.wpa_ie = o.wpa;
    a.wpa_akm_mask = o.wpa_akm_mask;
    a.pairwise_ciphers = o.pairwise;
    a.group_cipher = o.group;
    a.pmf_capable = o.mfpc;
    a.pmf_required = o.mfpr;
    a.has_bss_load = o.bss_load;
    a.sta_count = o.sta_count;
    a.channel_util = o.ch_util;
    std::memcpy(a.country, o.country, 3);
    a.hs20 = o.hs20;
    a.hs20_release = o.hs20_release;
    a.hs20_dgaf_disabled = o.hs20_dgaf_disabled;
    a.interworking = o.interworking;
    a.access_network_type = o.ant;
    a.internet = o.internet;
    a.asra = o.asra;
    a.esr = o.esr;
    a.uesa = o.uesa;
    a.has_venue = o.has_venue;
    a.venue_group = o.venue_group;
    a.venue_type = o.venue_type;
    a.has_hessid = o.has_hessid;
    std::memcpy(a.hessid, o.hessid, 6);
    a.has_roaming_consortium = o.rc;
    a.rc_anqp_oi_count = o.rc_anqp_count;
    a.rc_oi_count = o.rc_n;
    std::memcpy(a.rc_ois, o.rc_ois, sizeof(a.rc_ois));
    a.passpoint = o.hs20;
  }

  // --- client table -------------------------------------------------------------------------
  // `tx`: the client transmitted this frame, so its RSSI is the client's and it proves presence.
  ClientEntry* touch_client(const uint8_t* mac, const RxMeta& m, bool tx) {
    if (mac_is_group(mac) || ap_index.find(mac) >= 0) return nullptr;
    int i = client_index.find(mac);
    if (i < 0) {
      if (n_clients >= cfg.max_clients) {
        const int v = lru_victim(clients, cfg.max_clients);
        if (v < 0) return nullptr;
        evict_client(v);
        ++st.client_evictions;
      }
      for (uint16_t k = 0; k < cfg.max_clients; ++k) {
        if (clients[k].used) continue;
        ClientEntry& e = clients[k];
        std::memset(static_cast<void*>(&e.info), 0, sizeof(e.info));
        std::memcpy(e.info.mac, mac, 6);
        e.info.randomized = mac_is_local(mac);
        e.info.rssi_last = e.info.rssi_avg = e.info.rssi_peak = kNoRssi;
        e.info.first_seen_ms = m.now_ms;
        e.rssi_q4 = INT16_MIN;
        e.used = true;
        ++n_clients;
        client_index.insert(mac, k);
        i = k;
        break;
      }
      if (i < 0) return nullptr;
    }
    ClientEntry& e = clients[i];
    e.info.last_seen_ms = m.now_ms;
    e.info.band = m.band;
    e.info.channel = m.channel;
    if (tx) {
      ++e.info.frames;
      rssi_fold(m.rssi, &e.rssi_q4, &e.info.rssi_last, &e.info.rssi_avg, &e.info.rssi_peak,
                cfg.rssi_avg_shift);
    }
    return &e;
  }

  static void associate(ClientEntry* c, const uint8_t* bssid) {
    if (!c || mac_is_group(bssid)) return;
    c->info.associated = true;
    std::memcpy(c->info.bssid, bssid, 6);
  }

  void on_probe_request(const uint8_t* sa, const uint8_t* ies, size_t n, const RxMeta& m) {
    ClientEntry* c = touch_client(sa, m, true);
    if (!c) return;
    IeWalker w(ies, n);
    uint8_t id, len;
    const uint8_t* d;
    while (w.next(&id, &d, &len)) {
      if (id != 0) continue;
      bool all_nul = true;
      for (size_t i = 0; i < len; ++i) all_nul = all_nul && d[i] == 0;
      if (all_nul) {
        ++c->info.wildcard_probes;
        return;
      }
      const uint8_t l = len > 32 ? 32 : len;
      for (uint8_t k = 0; k < c->info.n_probed; ++k) {
        if (c->info.probed_len[k] == l && std::memcmp(c->info.probed[k], d, l) == 0) return;
      }
      if (c->info.n_probed < 8) {
        std::memcpy(c->info.probed[c->info.n_probed], d, l);
        c->info.probed_len[c->info.n_probed++] = l;
      }
      return;
    }
  }

  // --- GAS / ANQP ---------------------------------------------------------------------------
  int find_gas(const uint8_t* sa, uint8_t tok) const {
    for (uint16_t i = 0; i < cfg.max_gas; ++i) {
      if (gas[i].used && gas[i].token == tok && mac_eq(gas[i].sa, sa)) return i;
    }
    return -1;
  }

  int open_gas(const uint8_t* sa, uint8_t tok, const uint8_t* bssid, uint32_t now) {
    int slot = find_gas(sa, tok);
    if (slot < 0) {
      for (uint16_t i = 0; i < cfg.max_gas && slot < 0; ++i) {
        if (!gas[i].used) slot = i;
      }
    }
    if (slot < 0) {
      for (uint16_t i = 0; i < cfg.max_gas; ++i) {
        if (slot < 0 || static_cast<int32_t>(gas[i].touched_ms - gas[slot].touched_ms) < 0) slot = i;
      }
      if (slot < 0) return -1;
      ++st.gas_evictions;
    }
    GasTxn& t = gas[slot];
    t.used = true;
    std::memcpy(t.sa, sa, 6);
    t.token = tok;
    std::memcpy(t.bssid, bssid, 6);
    t.next_frag = 0;
    t.len = 0;
    t.touched_ms = now;
    return slot;
  }

  void complete(const uint8_t* bssid, const uint8_t* payload, size_t n, uint32_t now) {
    decode_anqp_elements(payload, n, scratch);
    deliver_anqp(bssid, *scratch, now);
  }

  void on_gas(const uint8_t* f, size_t len, size_t hdr, const RxMeta& m) {
    const uint8_t* b = f + hdr;
    const size_t n = len - hdr;
    const uint8_t action = b[1];
    const uint8_t tok = b[2];
    if (action != 11 && action != 13) return;  // requests carry nothing to attribute
    size_t pos = 3;
    if (pos + 2 > n) {
      ++st.malformed;
      return;
    }
    const uint16_t status = le16(b + pos);
    pos += 2;
    uint8_t frag = 0;
    bool more = false;
    if (action == 13) {
      if (pos + 1 > n) {
        ++st.malformed;
        return;
      }
      frag = b[pos] & 0x7f;
      more = (b[pos] & 0x80) != 0;
      pos += 1;
    }
    pos += 2;  // comeback delay
    if (pos + 2 > n || b[pos] != 108) {
      ++st.malformed;
      return;
    }
    const size_t adv_len = b[pos + 1];
    if (adv_len < 2 || pos + 2 + adv_len > n) {
      ++st.malformed;
      return;
    }
    if (b[pos + 3] != 0) {  // first tuple's protocol ID; 0 = ANQP
      ++st.gas_non_anqp;
      return;
    }
    pos += 2 + adv_len;
    if (pos + 2 > n) {
      ++st.malformed;
      return;
    }
    size_t resp_len = le16(b + pos);
    pos += 2;
    if (pos + resp_len > n) {
      ++st.malformed;  // clipped, as core does; reassembly will usually reject it anyway
      resp_len = n - pos;
    }
    const uint8_t* payload = b + pos;
    ++st.gas_responses;

    const uint8_t* sa = f + 10;
    const uint8_t* a3 = f + 16;
    // Same attribution as core: a wildcard (or zero) address3 means the transmitter is the AP.
    const uint8_t* bssid = (detail::mac_is_broadcast(a3) || detail::mac_is_zero(a3)) ? sa : a3;
    const bool retry = (f[1] & 0x08) != 0;

    if (action == 11) {
      const int old = find_gas(sa, tok);
      if (status != 0) {
        if (old >= 0) gas[old].used = false;
        ++st.gas_dropped;
        return;
      }
      if (resp_len > 0) {
        if (old >= 0) gas[old].used = false;
        complete(bssid, payload, resp_len, m.now_ms);
      } else {
        open_gas(sa, tok, bssid, m.now_ms);  // answer follows in comeback fragments
      }
      return;
    }

    int t = find_gas(sa, tok);
    if (status != 0) {
      // 95: the AP is still waiting on its server; the requester will come back again.
      if (status != 95 && t >= 0) {
        gas[t].used = false;
        ++st.gas_dropped;
      }
      return;
    }
    if (t >= 0 && frag == 0 && gas[t].next_frag > 0 && !retry) t = -1;  // token reused: restart
    if (t < 0) {
      if (frag != 0) {
        ++st.gas_dropped;  // joined mid-transaction
        return;
      }
      t = open_gas(sa, tok, bssid, m.now_ms);
      if (t < 0) return;
    }
    GasTxn& g = gas[t];
    g.touched_ms = m.now_ms;
    if (frag < g.next_frag) return;  // retransmission of a fragment we hold
    if (frag > g.next_frag || g.len + resp_len > cfg.gas_buf_bytes) {
      g.used = false;
      ++st.gas_dropped;
      return;
    }
    std::memcpy(g.buf + g.len, payload, resp_len);
    g.len = static_cast<uint16_t>(g.len + resp_len);
    ++g.next_frag;
    if (!more) {
      g.used = false;
      complete(g.bssid, g.buf, g.len, m.now_ms);
    }
  }

  // --- frame dispatch -----------------------------------------------------------------------
  void ingest(const uint8_t* f, size_t len, const RxMeta& m) {
    BssObservation obs;
    bool is_bss = false;
    size_t hdr = 0;
    uint8_t type = 0, sub = 0;
    if (f != nullptr && len >= 10) {
      const uint16_t fc = le16(f);
      type = (fc >> 2) & 0x3;
      sub = (fc >> 4) & 0xf;
      hdr = (fc & 0x8000) ? 28 : 24;  // +HTC
      // Parse a beacon's elements before taking the lock; it is most of the per-frame work.
      if ((fc & 0x3) == 0 && type == 0 && (sub == 8 || sub == 5) && len >= hdr + 12) {
        const uint8_t* fixed = f + hdr;
        obs.interval = le16(fixed + 8);
        obs.capability = le16(fixed + 10);
        parse_bss_ies(obs, fixed + 12, len - hdr - 12);
        is_bss = true;
      }
    }

    Guard g(lock);
    ++st.frames;
    st.last_ingest_ms = m.now_ms;
    if (f == nullptr || len < 10 || (le16(f) & 0x3) != 0) {
      ++st.malformed;
      return;
    }
    ++st.by_type[type][sub];
    const uint16_t fc = le16(f);
    const bool to_ds = (fc & 0x0100) != 0, from_ds = (fc & 0x0200) != 0;
    const uint8_t* a1 = f + 4;
    const uint8_t* a2 = f + 10;

    if (type == 0) {
      if (len < hdr) {
        ++st.malformed;
        return;
      }
      const uint8_t* a3 = f + 16;
      switch (sub) {
        case 8:
        case 5:
          if (!is_bss || mac_is_group(a3)) {
            ++st.malformed;
            return;
          }
          if (!obs.complete) ++st.malformed;
          apply_bss(a3, obs, sub == 8, m);
          return;
        case 4:
          on_probe_request(a2, f + hdr, len - hdr, m);
          return;
        case 0:
        case 2:
          associate(touch_client(a2, m, true), a1);
          return;
        case 1:
        case 3:
          if (len >= hdr + 4 && le16(f + hdr + 2) == 0) associate(touch_client(a1, m, false), a2);
          return;
        case 11:
          if (mac_eq(a2, a3)) {
            associate(touch_client(a1, m, false), a3);  // from the AP
          } else {
            associate(touch_client(a2, m, true), a1);
          }
          return;
        case 13:
          if ((fc & 0x4000) == 0 && len >= hdr + 3 && f[hdr] == 4) on_gas(f, len, hdr, m);
          return;
        default:
          return;
      }
    }
    if (type == 1) {
      if (sub == 10) {  // PS-Poll: addr1 BSSID, addr2 station
        if (len < 16) {
          ++st.malformed;
          return;
        }
        ClientEntry* c = touch_client(a2, m, true);
        associate(c, a1);
        if (c) c->info.power_save = true;
      }
      return;
    }
    if (type == 2) {
      if (len < 24) {
        ++st.malformed;
        return;
      }
      ClientEntry* c = nullptr;
      if (to_ds && !from_ds) {
        c = touch_client(a2, m, true);
        associate(c, a1);
        if (c && (sub == 4 || sub == 12)) c->info.power_save = true;  // Null / QoS Null
      } else if (from_ds && !to_ds) {
        associate(touch_client(a1, m, false), a2);
      }
    }
  }

  // --- snapshots ----------------------------------------------------------------------------
  bool ap_less(ApSort s, const ApInfo& a, const ApInfo& b) const {
    switch (s) {
      case ApSort::Rssi:
        if (a.rssi_avg != b.rssi_avg) return a.rssi_avg > b.rssi_avg;
        break;
      case ApSort::Ssid: {
        const bool ah = a.ssid_len == 0, bh = b.ssid_len == 0;
        if (ah != bh) return bh;  // unnamed last
        const int c = ascii_casecmp(a.ssid, a.ssid_len, b.ssid, b.ssid_len);
        if (c != 0) return c < 0;
        if (a.rssi_avg != b.rssi_avg) return a.rssi_avg > b.rssi_avg;
        break;
      }
      case ApSort::Channel:
        if (a.band != b.band) return a.band < b.band;
        if (a.channel != b.channel) return a.channel < b.channel;
        if (a.rssi_avg != b.rssi_avg) return a.rssi_avg > b.rssi_avg;
        break;
      case ApSort::LastSeen:
        if (a.last_seen_ms != b.last_seen_ms) return static_cast<int32_t>(a.last_seen_ms - b.last_seen_ms) > 0;
        break;
    }
    return std::memcmp(a.bssid, b.bssid, 6) < 0;
  }

  bool client_less(ClientSort s, const ClientInfo& a, const ClientInfo& b) const {
    switch (s) {
      case ClientSort::Rssi:
        if (a.rssi_avg != b.rssi_avg) return a.rssi_avg > b.rssi_avg;
        break;
      case ClientSort::LastSeen:
        if (a.last_seen_ms != b.last_seen_ms) return static_cast<int32_t>(a.last_seen_ms - b.last_seen_ms) > 0;
        break;
      case ClientSort::Frames:
        if (a.frames != b.frames) return a.frames > b.frames;
        break;
      case ClientSort::Mac:
        break;
    }
    return std::memcmp(a.mac, b.mac, 6) < 0;
  }
};

// ---------------------------------------------------------------------------------------------

Survey::~Survey() {
  if (!impl_) return;
  void (*dealloc)(void*) = impl_->cfg.dealloc ? impl_->cfg.dealloc : std::free;
  void* block = impl_->block;
  impl_->~Impl();
  dealloc(block);
}

size_t Survey::memory_required(const Config& cfg) {
  return Impl::align(sizeof(Impl)) + Impl::layout(cfg).total;
}

bool Survey::init(const Config& cfg) {
  if (impl_ || cfg.max_aps == 0 || cfg.max_clients == 0 || cfg.max_anqp == 0 || cfg.max_gas == 0 ||
      cfg.max_aps >= MacIndex::kEmpty || cfg.max_clients >= MacIndex::kEmpty ||
      cfg.rssi_avg_shift > 8) {
    return false;
  }
  void* (*alloc)(size_t) = cfg.alloc ? cfg.alloc : std::malloc;
  const size_t head = Impl::align(sizeof(Impl));
  const Impl::Layout l = Impl::layout(cfg);
  uint8_t* block = static_cast<uint8_t*>(alloc(head + l.total));
  if (!block) return false;
  std::memset(block, 0, head + l.total);
  Impl* im = new (block) Impl();
  uint8_t* base = block + head;
  im->cfg = cfg;
  im->block = block;
  im->lock = cfg.lock ? cfg.lock : &im->null_lock;
  im->aps = reinterpret_cast<Impl::ApEntry*>(base + l.aps);
  im->ap_index.attach(reinterpret_cast<MacIndex::Slot*>(base + l.ap_idx), MacIndex::slots_for(cfg.max_aps));
  im->clients = reinterpret_cast<Impl::ClientEntry*>(base + l.clients);
  im->client_index.attach(reinterpret_cast<MacIndex::Slot*>(base + l.cl_idx),
                          MacIndex::slots_for(cfg.max_clients));
  im->anqp = reinterpret_cast<Impl::AnqpSlot*>(base + l.anqp);
  im->gas = reinterpret_cast<Impl::GasTxn*>(base + l.gas);
  for (uint16_t i = 0; i < cfg.max_gas; ++i) {
    im->gas[i].buf = base + l.gas_buf + static_cast<size_t>(i) * cfg.gas_buf_bytes;
  }
  im->order = reinterpret_cast<uint16_t*>(base + l.order);
  im->scratch = reinterpret_cast<AnqpRecord*>(base + l.scratch);
  im->clear_all();
  impl_ = im;
  return true;
}

void Survey::ingest(const uint8_t* frame, size_t len, const RxMeta& meta) {
  if (impl_) impl_->ingest(frame, len, meta);
}

size_t Survey::aps(ApInfo* out, size_t max, ApSort sort, uint8_t band_mask, bool passpoint_only,
                   size_t* total, bool include_interworking) const {
  if (total) *total = 0;
  if (!impl_) return 0;
  Impl& im = *impl_;
  Impl::Guard g(im.lock);
  size_t n = 0;
  for (uint16_t i = 0; i < im.cfg.max_aps; ++i) {
    const Impl::ApEntry& e = im.aps[i];
    if (!e.used || !(band_bit(e.info.band) & band_mask)) continue;
    if (passpoint_only && !e.info.hs20 && !(include_interworking && e.info.interworking)) continue;
    im.order[n++] = i;
  }
  std::sort(im.order, im.order + n, [&](uint16_t a, uint16_t b) {
    return im.ap_less(sort, im.aps[a].info, im.aps[b].info);
  });
  const size_t k = n < max ? n : max;
  for (size_t i = 0; i < k; ++i) out[i] = im.aps[im.order[i]].info;
  if (total) *total = n;
  return k;
}

bool Survey::ap_detail(const uint8_t bssid[6], ApDetail* out) const {
  if (!impl_) return false;
  Impl& im = *impl_;
  Impl::Guard g(im.lock);
  const int i = im.ap_index.find(bssid);
  if (i < 0) return false;
  out->ap = im.aps[i].info;
  out->has_anqp = im.aps[i].anqp >= 0;
  if (out->has_anqp) {
    out->anqp = im.anqp[im.aps[i].anqp].rec;
  } else {
    out->anqp.clear();
  }
  return true;
}

bool Survey::anqp_for(const uint8_t bssid[6], AnqpRecord* out) const {
  if (!impl_) return false;
  Impl& im = *impl_;
  Impl::Guard g(im.lock);
  const int i = im.ap_index.find(bssid);
  int slot = i >= 0 ? im.aps[i].anqp : im.find_pending(bssid);
  if (slot < 0) return false;
  *out = im.anqp[slot].rec;
  return true;
}

size_t Survey::pending_bssids(uint8_t (*out)[6], size_t max) const {
  if (!impl_) return 0;
  Impl& im = *impl_;
  Impl::Guard g(im.lock);
  size_t n = 0;
  for (uint16_t i = 0; i < im.cfg.max_anqp && n < max; ++i) {
    if (im.anqp[i].state == Impl::SLOT_PENDING) std::memcpy(out[n++], im.anqp[i].owner, 6);
  }
  return n;
}

size_t Survey::clients(ClientInfo* out, size_t max, ClientSort sort, const uint8_t* associated_to,
                       size_t* total) const {
  if (total) *total = 0;
  if (!impl_) return 0;
  Impl& im = *impl_;
  Impl::Guard g(im.lock);
  size_t n = 0;
  for (uint16_t i = 0; i < im.cfg.max_clients; ++i) {
    const Impl::ClientEntry& e = im.clients[i];
    if (!e.used) continue;
    if (associated_to && !(e.info.associated && mac_eq(e.info.bssid, associated_to))) continue;
    im.order[n++] = i;
  }
  std::sort(im.order, im.order + n, [&](uint16_t a, uint16_t b) {
    return im.client_less(sort, im.clients[a].info, im.clients[b].info);
  });
  const size_t k = n < max ? n : max;
  for (size_t i = 0; i < k; ++i) out[i] = im.clients[im.order[i]].info;
  if (total) *total = n;
  return k;
}

Stats Survey::stats() const {
  if (!impl_) return Stats{};
  Impl::Guard g(impl_->lock);
  Stats s = impl_->st;
  s.aps = impl_->n_aps;
  s.clients = impl_->n_clients;
  s.pending = impl_->n_pending;
  return s;
}

void Survey::reset() {
  if (!impl_) return;
  Impl::Guard g(impl_->lock);
  impl_->clear_all();
}

}  // namespace wlc
