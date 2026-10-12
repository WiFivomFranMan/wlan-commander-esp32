// SPDX-License-Identifier: MIT
#include <memory>

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

Bytes domains(std::initializer_list<const char*> names) {
  Bytes b;
  for (const char* n : names) {
    b.push_back(uint8_t(std::strlen(n)));
    put(b, std::string(n));
  }
  return anqp_el(268, b);
}

std::unique_ptr<wlc::AnqpRecord> anqp(wlc::Survey& s, const Mac& bssid) {
  auto r = std::make_unique<wlc::AnqpRecord>();
  if (!s.anqp_for(bssid.b, r.get())) return nullptr;
  return r;
}

void beacon(wlc::Survey& s, const Mac& bssid, uint32_t now) {
  feed(s, bss_frame(bssid, ie(0, "pp")), meta(now));
}

}  // namespace

TEST(gas_initial_response_attaches_to_addr3) {
  auto& s = fresh();
  beacon(s, ap(1), 1);
  feed(s, gas_initial(ap(1), sta(1), ap(1), 5, domains({"boingo.com"})), meta(2));
  auto d = detail(s, ap(1));
  CHECK(d.has_anqp);
  CHECK(d.ap.has_anqp);
  CHECK_EQ(d.ap.anqp_updated_ms, 2u);
  CHECK_EQ(d.anqp.n_domains, 1);
  CHECK_STR(d.anqp.domains[0].s, "boingo.com");
  CHECK_EQ(d.anqp.responses, 1);
  CHECK_EQ(s.stats().anqp_completed, 1u);
  CHECK_EQ(s.stats().gas_responses, 1u);
  // The requester is not mistaken for an AP, and the response does not create a client.
  CHECK_EQ(s.stats().aps, 1u);
  CHECK_EQ(s.stats().clients, 0u);
}

TEST(gas_wildcard_addr3_attributes_to_transmitter) {
  auto& s = fresh();
  beacon(s, ap(2), 1);
  beacon(s, ap(3), 1);
  feed(s, gas_initial(ap(2), sta(1), kBroadcast, 9, domains({"wild.example"})), meta(2));
  feed(s, gas_initial(ap(3), sta(1), mac(0, 0, 0, 0, 0, 0), 9, domains({"zero.example"})), meta(2));
  auto r2 = anqp(s, ap(2));
  auto r3 = anqp(s, ap(3));
  CHECK(r2 && r2->n_domains == 1);
  CHECK(r3 && r3->n_domains == 1);
  if (r2) CHECK_STR(r2->domains[0].s, "wild.example");
  if (r3) CHECK_STR(r3->domains[0].s, "zero.example");
  CHECK(anqp(s, kBroadcast) == nullptr);
  // A non-wildcard addr3 that differs from the transmitter wins over the transmitter.
  beacon(s, ap(4), 1);
  feed(s, gas_initial(ap(5), sta(1), ap(4), 9, domains({"addr3.example"})), meta(3));
  CHECK(anqp(s, ap(4)) != nullptr);
  CHECK(anqp(s, ap(5)) == nullptr);
}

TEST(gas_pending_until_ap_appears) {
  auto& s = fresh();
  feed(s, gas_initial(ap(7), sta(1), ap(7), 1, domains({"early.example"})), meta(5));
  CHECK_EQ(s.stats().pending, 1u);
  CHECK_EQ(s.stats().anqp_pending, 1u);
  uint8_t pend[4][6];
  CHECK_EQ(s.pending_bssids(pend, 4), 1u);
  CHECK_STR(macs(pend[0]), macs(ap(7).b));
  wlc::ApDetail d;
  CHECK(!s.ap_detail(ap(7).b, &d));
  CHECK(anqp(s, ap(7)) != nullptr);
  // A second answer before the AP shows up merges into the same pending record.
  feed(s, gas_initial(ap(7), sta(1), ap(7), 2, domains({"later.example"})), meta(6));
  CHECK_EQ(s.stats().pending, 1u);

  beacon(s, ap(7), 10);
  CHECK_EQ(s.stats().pending, 0u);
  d = detail(s, ap(7));
  CHECK(d.has_anqp && d.ap.has_anqp);
  CHECK_EQ(d.ap.anqp_updated_ms, 6u);
  CHECK_EQ(d.anqp.responses, 2);
  CHECK_STR(d.anqp.domains[0].s, "later.example");
  CHECK_EQ(d.anqp.first_ms, 5u);
}

