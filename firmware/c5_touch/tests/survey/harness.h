// SPDX-License-Identifier: MIT
//
// Minimal test registry plus 802.11 frame builders for the wlc_survey unit tests.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "wlc_survey/survey.h"

namespace t {

using Bytes = std::vector<uint8_t>;

struct Case {
  const char* name;
  void (*fn)();
};
std::vector<Case>& registry();
struct Reg {
  Reg(const char* n, void (*f)()) { registry().push_back({n, f}); }
};
extern int g_failures;
void fail(const char* file, int line, const std::string& msg);

#define TEST(name)                          \
  static void name();                       \
  static ::t::Reg reg_##name(#name, name);  \
  static void name()

#define CHECK(cond)                                         \
  do {                                                      \
    if (!(cond)) ::t::fail(__FILE__, __LINE__, #cond);      \
  } while (0)

#define CHECK_EQ(a, b)                                                                   \
  do {                                                                                   \
    const auto va_ = (a);                                                                \
    const auto vb_ = (b);                                                                \
    if (!(va_ == vb_)) {                                                                 \
      ::t::fail(__FILE__, __LINE__,                                                      \
                std::string(#a " == " #b "  (") + std::to_string(static_cast<long long>(va_)) + \
                    " vs " + std::to_string(static_cast<long long>(vb_)) + ")");          \
    }                                                                                    \
  } while (0)

#define CHECK_STR(a, b)                                                                     \
  do {                                                                                      \
    const std::string va_ = (a);                                                            \
    const std::string vb_ = (b);                                                            \
    if (va_ != vb_) ::t::fail(__FILE__, __LINE__, std::string(#a " == \"") + vb_ + "\"  (got \"" + va_ + "\")"); \
  } while (0)

// ---- byte helpers --------------------------------------------------------------------------

inline void put(Bytes& b, std::initializer_list<uint8_t> v) { b.insert(b.end(), v.begin(), v.end()); }
inline void put(Bytes& b, const Bytes& v) { b.insert(b.end(), v.begin(), v.end()); }
inline void put(Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
inline void put16(Bytes& b, uint16_t v) { put(b, {uint8_t(v & 0xff), uint8_t(v >> 8)}); }
inline void put32(Bytes& b, uint32_t v) {
  put(b, {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)});
}

struct Mac {
  uint8_t b[6];
};
inline Mac mac(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e, uint8_t f) { return {{a, b, c, d, e, f}}; }
inline Mac ap(uint8_t last) { return mac(0x00, 0x11, 0x22, 0x33, 0x44, last); }
inline Mac sta(uint8_t last) { return mac(0x00, 0xaa, 0xbb, 0xcc, 0xdd, last); }
inline const Mac kBroadcast = mac(0xff, 0xff, 0xff, 0xff, 0xff, 0xff);

inline std::string macs(const uint8_t* m) {
  char s[18];
  return wlc::mac_to_string(m, s);
}

// ---- frame builders ------------------------------------------------------------------------

inline Bytes hdr(uint8_t type, uint8_t subtype, const Mac& a1, const Mac& a2, const Mac& a3,
                 uint8_t flags = 0) {
  Bytes b;
  put(b, {uint8_t((subtype << 4) | (type << 2)), flags, 0, 0});
  b.insert(b.end(), a1.b, a1.b + 6);
  b.insert(b.end(), a2.b, a2.b + 6);
  b.insert(b.end(), a3.b, a3.b + 6);
  put(b, {0x10, 0x00});  // sequence control
  return b;
}

inline Bytes ie(uint8_t id, const Bytes& body) {
  Bytes b = {id, uint8_t(body.size())};
  put(b, body);
  return b;
}
inline Bytes ie(uint8_t id, const std::string& body) { return ie(id, Bytes(body.begin(), body.end())); }
inline Bytes ext_ie(uint8_t ext, const Bytes& body) {
  Bytes x = {ext};
  put(x, body);
  return ie(255, x);
}

// Beacon (subtype 8) or probe response (5): header, 12 fixed bytes, then `ies`.
inline Bytes bss_frame(const Mac& bssid, const Bytes& ies, uint16_t capability = 0x0401,
                       uint8_t subtype = 8, uint16_t interval = 100) {
  Bytes b = hdr(0, subtype, subtype == 8 ? kBroadcast : sta(0xee), bssid, bssid);
  for (int i = 0; i < 8; ++i) b.push_back(uint8_t(i));
  put16(b, interval);
  put16(b, capability);
  put(b, ies);
  return b;
}

inline Bytes rsn(std::initializer_list<uint8_t> akms, uint16_t caps = 0, uint8_t pairwise = 4) {
  Bytes b;
  put16(b, 1);
  put(b, {0x00, 0x0f, 0xac, 4});
  put16(b, 1);
  put(b, {0x00, 0x0f, 0xac, pairwise});
  put16(b, uint16_t(akms.size()));
  for (uint8_t a : akms) put(b, {0x00, 0x0f, 0xac, a});
  put16(b, caps);
  return ie(48, b);
}

inline Bytes anqp_el(uint16_t id, const Bytes& body) {
  Bytes b;
  put16(b, id);
  put16(b, uint16_t(body.size()));
  put(b, body);
  return b;
}
inline Bytes hs20_el(uint8_t subtype, const Bytes& payload) {
  Bytes b = {0x50, 0x6f, 0x9a, 0x11, subtype, 0x00};
  put(b, payload);
  return anqp_el(0xdddd, b);
}

// GAS Initial Response (action 11). `a3` is address3, normally the BSSID.
inline Bytes gas_initial(const Mac& sa, const Mac& da, const Mac& a3, uint8_t token,
                         const Bytes& payload, uint16_t status = 0, uint16_t comeback_delay = 0,
                         uint8_t adv_proto = 0) {
  Bytes b = hdr(0, 13, da, sa, a3);
  put(b, {0x04, 0x0b, token});
  put16(b, status);
  put16(b, comeback_delay);
  put(b, {108, 2, 0x7f, adv_proto});
  put16(b, uint16_t(payload.size()));
  put(b, payload);
  return b;
}

inline Bytes gas_comeback(const Mac& sa, const Mac& da, const Mac& a3, uint8_t token, uint8_t frag,
                          bool more, const Bytes& payload, uint16_t status = 0, bool retry = false) {
  Bytes b = hdr(0, 13, da, sa, a3, retry ? 0x08 : 0x00);
  put(b, {0x04, 0x0d, token});
  put16(b, status);
  b.push_back(uint8_t(frag | (more ? 0x80 : 0)));
  put16(b, 0);
  put(b, {108, 2, 0x7f, 0x00});
  put16(b, uint16_t(payload.size()));
  put(b, payload);
  return b;
}

inline wlc::RxMeta meta(uint32_t now, int8_t rssi = -50, uint8_t ch = 6,
                        wlc::Band band = wlc::Band::B2G4) {
  return wlc::RxMeta{rssi, ch, band, now};
}

inline void feed(wlc::Survey& s, const Bytes& f, const wlc::RxMeta& m) { s.ingest(f.data(), f.size(), m); }

inline wlc::ApDetail detail(wlc::Survey& s, const Mac& bssid) {
  static wlc::ApDetail d;  // AnqpRecord is several KB; keep it off the stack
  std::memset(static_cast<void*>(&d), 0, sizeof(d));
  if (!s.ap_detail(bssid.b, &d)) std::memset(d.ap.bssid, 0, 6);
  return d;
}

inline std::string text(const char* s) { return std::string(s); }

}  // namespace t
