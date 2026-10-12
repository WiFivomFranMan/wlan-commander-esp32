// SPDX-License-Identifier: MIT
//
// Mutation fuzzer for wlc_survey under ASan/UBSan. Seeds are hand-built frames covering every
// parser path plus every frame of the pcaps given on the command line; mutations flip bits,
// truncate, splice and corrupt length octets. Two surveys run side by side — default sizes and a
// tiny configuration that evicts on almost every frame — and invariants are checked as it goes.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "harness.h"
#include "pcap_reader.h"

using namespace t;

namespace {

uint64_t g_rng = 0x9e3779b97f4a7c15ull;
uint32_t rnd() {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return uint32_t(g_rng >> 16);
}
uint32_t rnd(uint32_t n) { return n ? rnd() % n : 0; }

std::vector<Bytes> builtin_seeds() {
  std::vector<Bytes> s;
  Bytes ies = ie(0, "seed");
  put(ies, ie(3, Bytes{6}));
  put(ies, ie(7, Bytes{'U', 'S', 0x20}));
  put(ies, ie(11, Bytes{1, 0, 50, 0, 0}));
  put(ies, ie(45, Bytes(26, 0xff)));
  Bytes ht(22, 0);
  ht[0] = 6;
  ht[1] = 0x05;
  put(ies, ie(61, ht));
  put(ies, rsn({2, 8, 1, 12, 18, 24}, 0x00c0));
  put(ies, ie(191, Bytes(12, 0)));
  put(ies, ie(192, Bytes{1, 42, 50, 0, 0}));
  put(ies, ext_ie(35, Bytes(20, 0)));
  put(ies, ext_ie(36, Bytes{0, 0x40 | 0x80, 0x02, 1, 0, 0, 1, 42, 0, 7, 37, 3, 7, 15, 6}));
  put(ies, ext_ie(106, Bytes{1, 0, 0, 0, 0, 4, 7, 15}));
  put(ies, ext_ie(108, Bytes(12, 0)));
  put(ies, ie(107, Bytes{0x12, 2, 8, 2, 0, 0, 0, 0, 1}));
  put(ies, ie(111, Bytes{3, 0x53, 0x5a, 0x03, 0xba, 0x00, 0x00, 1, 2, 3, 4, 5, 6, 7}));
  put(ies, ie(221, Bytes{0x50, 0x6f, 0x9a, 0x10, 0x23, 1, 0, 2, 0}));
  put(ies, ie(221, Bytes{0x00, 0x50, 0xf2, 0x01, 1, 0, 0, 0x50, 0xf2, 2, 1, 0, 0, 0x50, 0xf2, 2, 1, 0,
                         0, 0x50, 0xf2, 2}));
  s.push_back(bss_frame(ap(1), ies));
  s.push_back(bss_frame(ap(2), ies, 0x411, 5));

  Bytes anqp;
  Bytes venue = {2, 8, 8, 'e', 'n', 'g', 'H', 'o', 't', 'e', 'l'};
  put(anqp, anqp_el(258, venue));
  put(anqp, anqp_el(260, Bytes{0, 3, 0, 'a', 'b', 'c', 1, 0, 0}));
  put(anqp, anqp_el(261, Bytes{3, 0, 0x1b, 0xc5}));
  put(anqp, anqp_el(262, Bytes{0x0d}));
  put(anqp, anqp_el(263, Bytes{1, 0, 16, 0, 0, 5, 'a', '.', 'o', 'r', 'g', 1, 8, 21, 2, 2, 1, 4, 5, 1, 7}));
  put(anqp, anqp_el(264, Bytes{0, 6, 0, 4, 1, 0x13, 0x00, 0x14}));
  put(anqp, anqp_el(268, Bytes{6, 'r', 'r', '.', 'c', 'o', 'm'}));
  put(anqp, anqp_el(259, Bytes{3, '9', '1', '1'}));
  put(anqp, anqp_el(265, Bytes(18, 0x11)));
  put(anqp, anqp_el(266, Bytes{0, 9, 'U', 'S', 1, 2, 'T', 'X', 3, 1, 'A'}));
  put(anqp, anqp_el(267, Bytes{4, 'h', 'e', 'l', 'd'}));
  put(anqp, anqp_el(269, Bytes{'u', 'r', 'n'}));
  put(anqp, anqp_el(271, Bytes{3, 's', 'o', 's'}));
  put(anqp, anqp_el(277, Bytes{4, 1, 'u', 'r', 'l'}));
  put(anqp, anqp_el(257, Bytes{2, 1, 0xdd, 0xdd, 2, 0, 1, 2, 8, 1}));
  for (uint8_t st = 1; st <= 13; ++st) put(anqp, hs20_el(st, Bytes{4, 'o', 's', 'u', '1', 1, 9, 0, 6, 0, 4, 'e', 'n', 'g', 'X', 0, 0}));
  s.push_back(gas_initial(ap(1), sta(1), ap(1), 1, anqp));
  s.push_back(gas_initial(ap(3), sta(1), kBroadcast, 2, anqp));
  s.push_back(gas_initial(ap(1), sta(1), ap(1), 3, Bytes{}, 0, 10));
  for (uint8_t frag = 0; frag < 3; ++frag) {
    const size_t a = anqp.size() * frag / 3, b = anqp.size() * (frag + 1) / 3;
    s.push_back(gas_comeback(ap(1), sta(1), ap(1), 3, frag, frag < 2, Bytes(anqp.begin() + long(a), anqp.begin() + long(b))));
  }
  Bytes probe = hdr(0, 4, kBroadcast, sta(2), kBroadcast);
  put(probe, ie(0, "probe"));
  s.push_back(probe);
  s.push_back(hdr(2, 8, ap(1), sta(3), ap(1), 0x11));
  s.push_back(hdr(2, 0, sta(4), ap(1), ap(1), 0x02));
  Bytes pspoll = {0xa4, 0, 1, 0xc0};
  const Mac b1 = ap(1), s5 = sta(5);
  pspoll.insert(pspoll.end(), b1.b, b1.b + 6);
  pspoll.insert(pspoll.end(), s5.b, s5.b + 6);
  s.push_back(pspoll);
  Bytes areq = hdr(0, 0, ap(1), sta(6), ap(1));
  put(areq, {0, 0, 0, 0});
  s.push_back(areq);
  Bytes aresp = hdr(0, 3, sta(7), ap(1), ap(1));
  put(aresp, {0, 0, 0, 0, 1, 0});
  s.push_back(aresp);
  Bytes auth = hdr(0, 11, ap(1), sta(8), ap(1));
  put(auth, {0, 0, 1, 0, 0, 0});
  s.push_back(auth);
  return s;
}

Bytes mutate(const std::vector<Bytes>& seeds) {
  const uint32_t mode = rnd(10);
  if (mode == 0) {  // pure noise, biased toward a plausible frame control
    Bytes b(rnd(400));
    for (auto& x : b) x = uint8_t(rnd());
    if (!b.empty() && rnd(2)) b[0] = uint8_t(rnd(2) ? 0x80 : 0xd0);
    return b;
  }
  Bytes b = seeds[rnd(uint32_t(seeds.size()))];
  const uint32_t n = 1 + rnd(4);
  for (uint32_t k = 0; k < n && !b.empty(); ++k) {
    switch (rnd(7)) {
      case 0:
        b[rnd(uint32_t(b.size()))] ^= uint8_t(1u << rnd(8));
        break;
      case 1:
        b[rnd(uint32_t(b.size()))] = uint8_t(rnd());
        break;
      case 2:
        b.resize(rnd(uint32_t(b.size()) + 1));
        break;
      case 3: {  // length-octet style corruption: small or extreme values
        static const uint8_t vals[] = {0, 1, 2, 3, 4, 0x7f, 0x80, 0xfe, 0xff};
        b[rnd(uint32_t(b.size()))] = vals[rnd(sizeof(vals))];
        break;
      }
      case 4: {  // splice the tail of another seed
        const Bytes& o = seeds[rnd(uint32_t(seeds.size()))];
        const size_t at = rnd(uint32_t(b.size()));
        const size_t from = rnd(uint32_t(o.size()));
        b.resize(at);
        b.insert(b.end(), o.begin() + long(from), o.end());
        break;
      }
      case 5: {
        const size_t extra = rnd(64);
        for (size_t i = 0; i < extra; ++i) b.push_back(uint8_t(rnd()));
        break;
      }
      case 6: {  // duplicate a slice (repeated elements)
        const size_t a = rnd(uint32_t(b.size())), l = rnd(uint32_t(b.size() - a) + 1);
        Bytes slice(b.begin() + long(a), b.begin() + long(a + l));
        b.insert(b.begin() + long(rnd(uint32_t(b.size()))), slice.begin(), slice.end());
        break;
      }
    }
  }
  return b;
}

int g_violations = 0;
void expect(bool ok, const char* what) {
  if (!ok && g_violations++ < 20) std::fprintf(stderr, "invariant violated: %s\n", what);
}

template <size_t N>
void check_text(const wlc::Text<N>& tx) {
  expect(tx.len < N && std::strlen(tx.s) == tx.len, "Text length");
}

void check_record(const wlc::AnqpRecord& r) {
  expect(r.n_domains <= 8 && r.n_nai_realms <= 8 && r.n_plmns <= 8 && r.n_roaming_ois <= 8, "anqp counts");
  expect(r.n_venue_names <= 4 && r.n_operator_names <= 4 && r.n_unknown <= 8 && r.n_conn_caps <= 16, "anqp counts 2");
  for (uint8_t i = 0; i < r.n_domains && i < 8; ++i) check_text(r.domains[i]);
  for (uint8_t i = 0; i < r.n_nai_realms && i < 8; ++i) {
    check_text(r.nai_realms[i].realm);
    expect(r.nai_realms[i].n_eap <= 4, "eap count");
  }
  for (uint8_t i = 0; i < r.n_emergency_numbers && i < 4; ++i) check_text(r.emergency_numbers[i]);
  check_text(r.civic_summary);
  check_text(r.geo_summary);
  check_text(r.emergency_nai);
  expect(r.geo.len <= sizeof(r.geo.raw) && r.civic.len <= sizeof(r.civic.raw), "raw blob bounds");
}

void check_survey(wlc::Survey& s, const wlc::Config& cfg) {
  static wlc::ApInfo aps[1024];
  static wlc::ClientInfo cl[1024];
  static wlc::ApDetail d;
  size_t total = 0;
  const auto sorts = {wlc::ApSort::Rssi, wlc::ApSort::Ssid, wlc::ApSort::Channel, wlc::ApSort::LastSeen};
  const auto sort = *(sorts.begin() + rnd(4));
  const size_t n = s.aps(aps, 1024, sort, uint8_t(rnd(8)), rnd(2) != 0, &total);
  expect(n == total && n <= cfg.max_aps, "ap count");
  for (size_t i = 0; i < n; ++i) {
    expect(aps[i].ssid_len <= 32 && aps[i].rc_oi_count <= 3, "ap fields");
    expect(uint8_t(aps[i].band) <= 2 && uint8_t(aps[i].width) <= 4, "ap enums");
    if (s.ap_detail(aps[i].bssid, &d)) {
      if (d.has_anqp) check_record(d.anqp);
    } else {
      expect(false, "listed AP has no detail");
    }
  }
  const uint8_t* filt = rnd(4) == 0 && n ? aps[0].bssid : nullptr;
  const size_t c = s.clients(cl, 1024, wlc::ClientSort(rnd(4)), filt, &total);
  expect(c == total && c <= cfg.max_clients, "client count");
  for (size_t i = 0; i < c; ++i) expect(cl[i].n_probed <= 8, "probed count");
  uint8_t pend[64][6];
  const size_t p = s.pending_bssids(pend, 64);
  const auto st = s.stats();
  expect(p == st.pending && st.pending <= cfg.max_pending, "pending count");
  s.aps(aps, 1024, wlc::ApSort::Rssi, wlc::BAND_ALL, false, &total);
  expect(st.aps == total && (filt || st.clients == c), "stats agree with snapshots");
}

}  // namespace

