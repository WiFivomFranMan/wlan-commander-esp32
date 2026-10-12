// SPDX-License-Identifier: MIT
#include <memory>

#include "harness.h"

using namespace t;

namespace {

std::unique_ptr<wlc::AnqpRecord> decode(const Bytes& stream, size_t* walked = nullptr) {
  auto r = std::make_unique<wlc::AnqpRecord>();
  const size_t w = wlc::decode_anqp_elements(stream.data(), stream.size(), r.get());
  if (walked) *walked = w;
  return r;
}

Bytes lv(const std::string& s) {
  Bytes b = {uint8_t(s.size())};
  put(b, s);
  return b;
}

Bytes duple(const char* lang, const std::string& name) {
  Bytes b = {uint8_t(3 + name.size())};
  put(b, std::string(lang, 3));
  put(b, name);
  return b;
}

struct Eap {
  uint8_t method;
  std::vector<std::pair<uint8_t, uint8_t>> params;  // id, one-octet value
};

Bytes nai_realm(const std::string& realm, const std::vector<Eap>& eaps) {
  Bytes f = {0x00, uint8_t(realm.size())};
  put(f, realm);
  f.push_back(uint8_t(eaps.size()));
  for (const auto& e : eaps) {
    Bytes m = {e.method, uint8_t(e.params.size())};
    for (const auto& p : e.params) put(m, {p.first, 1, p.second});
    f.push_back(uint8_t(m.size()));
    put(f, m);
  }
  Bytes out;
  put16(out, uint16_t(f.size()));
  put(out, f);
  return out;
}

// MSB-first bit packer, matching the field order core's decode_lci_field reads.
struct Bits {
  Bytes b = Bytes(16, 0);
  unsigned pos = 0;
  void add(uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i, ++pos) {
      if ((v >> (n - 1 - i)) & 1) b[pos / 8] |= uint8_t(0x80 >> (pos % 8));
    }
  }
};

Bytes lci(double lat, double lon, unsigned alt_type, double alt) {
  auto twos = [](double v, double scale, unsigned bits) {
    const int64_t raw = int64_t(v * scale);
    return uint64_t(raw) & ((1ull << bits) - 1);
  };
  Bits w;
  w.add(18, 6);
  w.add(twos(lat, 33554432.0, 34), 34);
  w.add(18, 6);
  w.add(twos(lon, 33554432.0, 34), 34);
  w.add(alt_type, 4);
  w.add(15, 6);
  w.add(twos(alt, 256.0, 30), 30);
  w.add(1, 3);  // WGS84
  w.add(0, 5);
  return w.b;
}

}  // namespace

