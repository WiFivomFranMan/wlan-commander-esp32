// SPDX-License-Identifier: MIT
#include "harness.h"

using namespace t;

namespace {

wlc::Survey& fresh(wlc::Config cfg = wlc::Config()) {
  static wlc::Survey* s = nullptr;
  delete s;
  s = new wlc::Survey();
  if (!s->init(cfg)) std::abort();
  return *s;
}

std::vector<wlc::ClientInfo> all(wlc::Survey& s, wlc::ClientSort sort = wlc::ClientSort::Mac,
                                 const uint8_t* assoc = nullptr) {
  std::vector<wlc::ClientInfo> v(64);
  v.resize(s.clients(v.data(), v.size(), sort, assoc));
  return v;
}

const wlc::ClientInfo* find(const std::vector<wlc::ClientInfo>& v, const Mac& m) {
  for (const auto& c : v) {
    if (std::memcmp(c.mac, m.b, 6) == 0) return &c;
  }
  return nullptr;
}

Bytes probe_req(const Mac& sa, const Bytes& ssid) {
  Bytes f = hdr(0, 4, kBroadcast, sa, kBroadcast);
  put(f, ie(0, ssid));
  put(f, ie(1, Bytes{0x82, 0x84}));
  return f;
}

Bytes data(const Mac& a1, const Mac& a2, const Mac& a3, uint8_t ds, uint8_t subtype = 0) {
  return hdr(2, subtype, a1, a2, a3, ds);
}

}  // namespace

TEST(probe_requests_collect_ssids) {
  auto& s = fresh();
  const Mac rnd = mac(0xda, 0xa1, 0x19, 0x00, 0x00, 0x01);
  feed(s, probe_req(rnd, Bytes{}), meta(1, -70));
  feed(s, probe_req(rnd, Bytes{}), meta(2, -60));
  for (int i = 0; i < 10; ++i) feed(s, probe_req(rnd, Bytes{'n', uint8_t('0' + i)}), meta(3 + i, -65));
  feed(s, probe_req(rnd, Bytes{'n', '0'}), meta(20, -65));  // duplicate
  auto v = all(s);
  CHECK_EQ(v.size(), 1u);
  const auto& c = v[0];
  CHECK(c.randomized);
  CHECK_EQ(c.wildcard_probes, 2u);
  CHECK_EQ(c.n_probed, 8);  // bounded
  CHECK_EQ(c.probed_len[0], 2);
  CHECK(std::memcmp(c.probed[0], "n0", 2) == 0);
  CHECK_EQ(c.frames, 13u);
  CHECK_EQ(c.rssi_peak, -60);
  CHECK_EQ(c.first_seen_ms, 1u);
  CHECK_EQ(c.last_seen_ms, 20u);
  CHECK(!c.associated);

  feed(s, probe_req(sta(1), Bytes{'x'}), meta(30));
  CHECK(!find(all(s), sta(1))->randomized);
}

TEST(data_frames_learn_association) {
  auto& s = fresh();
  // ToDS: addr1 BSSID, addr2 station.
  feed(s, data(ap(1), sta(1), mac(0x00, 0x99, 0, 0, 0, 1), 0x01), meta(10, -40, 36, wlc::Band::B5G));
  // FromDS: addr1 station, addr2 BSSID. RSSI is the AP's and must not touch the station.
  feed(s, data(sta(2), ap(1), mac(0x00, 0x99, 0, 0, 0, 1), 0x02), meta(11, -80));
  // QoS Null from a station: power save.
  feed(s, data(ap(2), sta(3), ap(2), 0x01 | 0x10, 12), meta(12, -55));
  // Null (subtype 4) too.
  feed(s, data(ap(2), sta(4), ap(2), 0x01, 4), meta(12, -56));
  // WDS and IBSS forms are ignored.
  feed(s, data(sta(5), sta(6), sta(7), 0x03), meta(13));
  feed(s, data(sta(5), sta(6), ap(9), 0x00), meta(13));
  auto v = all(s);
  CHECK_EQ(v.size(), 4u);
  const auto* c1 = find(v, sta(1));
  CHECK(c1 && c1->associated);
  CHECK_STR(macs(c1->bssid), "00:11:22:33:44:01");
  CHECK_EQ(c1->rssi_last, -40);
  CHECK(c1->band == wlc::Band::B5G);
  CHECK_EQ(c1->channel, 36);
  CHECK_EQ(c1->frames, 1u);
  const auto* c2 = find(v, sta(2));
  CHECK(c2 && c2->associated);
  CHECK_EQ(c2->rssi_last, -128);
  CHECK_EQ(c2->frames, 0u);
  CHECK(find(v, sta(3))->power_save);
  CHECK(find(v, sta(4))->power_save);
  CHECK(!c1->power_save);

  auto by_ap1 = all(s, wlc::ClientSort::Mac, ap(1).b);
  CHECK_EQ(by_ap1.size(), 2u);
  auto by_ap2 = all(s, wlc::ClientSort::Mac, ap(2).b);
  CHECK_EQ(by_ap2.size(), 2u);
}

