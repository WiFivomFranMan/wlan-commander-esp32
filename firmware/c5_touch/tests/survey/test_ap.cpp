// SPDX-License-Identifier: MIT
#include "harness.h"

using namespace t;
using wlc::Band;
using wlc::Security;
using wlc::Width;

namespace {

wlc::Survey& fresh(wlc::Config cfg = wlc::Config()) {
  static wlc::Survey* s = nullptr;
  delete s;
  s = new wlc::Survey();
  if (!s->init(cfg)) std::abort();
  return *s;
}

wlc::ApInfo one(wlc::Survey& s, const Mac& bssid) { return detail(s, bssid).ap; }

Bytes ht_op(uint8_t primary, uint8_t info1) {
  Bytes b(22, 0);
  b[0] = primary;
  b[1] = info1;
  return ie(61, b);
}

Bytes vht_op(uint8_t width, uint8_t c0, uint8_t c1) { return ie(192, Bytes{width, c0, c1, 0xfc, 0xff}); }

// HE Operation with optional embedded VHT info and 6 GHz info.
Bytes he_op(bool vht, uint8_t vw, bool six, uint8_t primary6 = 0, uint8_t ctrl6 = 0) {
  Bytes b = {0x00, uint8_t(vht ? 0x40 : 0x00), uint8_t(six ? 0x02 : 0x00), 0x01, 0xfc, 0xff};
  if (vht) put(b, {vw, 42, 0});
  if (six) put(b, {primary6, ctrl6, 7, 15, 6});
  return ext_ie(36, b);
}

Bytes eht_op(bool info, uint8_t width) {
  Bytes b = {uint8_t(info ? 0x01 : 0x00), 0x11, 0x11, 0x11, 0x11};
  if (info) put(b, {width, 7, 15});
  return ext_ie(106, b);
}

}  // namespace

TEST(beacon_basic_fields) {
  auto& s = fresh();
  Bytes ies = ie(0, "Lab");
  put(ies, ie(3, Bytes{11}));
  put(ies, ie(7, Bytes{'U', 'S', 0x20}));
  put(ies, ie(11, Bytes{5, 0, 128, 0, 0}));
  feed(s, bss_frame(ap(1), ies, 0x0431, 8, 102), meta(1000, -60, 6));
  feed(s, bss_frame(ap(1), ies, 0x0431, 8, 102), meta(1100, -40, 6));
  auto a = one(s, ap(1));
  CHECK_STR(macs(a.bssid), "00:11:22:33:44:01");
  CHECK_EQ(a.ssid_len, 3);
  CHECK(std::memcmp(a.ssid, "Lab", 3) == 0);
  CHECK(!a.hidden);
  CHECK_EQ(a.channel, 11);  // DS Parameter Set beats the radio's channel
  CHECK_EQ(a.rx_channel, 6);
  CHECK_EQ(a.beacons, 2u);
  CHECK_EQ(a.beacon_interval, 102);
  CHECK_EQ(a.capability, 0x0431);
  CHECK_EQ(a.first_seen_ms, 1000u);
  CHECK_EQ(a.last_seen_ms, 1100u);
  CHECK_EQ(a.rssi_last, -40);
  CHECK_EQ(a.rssi_peak, -40);
  CHECK_EQ(a.rssi_avg, -58);  // -60 + (+20)/8
  CHECK(a.has_bss_load);
  CHECK_EQ(a.sta_count, 5);
  CHECK_EQ(a.channel_util, 128);
  CHECK_STR(a.country, "US");
  CHECK(a.security == Security::WEP);  // privacy bit, no RSN
  CHECK(a.width == Width::W20);
}

TEST(rssi_unknown_is_not_folded) {
  auto& s = fresh();
  feed(s, bss_frame(ap(1), ie(0, "x")), meta(1, 0));
  auto a = one(s, ap(1));
  CHECK_EQ(a.rssi_last, -128);
  feed(s, bss_frame(ap(1), ie(0, "x")), meta(2, -70));
  CHECK_EQ(one(s, ap(1)).rssi_avg, -70);
}