TEST(anqp_core_passpoint_elements) {
  Bytes s;
  Bytes venue = {2, 8};
  put(venue, duple("eng", "Hotel"));
  put(venue, duple("fra", "H\xc3\xb4tel"));
  put(s, anqp_el(258, venue));
  Bytes auth = {0x00};
  put16(auth, 19);
  put(auth, "https://tc.example/");
  put(auth, {0x02, 0x00, 0x00});
  put(s, anqp_el(260, auth));
  put(s, anqp_el(261, Bytes{3, 0x00, 0x1b, 0xc5, 5, 0x5a, 0x03, 0xba, 0x00, 0x00}));
  put(s, anqp_el(262, Bytes{0x0d}));
  Bytes nai;
  put16(nai, 2);
  put(nai, nai_realm("example.com", {{21, {{2, 4}, {5, 7}}}, {13, {{5, 6}}}}));
  put(nai, nai_realm("b.org", {}));
  put(s, anqp_el(263, nai));
  put(s, anqp_el(264, Bytes{0, 9, 0, 7, 2, 0x13, 0x00, 0x14, 0x00, 0xf1, 0x10}));
  Bytes dom = lv("boingo.com");
  put(dom, lv("rr.com"));
  put(s, anqp_el(268, dom));
  put(s, anqp_el(257, Bytes{0x02, 0x01, 0x07, 0x01, 0xdd, 0xdd, 0x05, 0x00, 0x50, 0x6f, 0x9a, 0x11, 0x00,
                            0x08, 0x01}));
  put(s, anqp_el(256, Bytes{0x02, 0x01, 0x0c, 0x01}));
  Bytes vurl = {uint8_t(1 + 18), 1};
  put(vurl, "https://venue.test");
  put(s, anqp_el(277, vurl));

  size_t walked = 0;
  auto r = decode(s, &walked);
  CHECK_EQ(walked, 10u);
  CHECK(!r->truncated);
  CHECK(r->present & wlc::AF_VENUE_NAME);
  CHECK_EQ(r->venue_group, 2);
  CHECK_EQ(r->venue_type, 8);
  CHECK_EQ(r->n_venue_names, 2);
  CHECK_STR(r->venue_names[0].lang, "eng");
  CHECK_STR(r->venue_names[0].name.s, "Hotel");
  CHECK_STR(r->venue_names[1].name.s, "H\xc3\xb4tel");
  CHECK_EQ(r->n_auth_types, 2);
  CHECK_EQ(r->auth_types[0].type, 0);
  CHECK_STR(r->auth_types[0].url.s, "https://tc.example/");
  CHECK_EQ(r->auth_types[1].type, 2);
  CHECK_EQ(r->n_roaming_ois, 2);
  char oi[31];
  CHECK_STR(wlc::oi_to_string(r->roaming_ois[0], oi), "001BC5");
  CHECK_STR(wlc::oi_to_string(r->roaming_ois[1], oi), "5A03BA0000");
  CHECK_EQ(r->ip_addr_type, 0x0d);
  CHECK_EQ(r->nai_realm_count, 2);
  CHECK_EQ(r->n_nai_realms, 2);
  CHECK_STR(r->nai_realms[0].realm.s, "example.com");
  CHECK_EQ(r->nai_realms[0].eap_count, 2);
  CHECK_EQ(r->nai_realms[0].eap[0].method, 21);
  CHECK_EQ(r->nai_realms[0].eap[0].non_eap_inner, 4);
  CHECK_EQ(r->nai_realms[0].eap[0].credential, 7);
  CHECK_EQ(r->nai_realms[0].eap[1].method, 13);
  CHECK_EQ(r->nai_realms[0].eap[1].credential, 6);
  char sum[64];
  CHECK_STR(wlc::nai_eap_summary(r->nai_realms[0].eap[0], sum, sizeof(sum)),
            "EAP-TTLS (MSCHAPv2, username/password)");
  CHECK_STR(r->nai_realms[1].realm.s, "b.org");
  CHECK_EQ(r->nai_realms[1].n_eap, 0);
  CHECK_EQ(r->n_plmns, 2);
  CHECK_STR(r->plmns[0].mcc, "310");
  CHECK_STR(r->plmns[0].mnc, "410");
  CHECK_STR(r->plmns[1].mcc, "001");
  CHECK_STR(r->plmns[1].mnc, "01");
  CHECK_EQ(r->n_domains, 2);
  CHECK_STR(r->domains[0].s, "boingo.com");
  CHECK_STR(r->domains[1].s, "rr.com");
  CHECK_EQ(r->n_capability_ids, 4);
  CHECK_EQ(r->capability_ids[2], 0xdddd);
  CHECK_EQ(r->capability_ids[3], 264);
  CHECK_EQ(r->n_query_ids, 2);
  CHECK_EQ(r->n_venue_urls, 1);
  CHECK_EQ(r->venue_urls[0].venue_number, 1);
  CHECK_STR(r->venue_urls[0].url.s, "https://venue.test");
}

