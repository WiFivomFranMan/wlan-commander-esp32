// SPDX-License-Identifier: MIT
#include <cstdlib>

#include "harness.h"

using namespace t;

namespace {

int g_allocs = 0, g_frees = 0;
size_t g_alloc_bytes = 0;
void* counting_alloc(size_t n) {
  ++g_allocs;
  g_alloc_bytes = n;
  return std::malloc(n);
}
void counting_free(void* p) {
  ++g_frees;
  std::free(p);
}

class CountingLock final : public wlc::Lock {
 public:
  int depth = 0, max_depth = 0, locks = 0;
  void lock() override {
    ++locks;
    if (++depth > max_depth) max_depth = depth;
  }
  void unlock() override { --depth; }
};

}  // namespace

TEST(snapshot_sorting_and_filters) {
  wlc::Survey s;
  CHECK(s.init());
  Bytes pp = ie(0, "beta");
  put(pp, ie(107, Bytes{0x02}));
  feed(s, bss_frame(ap(1), ie(0, "Charlie"), 0x401), meta(10, -70, 11));
  feed(s, bss_frame(ap(2), pp, 0x401), meta(20, -40, 1));
  feed(s, bss_frame(ap(3), ie(0, "alpha"), 0x401), meta(30, -60, 36, wlc::Band::B5G));
  feed(s, bss_frame(ap(4), ie(0, Bytes{}), 0x401), meta(40, -50, 37, wlc::Band::B6G));

  wlc::ApInfo rows[8];
  size_t total = 0;
  auto order = [&](wlc::ApSort sort, uint8_t mask = wlc::BAND_ALL, bool pp_only = false) {
    std::string o;
    const size_t n = s.aps(rows, 8, sort, mask, pp_only, &total);
    for (size_t i = 0; i < n; ++i) o += char('0' + rows[i].bssid[5]);
    return o;
  };
  CHECK_STR(order(wlc::ApSort::Rssi), "2431");
  CHECK_STR(order(wlc::ApSort::Ssid), "3214");  // case-insensitive, hidden last
  CHECK_STR(order(wlc::ApSort::Channel), "2134");
  CHECK_STR(order(wlc::ApSort::LastSeen), "4321");
  CHECK_STR(order(wlc::ApSort::Rssi, wlc::BAND_2G4), "21");
  CHECK_EQ(total, 2u);
  CHECK_STR(order(wlc::ApSort::Rssi, wlc::BAND_5G | wlc::BAND_6G), "43");
  CHECK_STR(order(wlc::ApSort::Rssi, wlc::BAND_ALL, true), "");
  CHECK_EQ(s.aps(rows, 8, wlc::ApSort::Rssi, wlc::BAND_ALL, true, &total, true), 1u);
  CHECK(!rows[0].hs20 && rows[0].interworking && !rows[0].passpoint);
  CHECK_EQ(s.aps(rows, 2, wlc::ApSort::Rssi, wlc::BAND_ALL, false, &total), 2u);
  CHECK_EQ(total, 4u);
  CHECK_EQ(s.aps(rows, 0, wlc::ApSort::Rssi), 0u);
}

TEST(stats_and_reset) {
  wlc::Survey s;
  CHECK(s.init());
  feed(s, bss_frame(ap(1), ie(0, "a")), meta(100));
  feed(s, bss_frame(ap(1), ie(0, "a"), 0x401, 5), meta(110));
  feed(s, hdr(2, 0, ap(1), sta(1), ap(1), 0x01), meta(120));
  feed(s, Bytes{0xd4, 0, 0, 0, 1, 2, 3, 4, 5, 6}, meta(130));  // ACK
  auto st = s.stats();
  CHECK_EQ(st.frames, 4u);
  CHECK_EQ(st.by_type[0][8], 1u);
  CHECK_EQ(st.by_type[0][5], 1u);
  CHECK_EQ(st.by_type[2][0], 1u);
  CHECK_EQ(st.by_type[1][13], 1u);
  CHECK_EQ(st.last_ingest_ms, 130u);
  CHECK_EQ(st.aps, 1u);
  CHECK_EQ(st.clients, 1u);
  s.reset();
  st = s.stats();
  CHECK_EQ(st.frames, 0u);
  CHECK_EQ(st.aps, 0u);
  CHECK_EQ(st.clients, 0u);
  wlc::ApDetail d;
  CHECK(!s.ap_detail(ap(1).b, &d));
  // Tables still work after a reset.
  feed(s, bss_frame(ap(1), ie(0, "a")), meta(200));
  CHECK(s.ap_detail(ap(1).b, &d));
}

TEST(allocator_and_lock_hooks) {
  CountingLock lock;
  g_allocs = g_frees = 0;
  {
    wlc::Config cfg;
    cfg.alloc = counting_alloc;
    cfg.dealloc = counting_free;
    cfg.lock = &lock;
    wlc::Survey s;
    CHECK(s.init(cfg));
    CHECK(!s.init(cfg));  // one-shot
    CHECK_EQ(g_allocs, 1);
    CHECK_EQ(g_alloc_bytes, wlc::Survey::memory_required(cfg));
    for (int i = 0; i < 100; ++i) feed(s, bss_frame(ap(uint8_t(i)), ie(0, "x")), meta(uint32_t(i)));
    CHECK_EQ(g_allocs, 1);  // nothing on the frame path
    wlc::ApInfo rows[4];
    s.aps(rows, 4, wlc::ApSort::Rssi);
    s.stats();
    CHECK_EQ(lock.depth, 0);
    CHECK_EQ(lock.max_depth, 1);
    CHECK(lock.locks >= 102);
  }
  CHECK_EQ(g_frees, 1);

  wlc::Config bad;
  bad.max_aps = 0;
  wlc::Survey s2;
  CHECK(!s2.init(bad));
  wlc::Config nomem;
  nomem.alloc = [](size_t) -> void* { return nullptr; };
  CHECK(!s2.init(nomem));
  // An uninitialised survey is inert, not a crash.
  feed(s2, bss_frame(ap(1), ie(0, "x")), meta(1));
  CHECK_EQ(s2.stats().frames, 0u);
}

