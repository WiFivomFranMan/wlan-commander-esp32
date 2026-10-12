// SPDX-License-Identifier: MIT
#include "pcap_reader.h"

#include <cstdio>
#include <cstring>

namespace pcap {

namespace {
constexpr uint32_t kLinktype80211 = 105;
constexpr uint32_t kLinktypeRadiotap = 127;

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
}  // namespace

bool parse_radiotap(const uint8_t* p, size_t n, Radiotap* out) {
  *out = Radiotap();
  if (n < 8 || p[0] != 0) return false;
  const size_t len = le16(p + 2);
  if (len < 8 || len > n) return false;
  out->header_len = len;
  size_t off = 4;
  const uint32_t present = le32(p + 4);
  uint32_t word = present;
  while (word & 0x80000000u) {  // extended bitmaps precede all fields
    off += 4;
    if (off + 4 > len) return false;
    word = le32(p + off);
  }
  off += 4;
  // Only the first namespace's bits 0..5 matter here; each field is aligned to its size.
  static const struct {
    uint8_t align, size;
  } fields[6] = {{8, 8}, {1, 1}, {1, 1}, {2, 4}, {1, 2}, {1, 1}};
  for (unsigned bit = 0; bit < 6; ++bit) {
    if (!(present & (1u << bit))) continue;
    off = (off + fields[bit].align - 1) & ~size_t(fields[bit].align - 1);
    if (off + fields[bit].size > len) return false;
    if (bit == 1) {
      out->has_flags = true;
      out->flags = p[off];
    } else if (bit == 3) {
      out->has_channel = true;
      out->freq = le16(p + off);
    } else if (bit == 5) {
      out->has_signal = true;
      out->signal = int8_t(p[off]);
    }
    off += fields[bit].size;
  }
  return true;
}

bool freq_to_channel(uint16_t f, wlc::Band* band, uint8_t* ch) {
  if (f == 2484) {
    *band = wlc::Band::B2G4;
    *ch = 14;
  } else if (f >= 2412 && f <= 2472) {
    *band = wlc::Band::B2G4;
    *ch = uint8_t((f - 2407) / 5);
  } else if (f == 5935) {
    *band = wlc::Band::B6G;
    *ch = 2;
  } else if (f >= 5955 && f <= 7115) {
    *band = wlc::Band::B6G;
    *ch = uint8_t((f - 5950) / 5);
  } else if (f >= 5150 && f <= 5925) {
    *band = wlc::Band::B5G;
    *ch = uint8_t((f - 5000) / 5);
  } else {
    return false;
  }
  return true;
}

uint32_t crc32_ieee(const uint8_t* p, size_t n) {
  uint32_t c = 0xffffffffu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
  }
  return ~c;
}

uint16_t Reader::u16(const uint8_t* p) const { return swap_ ? uint16_t(p[0] << 8 | p[1]) : le16(p); }
uint32_t Reader::u32(const uint8_t* p) const {
  return swap_ ? (uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]) : le32(p);
}

bool Reader::open(const std::string& path, std::string* err) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    *err = "cannot open " + path;
    return false;
  }
  std::vector<uint8_t> b;
  uint8_t chunk[65536];
  size_t n;
  while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) b.insert(b.end(), chunk, chunk + n);
  std::fclose(f);
  return open_memory(std::move(b), err);
}

bool Reader::open_memory(std::vector<uint8_t> bytes, std::string* err) {
  buf_ = std::move(bytes);
  pos_ = 0;
  ifaces_.clear();
  have_first_ = false;
  if (buf_.size() < 24) {
    *err = "file too short";
    return false;
  }
  const uint32_t magic = le32(buf_.data());
  if (magic == 0x0a0d0d0a) {
    ng_ = true;
    return true;  // SHB is parsed as the first block
  }
  ng_ = false;
  if (magic == 0xa1b2c3d4 || magic == 0xa1b23c4d) {
    swap_ = false;
  } else if (magic == 0xd4c3b2a1 || magic == 0x4d3cb2a1) {
    swap_ = true;
  } else {
    *err = "not a pcap or pcapng file";
    return false;
  }
  nanos_ = magic == 0xa1b23c4d || magic == 0x4d3cb2a1;
  linktype_ = u32(buf_.data() + 20) & 0x0fffffff;
  pos_ = 24;
  return true;
}