TEST(hidden_ssid_rules) {
  auto& s = fresh();
  feed(s, bss_frame(ap(2), ie(0, Bytes{})), meta(1));
  auto a = one(s, ap(2));
  CHECK(a.hidden);
  CHECK_EQ(a.ssid_len, 0);

  feed(s, bss_frame(ap(3), ie(0, Bytes{0, 0, 0, 0})), meta(1));
  CHECK(one(s, ap(3)).hidden);
  CHECK_EQ(one(s, ap(3)).ssid_len, 0);

  // A probe response names the hidden network; the beacon's hiding does not erase the name.
  feed(s, bss_frame(ap(2), ie(0, "Secret"), 0x0401, 5), meta(2));
  a = one(s, ap(2));
  CHECK(a.hidden);
  CHECK_EQ(a.ssid_len, 6);
  CHECK_EQ(a.probe_resps, 1u);
  feed(s, bss_frame(ap(2), ie(0, Bytes{})), meta(3));
  a = one(s, ap(2));
  CHECK_EQ(a.ssid_len, 6);
  CHECK(a.hidden);

  // First seen by probe response: not hidden.
  feed(s, bss_frame(ap(4), ie(0, "Open"), 0x0401, 5), meta(1));
  CHECK(!one(s, ap(4)).hidden);
  CHECK_EQ(one(s, ap(4)).beacons, 0u);
}

TEST(primary_channel_resolution) {
  auto& s = fresh();
  // HT Operation primary alone.
  Bytes ies = ie(0, "a");
  put(ies, ht_op(40, 0));
  feed(s, bss_frame(ap(1), ies), meta(1, -50, 36, Band::B5G));
  CHECK_EQ(one(s, ap(1)).channel, 40);
  // DS and HT disagree: trust neither, keep the receive channel.
  ies = ie(0, "b");
  put(ies, ie(3, Bytes{1}));
  put(ies, ht_op(6, 0));
  feed(s, bss_frame(ap(2), ies), meta(1, -50, 11));
  CHECK_EQ(one(s, ap(2)).channel, 11);
  // A channel that does not exist in the band is ignored.
  ies = ie(0, "c");
  put(ies, ie(3, Bytes{36}));
  feed(s, bss_frame(ap(3), ies), meta(1, -50, 1));
  CHECK_EQ(one(s, ap(3)).channel, 1);
  // Truncated HT Operation (< 22) contributes no primary.
  ies = ie(0, "d");
  put(ies, ie(61, Bytes{44, 0}));
  feed(s, bss_frame(ap(4), ies), meta(1, -50, 48, Band::B5G));
  CHECK_EQ(one(s, ap(4)).channel, 48);
}

TEST(six_ghz_band_never_inferred_from_channel) {
  auto& s = fresh();
  Bytes ies = ie(0, "Six");
  put(ies, ext_ie(35, Bytes(20, 0)));
  put(ies, he_op(false, 0, true, 37, 0x03));
  put(ies, rsn({8}, 0x00c0));
  feed(s, bss_frame(ap(6), ies), meta(1, -55, 33, Band::B6G));
  auto a = one(s, ap(6));
  CHECK(a.band == Band::B6G);
  CHECK_EQ(a.channel, 37);  // HE 6 GHz Operation primary
  CHECK(a.width == Width::W160);
  CHECK(a.phy & wlc::PHY_HE);
  CHECK(a.security == Security::WPA3SAE);
  CHECK(a.pmf_required);

  // Channel 1 on 6 GHz is a real 6 GHz channel, and a DS element must not move it.
  ies = ie(0, "Six1");
  put(ies, ie(3, Bytes{6}));
  put(ies, he_op(false, 0, true, 1, 0x02));
  feed(s, bss_frame(ap(7), ies), meta(1, -55, 5, Band::B6G));
  a = one(s, ap(7));
  CHECK(a.band == Band::B6G);
  CHECK_EQ(a.channel, 1);
  CHECK(a.width == Width::W80);

  size_t total = 0;
  wlc::ApInfo rows[4];
  CHECK_EQ(s.aps(rows, 4, wlc::ApSort::Rssi, wlc::BAND_2G4, false, &total), 0u);
  CHECK_EQ(s.aps(rows, 4, wlc::ApSort::Rssi, wlc::BAND_6G, false, &total), 2u);
}