TEST(anqp_location_and_emergency_elements_are_text) {
  Bytes s;
  Bytes geo = {0, 16};
  put(geo, lci(37.5, -122.25, 1, 10.5));
  put(s, anqp_el(265, geo));
  Bytes civic = {0x00, 19, 'U', 'S', 1, 2, 'T', 'X', 3, 6, 'A', 'u', 's', 't', 'i', 'n', 19, 3, '1', '2', '3'};
  put(s, anqp_el(266, civic));
  put(s, anqp_el(267, lv("https://lis.example.net/held")));
  put(s, anqp_el(259, Bytes{0x01, 3, '1', '1', '2'}));  // leading descriptor octet
  put(s, anqp_el(269, Bytes{'u', 'r', 'n', ':', 'a', 'l', 'e', 'r', 't'}));  // bare text
  Bytes nai = lv("sos@example");
  put(s, anqp_el(271, nai));
  auto r = decode(s);
  CHECK(r->present & wlc::AF_GEOSPATIAL);
  CHECK_EQ(r->geo.len, 18);
  CHECK_EQ(r->geo.raw[1], 16);
  CHECK_STR(r->geo_summary.s, "37.500000,-122.250000 alt 10.5 m");
  CHECK_STR(r->civic_summary.s, "US: A1=TX; A3=Austin; HNO=123");
  CHECK_EQ(r->civic.wire_len, 21);
  CHECK_EQ(r->n_location_uris, 1);
  CHECK_STR(r->location_uris[0].s, "https://lis.example.net/held");
  CHECK_EQ(r->location_uri_raw.len, 29);
  CHECK_EQ(r->n_emergency_numbers, 1);
  CHECK_STR(r->emergency_numbers[0].s, "112");
  CHECK_EQ(r->n_emergency_alert_uris, 1);
  CHECK_STR(r->emergency_alert_uris[0].s, "urn:alert");
  CHECK_STR(r->emergency_nai.s, "sos@example");

  // Bare 16-octet LCI, floor altitude, southern/western hemisphere.
  r = decode(anqp_el(265, lci(-33.875, -70.5, 2, 3)));
  CHECK_STR(r->geo_summary.s, "-33.875000,-70.500000 floor 3.0");
  // Garbage: kept raw, no summary.
  Bits junk;
  junk.add(0, 6);
  junk.add(1ull << 32, 34);  // latitude 128 degrees
  r = decode(anqp_el(265, junk.b));
  CHECK(r->present & wlc::AF_GEOSPATIAL);
  CHECK_EQ(r->geo.len, 16);
  CHECK_EQ(r->geo_summary.len, 0);
  // Civic that validates at no offset: raw kept, no summary.
  r = decode(anqp_el(266, Bytes{'u', 's', 1, 9, 'x'}));
  CHECK_EQ(r->civic.len, 5);
  CHECK_EQ(r->civic_summary.len, 0);
  // Two emergency numbers.
  Bytes two = lv("911");
  put(two, lv("112"));
  r = decode(anqp_el(259, two));
  CHECK_EQ(r->n_emergency_numbers, 2);
  CHECK_STR(r->emergency_numbers[1].s, "112");
}

TEST(anqp_hs20_vendor_elements) {
  Bytes s;
  put(s, hs20_el(1, Bytes{3, 4, 5}));
  put(s, hs20_el(2, Bytes{1, 2, 3, 4, 5, 7}));
  Bytes ofn = duple("eng", "Boingo");
  put(ofn, duple("spa", "Boingo ES"));
  put(s, hs20_el(3, ofn));
  Bytes wan = {0x01};
  put32(wan, 100000);
  put32(wan, 50000);
  put(wan, {10, 20});
  put16(wan, 100);
  put(s, hs20_el(4, wan));
  Bytes cc;
  put(cc, {6});
  put16(cc, 443);
  put(cc, {1, 17});
  put16(cc, 500);
  put(cc, {1, 50});
  put16(cc, 0);
  put(cc, {2});
  put(s, hs20_el(5, cc));
  put(s, hs20_el(6, Bytes{1, 0, 7, 'a', '.', 'b', '.', 'c', 'o', 'm'}));
  put(s, hs20_el(7, Bytes{81, 115, 124}));
  Bytes prov;
  Bytes fn = duple("eng", "Acme!");
  put16(prov, uint16_t(fn.size()));
  put(prov, fn);
  put(prov, {0, 0});  // server URI len, method list len
  put16(prov, 0);     // icons
  prov.push_back(0);  // NAI
  put16(prov, 0);     // service description
  Bytes osu = lv("osu1");
  osu.push_back(1);
  put16(osu, uint16_t(prov.size()));
  put(osu, prov);
  put(s, hs20_el(8, osu));
  put(s, hs20_el(10, Bytes{0x01, 0x02}));                            // icon request: unknown
  put(s, anqp_el(0xdddd, Bytes{0x00, 0x11, 0x22, 0x01, 0x02, 0x03}));  // other vendor
  put(s, anqp_el(272, Bytes{1, 2, 3}));                                // neighbor report
  auto r = decode(s);
  CHECK_EQ(r->n_hs20_query, 3);
  CHECK_EQ(r->hs20_query[2], 5);
  CHECK_EQ(r->n_hs20_capability, 6);
  CHECK_EQ(r->n_operator_names, 2);
  CHECK_STR(r->operator_names[0].name.s, "Boingo");
  CHECK_STR(r->operator_names[1].lang, "spa");
  CHECK(r->present & wlc::AF_WAN_METRICS);
  CHECK_EQ(r->wan.info, 1);
  CHECK_EQ(r->wan.dl_kbps, 100000u);
  CHECK_EQ(r->wan.ul_kbps, 50000u);
  CHECK_EQ(r->wan.dl_load, 10);
  CHECK_EQ(r->wan.ul_load, 20);
  CHECK_EQ(r->wan.lmd, 100);
  CHECK_EQ(r->n_conn_caps, 3);
  CHECK_EQ(r->conn_caps[0].port, 443);
  CHECK_EQ(r->conn_caps[1].proto, 17);
  CHECK_EQ(r->conn_caps[2].status, 2);
  CHECK_EQ(r->n_home_realms, 1);
  CHECK_STR(r->home_realms[0].s, "a.b.com");
  CHECK_EQ(r->n_op_classes, 3);
  CHECK_EQ(r->op_classes[1], 115);
  CHECK_EQ(r->osu_provider_count, 1);
  CHECK_EQ(r->n_osu_names, 1);
  CHECK_STR(r->osu_names[0].s, "Acme!");
  CHECK_EQ(r->n_unknown, 3);
  CHECK_EQ(r->unknown[0].id, 0xdddd);
  CHECK_EQ(r->unknown[0].subtype, 10);
  CHECK_EQ(r->unknown[1].id, 0xdddd);
  CHECK_EQ(r->unknown[2].id, 272);
  CHECK_EQ(r->unknown[2].len, 3);
}