TEST(ps_poll_marks_power_save) {
  auto& s = fresh();
  const Mac bssid = ap(3), station = sta(8);
  Bytes f = {0xa4, 0x00, 0x01, 0xc0};
  f.insert(f.end(), bssid.b, bssid.b + 6);
  f.insert(f.end(), station.b, station.b + 6);
  feed(s, f, meta(1, -61));
  auto v = all(s);
  CHECK_EQ(v.size(), 1u);
  CHECK(v[0].power_save && v[0].associated);
  CHECK_STR(macs(v[0].bssid), "00:11:22:33:44:03");
  feed(s, Bytes(f.begin(), f.begin() + 12), meta(2));  // truncated PS-Poll
  CHECK_EQ(s.stats().malformed, 1u);
}

TEST(management_exchanges_learn_association) {
  auto& s = fresh();
  // Association request: station -> AP.
  Bytes areq = hdr(0, 0, ap(1), sta(1), ap(1));
  put(areq, {0x31, 0x04, 0x0a, 0x00});
  put(areq, ie(0, "net"));
  feed(s, areq, meta(1));
  // Reassociation response, success, AP -> station.
  Bytes rresp = hdr(0, 3, sta(2), ap(2), ap(2));
  put(rresp, {0x31, 0x04, 0x00, 0x00, 0x01, 0xc0});
  feed(s, rresp, meta(2));
  // Failed association response does not associate.
  Bytes fresp = hdr(0, 1, sta(3), ap(3), ap(3));
  put(fresp, {0x31, 0x04, 0x11, 0x00, 0x01, 0xc0});
  feed(s, fresp, meta(3));
  // Authentication from a station, and from an AP.
  Bytes auth1 = hdr(0, 11, ap(4), sta(4), ap(4));
  put(auth1, {0, 0, 1, 0, 0, 0});
  feed(s, auth1, meta(4));
  Bytes auth2 = hdr(0, 11, sta(5), ap(5), ap(5));
  put(auth2, {0, 0, 2, 0, 0, 0});
  feed(s, auth2, meta(5, -30));
  auto v = all(s);
  CHECK(find(v, sta(1)) && find(v, sta(1))->associated);
  CHECK_STR(macs(find(v, sta(1))->bssid), "00:11:22:33:44:01");
  CHECK(find(v, sta(2)) && find(v, sta(2))->associated);
  CHECK(find(v, sta(3)) == nullptr);
  CHECK(find(v, sta(4)) && find(v, sta(4))->associated);
  CHECK(find(v, sta(5)) && find(v, sta(5))->associated);
  CHECK_EQ(find(v, sta(5))->rssi_last, -128);  // the AP transmitted that one
  CHECK(find(v, ap(4)) == nullptr);
}

TEST(groups_and_bssids_are_not_clients) {
  auto& s = fresh();
  feed(s, data(ap(1), mac(0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb), ap(1), 0x01), meta(1));
  feed(s, data(kBroadcast, ap(1), sta(1), 0x02), meta(1));
  feed(s, data(mac(0x33, 0x33, 0, 0, 0, 1), ap(1), sta(1), 0x02), meta(1));
  CHECK_EQ(all(s).size(), 0u);
  // Learned as a station before its first beacon, then it beacons: it is an AP.
  feed(s, data(ap(2), ap(7), ap(2), 0x01), meta(2));
  CHECK_EQ(all(s).size(), 1u);
  feed(s, bss_frame(ap(7), ie(0, "late")), meta(3));
  CHECK_EQ(all(s).size(), 0u);
  // And from then on it is never re-added.
  feed(s, data(ap(2), ap(7), ap(2), 0x01), meta(4));
  CHECK_EQ(all(s).size(), 0u);
}

TEST(client_lru_and_sorting) {
  wlc::Config cfg;
  cfg.max_clients = 3;
  auto& s = fresh(cfg);
  feed(s, data(ap(1), sta(1), ap(1), 0x01), meta(10, -70));
  feed(s, data(ap(1), sta(2), ap(1), 0x01), meta(20, -40));
  feed(s, data(ap(1), sta(2), ap(1), 0x01), meta(21, -40));
  feed(s, data(ap(1), sta(3), ap(1), 0x01), meta(30, -55));
  feed(s, data(ap(1), sta(1), ap(1), 0x01), meta(40, -70));  // sta(2), last seen at 21, is now oldest
  feed(s, data(ap(1), sta(4), ap(1), 0x01), meta(50, -90));
  auto v = all(s);
  CHECK_EQ(v.size(), 3u);
  CHECK(find(v, sta(2)) == nullptr);
  CHECK_EQ(s.stats().client_evictions, 1u);
  auto r = all(s, wlc::ClientSort::Rssi);
  CHECK_STR(macs(r[0].mac), macs(sta(3).b));
  CHECK_STR(macs(r[2].mac), macs(sta(4).b));
  auto l = all(s, wlc::ClientSort::LastSeen);
  CHECK_STR(macs(l[0].mac), macs(sta(4).b));
  feed(s, data(ap(1), sta(1), ap(1), 0x01), meta(60, -70));
  auto fr = all(s, wlc::ClientSort::Frames);
  CHECK_STR(macs(fr[0].mac), macs(sta(1).b));
  CHECK_EQ(fr[0].frames, 3u);

  std::vector<wlc::ClientInfo> two(2);
  size_t total = 0;
  CHECK_EQ(s.clients(two.data(), 2, wlc::ClientSort::Mac, nullptr, &total), 2u);
  CHECK_EQ(total, 3u);
}
