// SPDX-License-Identifier: MIT
//
// Plain value types shared by the survey tables, the ANQP decoder and the UI.
// Everything here is fixed-size and trivially copyable so snapshots are memcpy-able.
#pragma once

#include <cstddef>
#include <cstdint>

namespace wlc {

// The radio says which band it was tuned to. Channel numbers alone cannot: 6 GHz reuses 1..233.
enum class Band : uint8_t { B2G4 = 0, B5G = 1, B6G = 2 };

enum BandMask : uint8_t {
  BAND_2G4 = 1u << 0,
  BAND_5G = 1u << 1,
  BAND_6G = 1u << 2,
  BAND_ALL = BAND_2G4 | BAND_5G | BAND_6G,
};

inline uint8_t band_bit(Band b) { return static_cast<uint8_t>(1u << static_cast<uint8_t>(b)); }

// `rssi >= 0` is treated as "no measurement" and never folded into averages.
struct RxMeta {
  int8_t rssi;
  uint8_t channel;
  Band band;
  uint32_t now_ms;
};

// Bounded display string. Bytes are kept as received (UTF-8 passes through), control bytes
// become '?', and a value that did not fit is cut on a UTF-8 boundary with `truncated` set.
template <size_t N>
struct Text {
  static_assert(N >= 2 && N <= 256, "Text capacity");
  char s[N];
  uint8_t len;
  bool truncated;

  void clear() {
    s[0] = '\0';
    len = 0;
    truncated = false;
  }
  // Returns false when the value was truncated.
  bool set(const uint8_t* p, size_t n);
  bool empty() const { return len == 0; }
};

// Copies `n` bytes into `dst` (capacity `cap`, NUL included) per the Text rules above.
// Returns the stored length; `*truncated` reports whether anything was dropped.
size_t copy_text(char* dst, size_t cap, const uint8_t* src, size_t n, bool* truncated);

template <size_t N>
bool Text<N>::set(const uint8_t* p, size_t n) {
  bool t = false;
  len = static_cast<uint8_t>(copy_text(s, N, p, n, &t));
  truncated = t;
  return !t;
}

// Roaming consortium organisation identifier, 3..15 octets.
struct OI {
  uint8_t len;
  uint8_t b[15];
};

// Lowercase, colon separated — the same form as WLAN Commander core's format_mac.
// `out` must hold 18 bytes. Returns `out`.
char* mac_to_string(const uint8_t mac[6], char out[18]);

inline bool mac_is_group(const uint8_t* mac) { return (mac[0] & 0x01) != 0; }
inline bool mac_is_local(const uint8_t* mac) { return (mac[0] & 0x02) != 0; }

}  // namespace wlc