TEST(channel_width_priority) {
  struct Case {
    Bytes ies;
    Width want;
  };
  auto with = [](std::initializer_list<Bytes> parts) {
    Bytes b = ie(0, "w");
    for (const auto& p : parts) put(b, p);
    return b;
  };
  Case cases[] = {
      {with({ht_op(36, 0x05)}), Width::W40},                     // secondary above + STA width
      {with({ht_op(36, 0x01)}), Width::W20},                     // offset but STA width 0
      {with({ht_op(36, 0x07)}), Width::W40},                     // below
      {with({ht_op(36, 0x05), vht_op(0, 0, 0)}), Width::W40},    // VHT 0 defers to HT
      {with({ht_op(36, 0x05), vht_op(1, 42, 0)}), Width::W80},
      {with({ht_op(36, 0x05), vht_op(1, 42, 50)}), Width::W160}, // CCFS1 set
      {with({vht_op(2, 50, 0)}), Width::W160},
      {with({vht_op(3, 42, 106)}), Width::W160},                 // 80+80 reported as 160
      {with({ht_op(36, 0x05), he_op(true, 1, false)}), Width::W80},
      {with({vht_op(1, 42, 0), eht_op(true, 4)}), Width::W320},
      {with({vht_op(1, 42, 0), eht_op(false, 4)}), Width::W80},  // no EHT info: fall back
      {with({ie(255, Bytes{106, 0x01, 0x00})}), Width::W20},     // truncated EHT op
  };
  int i = 0;
  for (const auto& c : cases) {
    auto& s = fresh();
    feed(s, bss_frame(ap(1), c.ies), meta(1, -50, 36, Band::B5G));
    if (one(s, ap(1)).width != c.want) {
      ::t::fail(__FILE__, __LINE__, "width case " + std::to_string(i) + ": " +
                                        wlc::width_label(one(s, ap(1)).width));
    }
    ++i;
  }
}

TEST(security_classification) {
  struct Case {
    Bytes ies;
    uint16_t cap;
    Security want;
    bool suite_b;
  };
  Bytes wpa1 = {0x00, 0x50, 0xf2, 0x01, 0x01, 0x00, 0x00, 0x50, 0xf2, 0x02, 0x01, 0x00,
                0x00, 0x50, 0xf2, 0x02, 0x01, 0x00, 0x00, 0x50, 0xf2, 0x02};
  Bytes owe_rsn = rsn({18}, 0x00c0);
  Case cases[] = {
      {ie(0, "o"), 0x0401, Security::Open, false},
      {ie(0, "w"), 0x0411, Security::WEP, false},
      {ie(221, wpa1), 0x0411, Security::WPA, false},
      {rsn({2}), 0x0411, Security::WPA2Personal, false},
      {rsn({4}), 0x0411, Security::WPA2Personal, false},  // FT-PSK
      {rsn({6}), 0x0411, Security::WPA2Personal, false},  // PSK-SHA256
      {rsn({8}), 0x0411, Security::WPA3SAE, false},
      {rsn({24}), 0x0411, Security::WPA3SAE, false},      // SAE-EXT-KEY
      {rsn({2, 8}), 0x0411, Security::WPA2WPA3, false},
      {rsn({8, 9, 4, 2}), 0x0411, Security::WPA2WPA3, false},
      {owe_rsn, 0x0411, Security::OWE, false},
      {rsn({1}), 0x0411, Security::Enterprise, false},
      {rsn({5, 3}), 0x0411, Security::Enterprise, false},
      {rsn({12}), 0x0411, Security::Enterprise, true},    // Suite-B 192
      {rsn({11}), 0x0411, Security::Enterprise, true},
      {rsn({1, 8}), 0x0411, Security::Enterprise, false}, // enterprise wins
      {ie(48, Bytes{1, 0, 0x00, 0x0f, 0xac, 4}), 0x0411, Security::Unknown, false},  // no AKM list
  };
  int i = 0;
  for (const auto& c : cases) {
    auto& s = fresh();
    feed(s, bss_frame(ap(1), c.ies, c.cap), meta(1));
    const auto a = one(s, ap(1));
    if (a.security != c.want || a.suite_b != c.suite_b) {
      ::t::fail(__FILE__, __LINE__, "security case " + std::to_string(i) + ": got " +
                                        wlc::security_label(a.security, a.suite_b));
    }
    ++i;
  }
  auto& s = fresh();
  feed(s, bss_frame(ap(1), rsn({2, 8}, 0x0080)), meta(1));
  auto a = one(s, ap(1));
  CHECK(a.pmf_capable);
  CHECK(!a.pmf_required);
  CHECK_EQ(a.akm_mask, (1u << 2) | (1u << 8));
  CHECK_EQ(a.pairwise_ciphers, wlc::CIPHER_CCMP);
  CHECK_EQ(a.group_cipher, 4);
  feed(s, bss_frame(ap(1), rsn({8}, 0x00c0)), meta(2));
  a = one(s, ap(1));
  CHECK(a.pmf_capable && a.pmf_required);
}