TEST(gas_pending_capacity_evicts_oldest) {
  wlc::Config cfg;
  cfg.max_pending = 2;
  auto& s = fresh(cfg);
  for (uint8_t i = 1; i <= 3; ++i) feed(s, gas_initial(ap(i), sta(1), ap(i), i, domains({"p.example"})), meta(i));
  CHECK_EQ(s.stats().pending, 2u);
  CHECK_EQ(s.stats().pending_evictions, 1u);
  CHECK(anqp(s, ap(1)) == nullptr);
  CHECK(anqp(s, ap(3)) != nullptr);
}

TEST(gas_comeback_reassembles_three_fragments) {
  auto& s = fresh();
  beacon(s, ap(1), 1);
  Bytes stream = domains({"one.example", "two.example"});
  Bytes venue = {2, 8, 8, 'e', 'n', 'g', 'H', 'o', 't', 'e', 'l'};
  put(stream, anqp_el(258, venue));
  Bytes realm;
  put16(realm, 1);
  put(realm, {14, 0, 0, 11});
  put(realm, std::string("realm.test1"));
  put(realm, {0});
  put(stream, anqp_el(263, realm));
  // Split mid-element so only reassembly can make sense of it.
  const size_t c1 = 7, c2 = 30;
  Bytes f0(stream.begin(), stream.begin() + c1), f1(stream.begin() + c1, stream.begin() + c2),
      f2(stream.begin() + c2, stream.end());

  feed(s, gas_initial(ap(1), sta(1), ap(1), 0x33, Bytes{}, 0, 100), meta(2));
  CHECK_EQ(s.stats().anqp_completed, 0u);
  feed(s, gas_comeback(ap(1), sta(1), ap(1), 0x33, 0, true, f0), meta(3));
  feed(s, gas_comeback(ap(1), sta(1), ap(1), 0x33, 1, true, f1), meta(4));
  feed(s, gas_comeback(ap(1), sta(1), ap(1), 0x33, 1, true, f1, 0, true), meta(4));  // retry
  CHECK_EQ(s.stats().anqp_completed, 0u);
  feed(s, gas_comeback(ap(1), sta(1), ap(1), 0x33, 2, false, f2), meta(5));
  CHECK_EQ(s.stats().anqp_completed, 1u);
  auto d = detail(s, ap(1));
  CHECK(d.has_anqp);
  CHECK_EQ(d.anqp.n_domains, 2);
  CHECK_STR(d.anqp.domains[1].s, "two.example");
  CHECK_EQ(d.anqp.n_venue_names, 1);
  CHECK_STR(d.anqp.venue_names[0].name.s, "Hotel");
  CHECK_EQ(d.anqp.n_nai_realms, 1);
  CHECK_STR(d.anqp.nai_realms[0].realm.s, "realm.test1");
  CHECK_EQ(s.stats().gas_dropped, 0u);
}

TEST(gas_comeback_wildcard_and_interleaved) {
  auto& s = fresh();
  beacon(s, ap(1), 1);
  beacon(s, ap(2), 1);
  Bytes a = domains({"a.example"}), b = domains({"b.example"});
  // Two APs, same dialog token, interleaved, addr3 wildcard on one of them.
  feed(s, gas_comeback(ap(1), sta(1), kBroadcast, 4, 0, true, Bytes(a.begin(), a.begin() + 5)), meta(2));
  feed(s, gas_comeback(ap(2), sta(1), ap(2), 4, 0, true, Bytes(b.begin(), b.begin() + 3)), meta(2));
  feed(s, gas_comeback(ap(2), sta(1), ap(2), 4, 1, false, Bytes(b.begin() + 3, b.end())), meta(3));
  feed(s, gas_comeback(ap(1), sta(1), kBroadcast, 4, 1, false, Bytes(a.begin() + 5, a.end())), meta(3));
  auto r1 = anqp(s, ap(1)), r2 = anqp(s, ap(2));
  CHECK(r1 && r1->n_domains == 1 && std::string(r1->domains[0].s) == "a.example");
  CHECK(r2 && r2->n_domains == 1 && std::string(r2->domains[0].s) == "b.example");
}