TEST(anqp_bounds_and_truncation) {
  Bytes dom;
  for (int i = 0; i < 10; ++i) put(dom, lv("d" + std::to_string(i) + ".example"));
  auto r = decode(anqp_el(268, dom));
  CHECK_EQ(r->n_domains, 8);
  CHECK(r->truncated);

  // 62 ASCII bytes then a 2-byte UTF-8 character: the cut at 63 must back up to 62.
  std::string longname(62, 'a');
  longname += "\xc3\xa9";
  r = decode(anqp_el(268, lv(longname)));
  CHECK_EQ(r->domains[0].len, 62);
  CHECK(r->domains[0].truncated);
  CHECK(r->truncated);

  // Control bytes never reach the UI verbatim.
  r = decode(anqp_el(268, lv(std::string("a\x07" "b\x1b" "c", 5))));
  CHECK_STR(r->domains[0].s, "a?b?c");

  // An element whose length overruns the stream ends the walk; what came before survives.
  Bytes s = anqp_el(268, lv("ok.example"));
  put16(s, 263);
  put16(s, 500);
  put(s, {1, 0, 2});
  size_t walked = 0;
  r = decode(s, &walked);
  CHECK_EQ(walked, 1u);
  CHECK_EQ(r->n_domains, 1);
  CHECK(!(r->present & wlc::AF_NAI_REALM));

  // NAI realm lying about its count and field lengths.
  Bytes nai;
  put16(nai, 50);
  put(nai, nai_realm("x.org", {{21, {{2, 4}}}}));
  put16(nai, 400);
  r = decode(anqp_el(263, nai));
  CHECK_EQ(r->n_nai_realms, 1);
  // EAP method whose length runs past the realm field.
  Bytes bad;
  put16(bad, 1);
  Bytes f = {0, 3, 'a', '.', 'b', 1, 40, 21, 0};
  put16(bad, uint16_t(f.size()));
  put(bad, f);
  r = decode(anqp_el(263, bad));
  CHECK_EQ(r->n_nai_realms, 1);
  CHECK_EQ(r->nai_realms[0].n_eap, 0);
  // 3GPP whose count exceeds its bytes.
  r = decode(anqp_el(264, Bytes{0, 9, 0, 7, 9, 0x13, 0x00, 0x14}));
  CHECK_EQ(r->n_plmns, 1);
  // Venue name too short for a duple.
  r = decode(anqp_el(258, Bytes{1, 2, 2, 'e', 'n'}));
  CHECK_EQ(r->n_venue_names, 0);
  // Empty stream, and a stream shorter than one header.
  CHECK_EQ(wlc::decode_anqp_elements(nullptr, 0, r.get()), 0u);
  Bytes three = {0x00, 0x01, 0x05};
  r = decode(three);
  CHECK_EQ(r->present, 0u);
}