TEST(memory_budget_defaults) {
  const size_t bytes = wlc::Survey::memory_required(wlc::Config());
  std::printf("  default Config needs %zu bytes (ApInfo %zu, ClientInfo %zu, AnqpRecord %zu)\n", bytes,
              sizeof(wlc::ApInfo), sizeof(wlc::ClientInfo), sizeof(wlc::AnqpRecord));
  CHECK(bytes < 1024 * 1024);
}

TEST(formatting_helpers) {
  char m[18];
  const uint8_t up[6] = {0xAB, 0xCD, 0xEF, 0x01, 0x02, 0x0A};
  CHECK_STR(wlc::mac_to_string(up, m), "ab:cd:ef:01:02:0a");
  CHECK_STR(wlc::security_label(wlc::Security::WPA2WPA3), "WPA2/WPA3");
  CHECK_STR(wlc::security_label(wlc::Security::Enterprise, true), "Enterprise (Suite-B)");
  CHECK_STR(wlc::security_label(wlc::Security::OWE), "OWE");
  CHECK_STR(wlc::width_label(wlc::Width::W320), "320 MHz");
  CHECK_STR(wlc::phy_label(wlc::PHY_HT | wlc::PHY_HE), "Wi-Fi 6");
  CHECK_STR(wlc::band_label(wlc::Band::B6G), "6 GHz");
  CHECK_STR(wlc::access_network_type_label(2), "Chargeable public");
  CHECK_STR(wlc::eap_method_label(50), "EAP-AKA'");
  CHECK_STR(wlc::anqp_element_label(271), "Emergency NAI");
  char ssid[33];
  const uint8_t raw[] = {'a', 0x01, 'b'};
  CHECK_STR(wlc::ssid_to_string(raw, 3, ssid, sizeof(ssid)), "a?b");
  wlc::NaiEap e = {};
  e.method = 18;
  e.credential = 1;
  char sum[40];
  CHECK_STR(wlc::nai_eap_summary(e, sum, sizeof(sum)), "EAP-SIM (SIM)");
  e = {};
  e.method = 25;
  CHECK_STR(wlc::nai_eap_summary(e, sum, sizeof(sum)), "PEAP");
  CHECK_STR(wlc::nai_eap_summary(e, sum, 3), "PE");
}

TEST(copy_text_rules) {
  char out[8];
  bool tr = false;
  const uint8_t a[] = {'h', 'i'};
  CHECK_EQ(wlc::copy_text(out, sizeof(out), a, 2, &tr), 2u);
  CHECK(!tr);
  const uint8_t b[] = {'1', '2', '3', '4', '5', '6', 0xe2, 0x82, 0xac};  // ends in a 3-byte euro sign
  CHECK_EQ(wlc::copy_text(out, sizeof(out), b, sizeof(b), &tr), 6u);
  CHECK(tr);
  CHECK_STR(out, "123456");
  const uint8_t c[] = {0x7f, 0x00, 'z'};
  wlc::copy_text(out, sizeof(out), c, 3, &tr);
  CHECK_STR(out, "??z");
  CHECK_EQ(wlc::copy_text(out, 0, c, 3, &tr), 0u);
  CHECK(tr);
}

TEST(passpoint_scope_defaults_to_hs20_and_opt_in_keeps_identity) {
  wlc::Survey s;
  CHECK(s.init());
  Bytes hs = ie(0, "hs20-only");
  put(hs, ie(221, Bytes{0x50, 0x6f, 0x9a, 0x10, 0x10}));
  Bytes iw = ie(0, "interworking-only");
  put(iw, ie(107, Bytes{0x02}));
  Bytes both = hs;
  put(both, ie(107, Bytes{0x02}));
  feed(s, bss_frame(ap(1), hs), meta(1));
  feed(s, bss_frame(ap(2), iw), meta(2));
  feed(s, bss_frame(ap(3), both), meta(3));
  feed(s, bss_frame(ap(4), ie(0, "ordinary")), meta(4));
  wlc::ApInfo rows[8];
  size_t total = 0;
  CHECK_EQ(s.aps(rows, 8, wlc::ApSort::LastSeen, wlc::BAND_ALL, true, &total), 2u);
  CHECK_EQ(total, 2u);
  CHECK(rows[0].hs20 && rows[1].hs20);
  CHECK_EQ(s.aps(rows, 8, wlc::ApSort::LastSeen, wlc::BAND_ALL, true, &total, true), 3u);
  CHECK_EQ(total, 3u);
  CHECK(!rows[1].hs20 && rows[1].interworking && !rows[1].passpoint);
  // Filter before the row cap: a stronger Interworking-only AP cannot displace HS2.0.
  CHECK_EQ(s.aps(rows, 1, wlc::ApSort::LastSeen, wlc::BAND_ALL, true, &total), 1u);
  CHECK_EQ(total, 2u);
  CHECK(rows[0].hs20);
}