TEST(phy_generation_flags) {
  auto& s = fresh();
  Bytes ies = ie(0, "g");
  put(ies, ie(45, Bytes(26, 0)));
  feed(s, bss_frame(ap(1), ies), meta(1));
  CHECK_STR(wlc::phy_label(one(s, ap(1)).phy), "Wi-Fi 4");
  put(ies, ie(191, Bytes(12, 0)));
  feed(s, bss_frame(ap(1), ies), meta(2));
  CHECK_STR(wlc::phy_label(one(s, ap(1)).phy), "Wi-Fi 5");
  put(ies, ext_ie(35, Bytes(20, 0)));
  feed(s, bss_frame(ap(1), ies), meta(3));
  CHECK_STR(wlc::phy_label(one(s, ap(1)).phy), "Wi-Fi 6");
  put(ies, ext_ie(108, Bytes(12, 0)));
  feed(s, bss_frame(ap(1), ies), meta(4));
  auto a = one(s, ap(1));
  CHECK_STR(wlc::phy_label(a.phy), "Wi-Fi 7");
  CHECK_EQ(a.phy, wlc::PHY_HT | wlc::PHY_VHT | wlc::PHY_HE | wlc::PHY_EHT);
  feed(s, bss_frame(ap(2), ie(0, "legacy")), meta(4));
  CHECK_STR(wlc::phy_label(one(s, ap(2)).phy), "Legacy");
}

TEST(hotspot20_beacon_elements) {
  auto& s = fresh();
  Bytes ies = ie(0, "Passpoint");
  put(ies, ie(221, Bytes{0x50, 0x6f, 0x9a, 0x10, 0x21}));  // release field 2 -> R3, DGAF disabled
  put(ies, ie(107, Bytes{0x12, 2, 8, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55}));  // full form
  put(ies, ie(111, Bytes{4, 0x45, 0x5a, 0x03, 0xba, 0x00, 0x00, 0x1b, 0xc5, 0x04, 0xbd, 0x50, 0x6f, 0x9a}));
  feed(s, bss_frame(ap(9), ies, 0x0411), meta(1));
  auto a = one(s, ap(9));
  CHECK(a.hs20);
  CHECK_EQ(a.hs20_release, 3);
  CHECK(a.hs20_dgaf_disabled);
  CHECK(a.interworking);
  CHECK_EQ(a.access_network_type, 2);
  CHECK(a.internet);
  CHECK(a.has_venue);
  CHECK_EQ(a.venue_group, 2);
  CHECK_EQ(a.venue_type, 8);
  CHECK(a.has_hessid);
  CHECK_STR(macs(a.hessid), "02:11:22:33:44:55");
  CHECK(a.has_roaming_consortium);
  CHECK_EQ(a.rc_anqp_oi_count, 4);
  CHECK_EQ(a.rc_oi_count, 3);
  char oi[31];
  CHECK_STR(wlc::oi_to_string(a.rc_ois[0], oi), "5A03BA0000");
  CHECK_STR(wlc::oi_to_string(a.rc_ois[1], oi), "1BC504BD");
  CHECK_STR(wlc::oi_to_string(a.rc_ois[2], oi), "506F9A");
  CHECK(a.passpoint);

  // Interworking alone (length 1) is enough for the passpoint flag; HESSID-only form (7).
  ies = ie(0, "iw");
  put(ies, ie(107, Bytes{0x03, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01}));
  feed(s, bss_frame(ap(10), ies), meta(1));
  a = one(s, ap(10));
  CHECK(!a.passpoint && a.interworking && !a.hs20 && a.has_hessid && !a.has_venue);
  CHECK_EQ(a.access_network_type, 3);
  ies = ie(0, "iw3");
  put(ies, ie(107, Bytes{0x00, 1, 7}));
  feed(s, bss_frame(ap(11), ies), meta(1));
  a = one(s, ap(11));
  CHECK(a.has_venue && !a.has_hessid);
  feed(s, bss_frame(ap(12), ie(0, "plain")), meta(1));
  CHECK(!one(s, ap(12)).passpoint);

  wlc::ApInfo rows[8];
  size_t total = 0;
  CHECK_EQ(s.aps(rows, 8, wlc::ApSort::Ssid, wlc::BAND_ALL, true, &total), 1u);
  CHECK_EQ(total, 1u);
  CHECK_EQ(s.aps(rows, 8, wlc::ApSort::Ssid, wlc::BAND_ALL, true, &total, true), 3u);
  CHECK_EQ(total, 3u);
}