TEST(gas_comeback_gaps_and_failures_drop) {
  auto& s = fresh();
  beacon(s, ap(1), 1);
  Bytes st = domains({"gap.example"});
  Bytes h1(st.begin(), st.begin() + 4), h2(st.begin() + 4, st.end());
  // Fragment 1 lost.
  feed(s, gas_comeback(ap(1), sta(1), ap(1), 1, 0, true, h1), meta(2));
  feed(s, gas_comeback(ap(1), sta(1), ap(1), 1, 2, false, h2), meta(3));
  CHECK_EQ(s.stats().anqp_completed, 0u);
  CHECK_EQ(s.stats().gas_dropped, 1u);
  // Joined mid-transaction.
  feed(s, gas_comeback(ap(1), sta(1), ap(1), 2, 1, false, h2), meta(4));
  CHECK_EQ(s.stats().gas_dropped, 2u);
  // Failed initial response.
  feed(s, gas_initial(ap(1), sta(1), ap(1), 3, Bytes{}, 61), meta(5));
  CHECK_EQ(s.stats().gas_dropped, 3u);
  // Status 95 (server response outstanding) keeps the transaction alive.
  feed(s, gas_initial(ap(1), sta(1), ap(1), 4, Bytes{}, 0, 10), meta(6));
  feed(s, gas_comeback(ap(1), sta(1), ap(1), 4, 0, false, Bytes{}, 95), meta(7));
  feed(s, gas_comeback(ap(1), sta(1), ap(1), 4, 0, false, st), meta(8));
  CHECK_EQ(s.stats().anqp_completed, 1u);
  // Any other failure status ends it.
  feed(s, gas_comeback(ap(1), sta(1), ap(1), 5, 0, true, h1), meta(9));
  feed(s, gas_comeback(ap(1), sta(1), ap(1), 5, 1, false, h2, 1), meta(10));
  feed(s, gas_comeback(ap(1), sta(1), ap(1), 5, 1, false, h2), meta(11));
  CHECK_EQ(s.stats().anqp_completed, 1u);
  // Token reused without a retry flag restarts rather than appending.
  feed(s, gas_comeback(ap(1), sta(1), ap(1), 6, 0, true, Bytes{0xde, 0xad}), meta(12));
  feed(s, gas_comeback(ap(1), sta(1), ap(1), 6, 0, false, st), meta(13));
  CHECK_EQ(s.stats().anqp_completed, 2u);
}

TEST(gas_non_anqp_and_malformed) {
  auto& s = fresh();
  beacon(s, ap(1), 1);
  feed(s, gas_initial(ap(1), sta(1), ap(1), 1, domains({"x.example"}), 0, 0, 221), meta(2));
  CHECK_EQ(s.stats().gas_non_anqp, 1u);
  CHECK_EQ(s.stats().anqp_completed, 0u);
  // Advertisement Protocol element missing.
  Bytes f = hdr(0, 13, sta(1), ap(1), ap(1));
  put(f, {0x04, 0x0b, 1, 0, 0, 0, 0, 0xdd, 2, 0, 0});
  feed(s, f, meta(3));
  // Truncated after the status code.
  Bytes g = hdr(0, 13, sta(1), ap(1), ap(1));
  put(g, {0x04, 0x0d, 1, 0, 0});
  feed(s, g, meta(3));
  // Query response length longer than the frame: clipped and counted, never over-read.
  Bytes h = gas_initial(ap(1), sta(1), ap(1), 2, domains({"clip.example"}));
  h[24 + 11] = 0xff;
  feed(s, h, meta(4));
  CHECK_EQ(s.stats().malformed, 3u);
  // GAS requests and other public actions are counted, not attributed.
  Bytes req = hdr(0, 13, ap(1), sta(1), ap(1));
  uint8_t body[64];
  const size_t n = wlc::build_anqp_query_body(body, sizeof(body), 1, wlc::QuerySet::Location);
  req.insert(req.end(), body, body + n);
  feed(s, req, meta(5));
  // Protected action frames are not parsed.
  Bytes prot = gas_initial(ap(1), sta(1), ap(1), 3, domains({"p.example"}));
  prot[1] |= 0x40;
  feed(s, prot, meta(6));
  CHECK_EQ(s.stats().gas_responses, 1u);
  CHECK_EQ(s.stats().by_type[0][13], 6u);
}