bool Reader::next_record(const uint8_t** data, size_t* len, uint32_t* linktype, uint64_t* ts_us) {
  const size_t size = buf_.size();
  if (!ng_) {
    if (pos_ + 16 > size) return false;
    const uint8_t* h = buf_.data() + pos_;
    const uint32_t incl = u32(h + 8);
    if (pos_ + 16 + incl > size) return false;
    *data = h + 16;
    *len = incl;
    *linktype = linktype_;
    *ts_us = uint64_t(u32(h)) * 1000000 + (nanos_ ? u32(h + 4) / 1000 : u32(h + 4));
    pos_ += 16 + incl;
    return true;
  }
  while (pos_ + 12 <= size) {
    const uint8_t* b = buf_.data() + pos_;
    if (le32(b) == 0x0a0d0d0a) {  // SHB: byte order decides everything after it
      const uint32_t bom = le32(b + 8);
      if (bom == 0x1a2b3c4d) {
        swap_ = false;
      } else if (bom == 0x4d3c2b1a) {
        swap_ = true;
      } else {
        return false;
      }
      ifaces_.clear();
    }
    const uint32_t type = u32(b);
    const uint32_t blen = u32(b + 4);
    if (blen < 12 || blen % 4 != 0 || pos_ + blen > size) return false;
    pos_ += blen;
    if (type == 1 && blen >= 20) {  // IDB
      Iface ifc = {u16(b + 8), 1000000};
      for (size_t o = 16; o + 4 <= blen - 4;) {  // options
        const uint16_t code = u16(b + o), olen = u16(b + o + 2);
        if (code == 0 || o + 4 + olen > blen - 4) break;
        if (code == 9 && olen >= 1) {  // if_tsresol
          const uint8_t r = b[o + 4];
          uint64_t t = 1;
          for (int k = 0; k < (r & 0x7f) && k < 19; ++k) t *= (r & 0x80) ? 2 : 10;
          ifc.ticks_per_sec = t;
        }
        o += 4 + ((olen + 3u) & ~3u);
      }
      ifaces_.push_back(ifc);
    } else if (type == 6 && blen >= 32) {  // EPB
      const uint32_t id = u32(b + 8);
      const uint32_t cap = u32(b + 20);
      if (id >= ifaces_.size() || 28 + cap > blen - 4) continue;
      const uint64_t ticks = (uint64_t(u32(b + 12)) << 32) | u32(b + 16);
      *data = b + 28;
      *len = cap;
      *linktype = ifaces_[id].linktype;
      *ts_us = ticks * 1000000 / ifaces_[id].ticks_per_sec;
      return true;
    } else if (type == 3 && blen >= 16) {  // SPB: interface 0, no timestamp
      if (ifaces_.empty()) continue;
      const uint32_t orig = u32(b + 8);
      const uint32_t cap = orig < blen - 16 ? orig : blen - 16;
      *data = b + 12;
      *len = cap;
      *linktype = ifaces_[0].linktype;
      *ts_us = 0;
      return true;
    }
  }
  return false;
}

bool Reader::next(Packet* out) {
  const uint8_t* d;
  size_t n;
  uint32_t lt;
  uint64_t ts;
  while (next_record(&d, &n, &lt, &ts)) {
    ++records;
    Packet p;
    p.index = records;
    p.ts_us = ts;
    if (!have_first_) {
      first_ts_ = ts;
      have_first_ = true;
    }
    p.meta.now_ms = uint32_t((ts >= first_ts_ ? ts - first_ts_ : 0) / 1000);
    p.meta.band = default_band;
    if (lt == kLinktypeRadiotap) {
      Radiotap rt;
      if (!parse_radiotap(d, n, &rt)) {
        ++skipped;
        continue;
      }
      d += rt.header_len;
      n -= rt.header_len;
      if (rt.has_flags && (rt.flags & 0x40)) {
        ++bad_fcs;
        continue;
      }
      if (rt.has_flags && (rt.flags & 0x10)) {
        if (n < 4) {
          ++skipped;
          continue;
        }
        n -= 4;
        if (crc32_ieee(d, n) != le32(d + n)) {
          ++bad_fcs;
          continue;
        }
      }
      p.meta.rssi = rt.has_signal ? rt.signal : 0;
      p.freq = rt.freq;
      if (!rt.has_channel || !freq_to_channel(rt.freq, &p.meta.band, &p.meta.channel)) {
        p.meta.band = default_band;
        p.meta.channel = 0;
      }
    } else if (lt != kLinktype80211) {
      ++skipped;
      continue;
    }
    p.frame = d;
    p.len = n;
    *out = p;
    return true;
  }
  return false;
}

}  // namespace pcap
