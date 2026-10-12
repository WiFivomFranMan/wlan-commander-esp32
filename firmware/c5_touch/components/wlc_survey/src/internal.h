// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wlc {
namespace detail {

inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline bool mac_eq(const uint8_t* a, const uint8_t* b) { return std::memcmp(a, b, 6) == 0; }
inline bool mac_is_broadcast(const uint8_t* m) {
  static const uint8_t ff[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  return mac_eq(m, ff);
}
inline bool mac_is_zero(const uint8_t* m) {
  static const uint8_t z[6] = {0, 0, 0, 0, 0, 0};
  return mac_eq(m, z);
}

// Walks [id][len][body] elements. Stops at the first element whose body would overrun.
struct IeWalker {
  const uint8_t* p;
  size_t n;
  size_t pos = 0;
  bool overrun = false;

  IeWalker(const uint8_t* data, size_t len) : p(data), n(len) {}

  bool next(uint8_t* id, const uint8_t** body, uint8_t* len) {
    if (pos + 2 > n) {
      overrun = pos != n;  // a single dangling byte is malformed too
      return false;
    }
    const uint8_t l = p[pos + 1];
    if (pos + 2 + l > n) {
      overrun = true;
      return false;
    }
    *id = p[pos];
    *len = l;
    *body = p + pos + 2;
    pos += 2u + l;
    return true;
  }
};

// Open-addressed MAC -> slot index, linear probing with backward-shift delete, so eviction
// leaves no tombstones and lookups stay O(1) for the life of a long survey.
class MacIndex {
 public:
  static constexpr uint16_t kEmpty = 0xFFFF;
  struct Slot {
    uint8_t mac[6];
    uint16_t idx;
  };

  void attach(Slot* slots, size_t pow2) {
    slots_ = slots;
    mask_ = pow2 - 1;
    clear();
  }
  void clear() {
    for (size_t i = 0; i <= mask_; ++i) slots_[i].idx = kEmpty;
  }
  static size_t slots_for(size_t capacity) {
    size_t s = 4;
    while (s < capacity * 2) s <<= 1;
    return s;
  }

  int find(const uint8_t* mac) const {
    for (size_t i = hash(mac);; i = (i + 1) & mask_) {
      const Slot& s = slots_[i];
      if (s.idx == kEmpty) return -1;
      if (mac_eq(s.mac, mac)) return s.idx;
    }
  }
  void insert(const uint8_t* mac, uint16_t idx) {
    size_t i = hash(mac);
    while (slots_[i].idx != kEmpty && !mac_eq(slots_[i].mac, mac)) i = (i + 1) & mask_;
    std::memcpy(slots_[i].mac, mac, 6);
    slots_[i].idx = idx;
  }
  void erase(const uint8_t* mac) {
    size_t i = hash(mac);
    for (;; i = (i + 1) & mask_) {
      if (slots_[i].idx == kEmpty) return;
      if (mac_eq(slots_[i].mac, mac)) break;
    }
    slots_[i].idx = kEmpty;
    // Pull later members of the probe run back over the hole so find() never stops early.
    for (size_t j = (i + 1) & mask_; slots_[j].idx != kEmpty; j = (j + 1) & mask_) {
      const size_t home = hash(slots_[j].mac);
      const bool movable = (i <= j) ? (home <= i || home > j) : (home <= i && home > j);
      if (movable) {
        slots_[i] = slots_[j];
        slots_[j].idx = kEmpty;
        i = j;
      }
    }
  }

 private:
  size_t hash(const uint8_t* m) const {
    // Vendors allocate sequentially from the low octets, so mix all six.
    uint32_t h = 2166136261u;
    for (int k = 0; k < 6; ++k) h = (h ^ m[k]) * 16777619u;
    return (h ^ (h >> 15)) & mask_;
  }
  Slot* slots_ = nullptr;
  size_t mask_ = 0;
};

}  // namespace detail
}  // namespace wlc