TEST(gas_reassembly_overflow_is_dropped) {
  wlc::Config cfg;
  cfg.gas_buf_bytes = 32;
  auto& s = fresh(cfg);
  beacon(s, ap(1), 1);
  Bytes big = domains({"aaaaaaaaaaaaaaaaaaaa.example", "bbbbbbbbbbbbbbbbbbbb.example"});
  feed(s, gas_comeback(ap(1), sta(1), ap(1), 1, 0, true, Bytes(big.begin(), big.begin() + 30)), meta(2));
  feed(s, gas_comeback(ap(1), sta(1), ap(1), 1, 1, false, Bytes(big.begin() + 30, big.end())), meta(3));
  CHECK_EQ(s.stats().gas_dropped, 1u);
  CHECK_EQ(s.stats().anqp_completed, 0u);
}

TEST(gas_transaction_table_evicts_oldest) {
  wlc::Config cfg;
  cfg.max_gas = 2;
  auto& s = fresh(cfg);
  for (uint8_t i = 1; i <= 3; ++i) feed(s, gas_initial(ap(i), sta(1), ap(i), 1, Bytes{}, 0, 5), meta(i));
  CHECK_EQ(s.stats().gas_evictions, 1u);
  Bytes st = domains({"e.example"});
  feed(s, gas_comeback(ap(1), sta(1), ap(1), 1, 0, false, st), meta(10));  // starts afresh
  feed(s, gas_comeback(ap(3), sta(1), ap(3), 1, 0, false, st), meta(11));
  CHECK_EQ(s.stats().anqp_completed, 2u);
}

TEST(anqp_pool_eviction_detaches) {
  wlc::Config cfg;
  cfg.max_anqp = 2;
  auto& s = fresh(cfg);
  for (uint8_t i = 1; i <= 3; ++i) {
    beacon(s, ap(i), i);
    feed(s, gas_initial(ap(i), sta(1), ap(i), i, domains({"pool.example"})), meta(10u + i));
  }
  CHECK_EQ(s.stats().anqp_evictions, 1u);
  auto d1 = detail(s, ap(1));
  CHECK(!d1.has_anqp && !d1.ap.has_anqp);
  CHECK(detail(s, ap(3)).has_anqp);
}

TEST(ap_eviction_releases_anqp) {
  wlc::Config cfg;
  cfg.max_aps = 2;
  cfg.max_anqp = 2;
  auto& s = fresh(cfg);
  beacon(s, ap(1), 1);
  feed(s, gas_initial(ap(1), sta(1), ap(1), 1, domains({"x.example"})), meta(2));
  beacon(s, ap(2), 3);
  feed(s, gas_initial(ap(2), sta(1), ap(2), 1, domains({"y.example"})), meta(4));
  beacon(s, ap(3), 5);  // evicts ap(1) and frees its record
  beacon(s, ap(2), 6);
  feed(s, gas_initial(ap(3), sta(1), ap(3), 1, domains({"z.example"})), meta(7));
  CHECK_EQ(s.stats().anqp_evictions, 0u);  // the freed slot was reused
  CHECK(detail(s, ap(2)).has_anqp);
  CHECK(detail(s, ap(3)).has_anqp);
}
