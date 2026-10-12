// SPDX-License-Identifier: MIT
#include "harness.h"
#include "pcap_reader.h"

using namespace t;

namespace {

Bytes be32(uint32_t v) { return {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; }

Bytes with_fcs(const Bytes& mpdu) {
  Bytes b = mpdu;
  put32(b, pcap::crc32_ieee(mpdu.data(), mpdu.size()));
  return b;
}

// Radiotap: TSFT, Flags, Rate, Channel, dBm signal — exercising 8- and 2-byte alignment.
Bytes radiotap(uint16_t freq, int8_t sig, uint8_t flags, bool ext = false) {
  Bytes b = {0, 0, 0, 0};
  put32(b, (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 5) | (ext ? 0x80000000u : 0));
  if (ext) put32(b, 0);  // second bitmap word, then pad to 8 for TSFT
  while (b.size() % 8) b.push_back(0);
  for (int i = 0; i < 8; ++i) b.push_back(uint8_t(i));
  put(b, {flags, 12});
  put16(b, freq);
  put16(b, 0x0140);
  b.push_back(uint8_t(sig));
  b[2] = uint8_t(b.size());
  return b;
}

Bytes classic(const std::vector<Bytes>& recs, uint32_t linktype) {
  Bytes f;
  put32(f, 0xa1b2c3d4);
  put16(f, 2);
  put16(f, 4);
  put32(f, 0);
  put32(f, 0);
  put32(f, 65535);
  put32(f, linktype);
  uint32_t sec = 100;
  for (const auto& r : recs) {
    put32(f, sec++);
    put32(f, 500000);
    put32(f, uint32_t(r.size()));
    put32(f, uint32_t(r.size()));
    put(f, r);
  }
  return f;
}

Bytes block(uint32_t type, const Bytes& body) {
  Bytes b;
  Bytes padded = body;
  while (padded.size() % 4) padded.push_back(0);
  put32(b, type);
  put32(b, uint32_t(12 + padded.size()));
  put(b, padded);
  put32(b, uint32_t(12 + padded.size()));
  return b;
}

}  // namespace

TEST(pcap_freq_and_crc) {
  struct {
    uint16_t f;
    wlc::Band band;
    uint8_t ch;
  } ok[] = {{2412, wlc::Band::B2G4, 1},  {2484, wlc::Band::B2G4, 14}, {5180, wlc::Band::B5G, 36},
            {5825, wlc::Band::B5G, 165}, {5935, wlc::Band::B6G, 2},   {5955, wlc::Band::B6G, 1},
            {6115, wlc::Band::B6G, 33},  {7115, wlc::Band::B6G, 233}};
  for (const auto& c : ok) {
    wlc::Band b;
    uint8_t ch;
    CHECK(pcap::freq_to_channel(c.f, &b, &ch));
    CHECK(b == c.band);
    CHECK_EQ(ch, c.ch);
  }
  wlc::Band b;
  uint8_t ch;
  CHECK(!pcap::freq_to_channel(900, &b, &ch));
  const char* v = "123456789";
  CHECK_EQ(pcap::crc32_ieee(reinterpret_cast<const uint8_t*>(v), 9), 0xcbf43926u);
}

TEST(pcap_classic_radiotap_fcs) {
  const Bytes beacon = bss_frame(ap(1), ie(0, "pc"));
  Bytes good = radiotap(5180, -42, 0x10);
  put(good, with_fcs(beacon));
  Bytes corrupt = good;
  corrupt[corrupt.size() - 10] ^= 0x01;
  Bytes flagged = radiotap(5180, -42, 0x50);
  put(flagged, with_fcs(beacon));
  Bytes nofcs = radiotap(2437, -77, 0x00, true);
  put(nofcs, beacon);

  pcap::Reader r;
  std::string err;
  CHECK(r.open_memory(classic({good, corrupt, flagged, nofcs}, 127), &err));
  pcap::Packet p;
  CHECK(r.next(&p));
  CHECK_EQ(p.len, beacon.size());
  CHECK(std::memcmp(p.frame, beacon.data(), beacon.size()) == 0);
  CHECK_EQ(p.meta.rssi, -42);
  CHECK(p.meta.band == wlc::Band::B5G);
  CHECK_EQ(p.meta.channel, 36);
  CHECK_EQ(p.meta.now_ms, 0u);
  CHECK_EQ(p.index, 1u);
  CHECK(r.next(&p));
  CHECK_EQ(p.index, 4u);
  CHECK_EQ(p.len, beacon.size());
  CHECK_EQ(p.meta.rssi, -77);
  CHECK_EQ(p.meta.channel, 6);
  CHECK_EQ(p.meta.now_ms, 3000u);
  CHECK(!r.next(&p));
  CHECK_EQ(r.bad_fcs, 2u);
}

TEST(pcap_swapped_nanosecond_80211) {
  const Bytes beacon = bss_frame(ap(2), ie(0, "be"));
  Bytes f;
  put(f, be32(0xa1b23c4d));
  put(f, {0, 2, 0, 4});
  put(f, be32(0));
  put(f, be32(0));
  put(f, be32(65535));
  put(f, be32(105));
  put(f, be32(7));
  put(f, be32(250000000));
  put(f, be32(uint32_t(beacon.size())));
  put(f, be32(uint32_t(beacon.size())));
  put(f, beacon);
  pcap::Reader r;
  r.default_band = wlc::Band::B5G;
  std::string err;
  CHECK(r.open_memory(f, &err));
  pcap::Packet p;
  CHECK(r.next(&p));
  CHECK_EQ(p.ts_us, 7250000ull);
  CHECK(p.meta.band == wlc::Band::B5G);
  CHECK_EQ(p.meta.channel, 0);
  CHECK_EQ(p.meta.rssi, 0);
  CHECK_EQ(p.len, beacon.size());
}

TEST(pcapng_blocks) {
  const Bytes beacon = bss_frame(ap(3), ie(0, "ng"));
  Bytes rec = radiotap(5955, -60, 0);
  put(rec, beacon);

  Bytes shb;
  put32(shb, 0x1a2b3c4d);
  put16(shb, 1);
  put16(shb, 0);
  put32(shb, 0xffffffff);
  put32(shb, 0xffffffff);
  Bytes idb_rt;
  put16(idb_rt, 127);
  put16(idb_rt, 0);
  put32(idb_rt, 0);
  put(idb_rt, {9, 0, 1, 0, 9, 0, 0, 0});  // if_tsresol = 10^-9
  put(idb_rt, {0, 0, 0, 0});              // opt_endofopt
  Bytes idb_eth;
  put16(idb_eth, 1);
  put16(idb_eth, 0);
  put32(idb_eth, 0);
  auto epb = [](uint32_t iface, uint64_t ticks, const Bytes& data) {
    Bytes b;
    put32(b, iface);
    put32(b, uint32_t(ticks >> 32));
    put32(b, uint32_t(ticks));
    put32(b, uint32_t(data.size()));
    put32(b, uint32_t(data.size()));
    put(b, data);
    return block(6, b);
  };
  Bytes spb;
  put32(spb, uint32_t(rec.size()));
  put(spb, rec);

  Bytes file = block(0x0a0d0d0a, shb);
  put(file, block(1, idb_rt));
  put(file, block(1, idb_eth));
  put(file, epb(1, 5, Bytes(60, 0xee)));               // ethernet: skipped
  put(file, epb(0, 2000000000ull, rec));               // 2 s in ns ticks
  put(file, epb(7, 1, rec));                           // no such interface: ignored
  put(file, block(3, spb));
  put(file, block(0xbad, Bytes{1, 2, 3, 4}));          // unknown block type
  pcap::Reader r;
  std::string err;
  CHECK(r.open_memory(file, &err));
  pcap::Packet p;
  CHECK(r.next(&p));
  CHECK_EQ(p.ts_us, 2000000ull);
  CHECK(p.meta.band == wlc::Band::B6G);
  CHECK_EQ(p.meta.channel, 1);
  CHECK_EQ(p.meta.rssi, -60);
  CHECK_EQ(p.len, beacon.size());
  CHECK(r.next(&p));  // SPB
  CHECK_EQ(p.len, beacon.size());
  CHECK(!r.next(&p));
  CHECK_EQ(r.skipped, 1u);

  CHECK(!r.open_memory(Bytes(10, 0), &err));
  CHECK(!r.open_memory(Bytes(40, 0x55), &err));
}
