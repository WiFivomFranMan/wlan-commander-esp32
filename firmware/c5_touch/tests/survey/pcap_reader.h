// SPDX-License-Identifier: MIT
//
// Host-only test helper: classic pcap and pcapng, linktype 127 (radiotap) and 105 (bare
// 802.11). Yields MPDUs ready for wlc::Survey::ingest — radiotap stripped, FCS stripped when
// radiotap says it is present, RSSI and band/channel taken from radiotap. Not part of the
// library.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "wlc_survey/types.h"

namespace pcap {

struct Radiotap {
  size_t header_len = 0;
  bool has_flags = false;
  uint8_t flags = 0;
  bool has_channel = false;
  uint16_t freq = 0;
  bool has_signal = false;
  int8_t signal = 0;
};

// Parses the radiotap header at `p`. False when it is not a well-formed v0 header.
bool parse_radiotap(const uint8_t* p, size_t n, Radiotap* out);

// MHz -> band/channel. False for frequencies outside the three Wi-Fi bands.
bool freq_to_channel(uint16_t freq, wlc::Band* band, uint8_t* channel);

uint32_t crc32_ieee(const uint8_t* p, size_t n);

struct Packet {
  const uint8_t* frame = nullptr;  // starts at Frame Control
  size_t len = 0;
  wlc::RxMeta meta = {};
  uint64_t ts_us = 0;
  uint16_t freq = 0;
  uint32_t index = 0;  // 1-based record number in the file, as tshark numbers frames
};

class Reader {
 public:
  bool open(const std::string& path, std::string* err);
  bool open_memory(std::vector<uint8_t> bytes, std::string* err);
  // Next usable 802.11 frame. Frames with a bad FCS (flagged or recomputed) are skipped and
  // counted, as are records on other linktypes.
  bool next(Packet* out);

  wlc::Band default_band = wlc::Band::B2G4;  // for linktype 105, which carries no channel
  uint32_t bad_fcs = 0;
  uint32_t skipped = 0;
  uint32_t records = 0;

 private:
  bool next_record(const uint8_t** data, size_t* len, uint32_t* linktype, uint64_t* ts_us);
  uint16_t u16(const uint8_t* p) const;
  uint32_t u32(const uint8_t* p) const;

  std::vector<uint8_t> buf_;
  size_t pos_ = 0;
  bool ng_ = false;
  bool swap_ = false;
  bool nanos_ = false;
  uint32_t linktype_ = 0;
  struct Iface {
    uint32_t linktype;
    uint64_t ticks_per_sec;
  };
  std::vector<Iface> ifaces_;
  uint64_t first_ts_ = 0;
  bool have_first_ = false;
};

}  // namespace pcap