TEST(anqp_merge_latest_element_wins) {
  Bytes a = anqp_el(268, lv("first.example"));
  Bytes venue = {1, 1};
  put(venue, duple("eng", "Venue"));
  put(a, anqp_el(258, venue));
  Bytes b = anqp_el(268, lv("second.example"));
  auto ra = decode(a);
  auto rb = decode(b);
  auto acc = std::make_unique<wlc::AnqpRecord>();
  acc->clear();
  wlc::merge_anqp(acc.get(), *ra);
  wlc::merge_anqp(acc.get(), *rb);
  CHECK_EQ(acc->n_domains, 1);
  CHECK_STR(acc->domains[0].s, "second.example");
  CHECK_EQ(acc->n_venue_names, 1);
  CHECK_STR(acc->venue_names[0].name.s, "Venue");
  CHECK(acc->present & wlc::AF_VENUE_NAME);
}

// ---- query builder ------------------------------------------------------------------------

// Derived by hand from wlc_sniffer.ino build_gas(): 04 0a tok | 6c 02 00 00 | qlen | 00 01 len ids
// | dd dd vlen 50 6f 9a 11 01 00 03 04 05 (Passpoint set only).
TEST(anqp_query_builder_matches_firmware) {
  const Bytes passpoint = {0x04, 0x0a, 0x2a, 0x6c, 0x02, 0x00, 0x00, 0x23, 0x00, 0x00, 0x01,
                           0x12, 0x00, 0x01, 0x01, 0x02, 0x01, 0x04, 0x01, 0x05, 0x01, 0x06,
                           0x01, 0x07, 0x01, 0x08, 0x01, 0x0c, 0x01, 0x15, 0x01, 0xdd, 0xdd,
                           0x09, 0x00, 0x50, 0x6f, 0x9a, 0x11, 0x01, 0x00, 0x03, 0x04, 0x05};
  const Bytes location = {0x04, 0x0a, 0x07, 0x6c, 0x02, 0x00, 0x00, 0x10, 0x00, 0x00, 0x01, 0x0c, 0x00,
                          0x03, 0x01, 0x09, 0x01, 0x0a, 0x01, 0x0b, 0x01, 0x0d, 0x01, 0x0f, 0x01};
  uint8_t out[128];
  size_t n = wlc::build_anqp_query_body(out, sizeof(out), 0x2a, wlc::QuerySet::Passpoint);
  CHECK_EQ(n, passpoint.size());
  CHECK(Bytes(out, out + n) == passpoint);
  n = wlc::build_anqp_query_body(out, sizeof(out), 0x07, wlc::QuerySet::Location);
  CHECK_EQ(n, location.size());
  CHECK(Bytes(out, out + n) == location);

  // The public name and the integration's name are one function.
  n = wlc::build_anqp_query(out, sizeof(out), 0x2a, wlc::QuerySet::Passpoint);
  CHECK(Bytes(out, out + n) == passpoint);
  n = wlc::build_anqp_query(out, sizeof(out), 0x07, wlc::QuerySet::Location);
  CHECK(Bytes(out, out + n) == location);

  CHECK_EQ(wlc::build_anqp_query_body(out, passpoint.size() - 1, 1, wlc::QuerySet::Passpoint), 0u);
  CHECK_EQ(wlc::build_anqp_query(out, location.size() - 1, 1, wlc::QuerySet::Location), 0u);
  CHECK_EQ(wlc::build_anqp_query(nullptr, 128, 1, wlc::QuerySet::Location), 0u);

  // What we send is what we can read back: the request's own query list decodes.
  n = wlc::build_anqp_query_body(out, sizeof(out), 1, wlc::QuerySet::Passpoint);
  auto r = std::make_unique<wlc::AnqpRecord>();
  wlc::decode_anqp_elements(out + 9, n - 9, r.get());
  CHECK_EQ(r->n_query_ids, 9);
  CHECK_EQ(r->query_ids[8], 277);
  CHECK_EQ(r->n_hs20_query, 3);
}