int main(int argc, char** argv) {
  const long iters = argc > 1 ? std::atol(argv[1]) : 200000;
  std::vector<Bytes> seeds = builtin_seeds();
  size_t from_pcap = 0;
  for (int i = 2; i < argc; ++i) {
    pcap::Reader r;
    std::string err;
    if (!r.open(argv[i], &err)) {
      std::fprintf(stderr, "%s: %s\n", argv[i], err.c_str());
      return 2;
    }
    pcap::Packet p;
    while (r.next(&p)) {
      seeds.emplace_back(p.frame, p.frame + p.len);
      ++from_pcap;
    }
  }

  wlc::Config big;
  wlc::Config tiny;
  tiny.max_aps = 4;
  tiny.max_clients = 4;
  tiny.max_anqp = 2;
  tiny.max_pending = 1;
  tiny.max_gas = 1;
  tiny.gas_buf_bytes = 64;
  wlc::Survey a, b;
  if (!a.init(big) || !b.init(tiny)) return 2;
  auto rec = std::make_unique<wlc::AnqpRecord>();

  for (long i = 0; i < iters; ++i) {
    const Bytes f = mutate(seeds);
    wlc::RxMeta m;
    m.rssi = int8_t(rnd(2) ? -int(rnd(100)) : int(rnd(256)) - 128);
    m.channel = uint8_t(rnd());
    m.band = wlc::Band(rnd(3));
    m.now_ms = uint32_t(i * 7 + rnd(5));
    if (rnd(1000) == 0) m.now_ms = uint32_t(0xffffff00u + rnd(512));  // clock wrap
    a.ingest(f.data(), f.size(), m);
    b.ingest(f.data(), f.size(), m);
    // The ANQP decoder on its own, starting at every plausible offset.
    if (f.size() > 24) {
      const size_t off = 24 + rnd(uint32_t(f.size() - 24));
      wlc::decode_anqp_elements(f.data() + off, f.size() - off, rec.get());
      check_record(*rec);
    }
    if (i % 2000 == 0) {
      check_survey(a, big);
      check_survey(b, tiny);
    }
    if (rnd(50000) == 0) b.reset();
  }
  check_survey(a, big);
  check_survey(b, tiny);
  const auto sa = a.stats(), sb = b.stats();
  std::printf("fuzz: %ld frames (%zu seeds, %zu from pcaps); default: %u APs %u clients %u malformed "
              "%u ANQP; tiny: %u AP evictions %u client evictions %u ANQP evictions; %d invariant "
              "violations\n",
              iters, seeds.size(), from_pcap, sa.aps, sa.clients, sa.malformed, sa.anqp_completed,
              sb.ap_evictions, sb.client_evictions, sb.anqp_evictions, g_violations);
  return g_violations ? 1 : 0;
}