TEST(malformed_beacons) {
  auto& s = fresh();
  // RSN after a well-formed SSID, then an element claiming 200 bytes that are not there.
  Bytes ies = ie(0, "cut");
  put(ies, rsn({8}));
  put(ies, {221, 200, 0x00, 0x50});
  feed(s, bss_frame(ap(1), ies), meta(1));
  auto a = one(s, ap(1));
  CHECK_EQ(a.ssid_len, 3);
  CHECK(a.security == Security::WPA3SAE);  // first sighting: take what was readable
  CHECK_EQ(s.stats().malformed, 1u);

  // Later truncated frame before RSN must not downgrade a known AP to Open.
  Bytes cut = bss_frame(ap(1), ie(0, "cut"));
  put(cut, {48, 20, 1, 0});
  feed(s, cut, meta(2));
  CHECK(one(s, ap(1)).security == Security::WPA3SAE);
  CHECK_EQ(one(s, ap(1)).beacons, 2u);

  // Shorter than header + fixed fields: no AP created.
  Bytes tiny = hdr(0, 8, kBroadcast, ap(2), ap(2));
  put(tiny, {1, 2, 3});
  feed(s, tiny, meta(3));
  CHECK_EQ(s.stats().aps, 1u);
  // Group BSSID is not an AP.
  feed(s, bss_frame(mac(0x01, 0, 0x5e, 0, 0, 1), ie(0, "m")), meta(3));
  CHECK_EQ(s.stats().aps, 1u);
  // A single dangling byte after the elements counts as malformed too.
  Bytes dangling = bss_frame(ap(3), ie(0, "d"));
  dangling.push_back(0xdd);
  feed(s, dangling, meta(4));
  CHECK_EQ(s.stats().malformed, 5u);
  // The last element claims exactly one octet more than the frame holds.
  Bytes short1 = bss_frame(ap(5), ie(0, "o"));
  put(short1, {221, 5, 0x50, 0x6f, 0x9a, 0x10});
  feed(s, short1, meta(4));
  CHECK_EQ(s.stats().malformed, 6u);
  CHECK(!one(s, ap(5)).hs20);
  // Frames that are not 802.11 version 0, or too short to classify.
  Bytes v1 = bss_frame(ap(4), ie(0, "v"));
  v1[0] |= 0x01;
  feed(s, v1, meta(5));
  feed(s, Bytes{0x80, 0}, meta(5));
  s.ingest(nullptr, 0, meta(5));
  CHECK_EQ(s.stats().malformed, 9u);
}

TEST(htc_header_offset) {
  auto& s = fresh();
  Bytes f = hdr(0, 8, kBroadcast, ap(1), ap(1), 0x80);  // Order bit: +HTC
  put(f, {0, 0, 0, 0});
  for (int i = 0; i < 8; ++i) f.push_back(0);
  put16(f, 100);
  put16(f, 0x0401);
  put(f, ie(0, "htc"));
  feed(s, f, meta(1));
  CHECK_EQ(one(s, ap(1)).ssid_len, 3);
}

TEST(ap_lru_eviction) {
  wlc::Config cfg;
  cfg.max_aps = 4;
  auto& s = fresh(cfg);
  for (uint8_t i = 1; i <= 4; ++i) feed(s, bss_frame(ap(i), ie(0, "e")), meta(i * 10));
  feed(s, bss_frame(ap(1), ie(0, "e")), meta(50));  // ap(2) is now least recently seen
  feed(s, bss_frame(ap(5), ie(0, "e")), meta(60));
  CHECK_EQ(s.stats().aps, 4u);
  CHECK_EQ(s.stats().ap_evictions, 1u);
  wlc::ApDetail d;
  CHECK(!s.ap_detail(ap(2).b, &d));
  CHECK(s.ap_detail(ap(1).b, &d));
  CHECK(s.ap_detail(ap(5).b, &d));
  // Evict many in a row: the hash index must stay consistent through backward-shift deletes.
  for (uint8_t i = 6; i < 200; ++i) feed(s, bss_frame(ap(i), ie(0, "e")), meta(100u + i));
  for (uint8_t i = 196; i < 200; ++i) CHECK(s.ap_detail(ap(i).b, &d));
  CHECK_EQ(s.stats().aps, 4u);
  CHECK_EQ(s.stats().ap_evictions, 195u);
}
