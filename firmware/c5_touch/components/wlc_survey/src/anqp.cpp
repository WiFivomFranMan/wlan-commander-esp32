// SPDX-License-Identifier: MIT
#include "wlc_survey/anqp.h"

#include <cstdio>
#include <cstring>

#include "internal.h"

namespace wlc {

using detail::le16;
using detail::le32;

void AnqpRecord::clear() { std::memset(this, 0, sizeof(*this)); }

namespace {

constexpr uint8_t kWfaOui[3] = {0x50, 0x6F, 0x9A};
constexpr uint8_t kHs20AnqpType = 0x11;

template <size_t N>
void put_text(Text<N>& t, const uint8_t* p, size_t n, AnqpRecord* r) {
  if (!t.set(p, n)) r->truncated = true;
}

// Bounded append: the count field doubles as "slots used", and an overflow marks the record.
template <typename T, size_t N>
T* push(T (&arr)[N], uint8_t* count, AnqpRecord* r) {
  if (*count >= N) {
    r->truncated = true;
    return nullptr;
  }
  T* slot = &arr[(*count)++];
  std::memset(static_cast<void*>(slot), 0, sizeof(T));
  return slot;
}

void put_lang(char lang[4], const uint8_t* p) {
  size_t i = 0;
  for (; i < 3 && p[i] >= 0x20 && p[i] < 0x7f; ++i) lang[i] = static_cast<char>(p[i]);
  for (; i < 4; ++i) lang[i] = '\0';
}

// [len][lang 3][text] duples, shared by Venue Name, Operator Friendly Name and OSU names.
void decode_duples(const uint8_t* b, size_t n, LangName* out, size_t cap, uint8_t* count,
                   AnqpRecord* r) {
  size_t pos = 0;
  while (pos < n) {
    const size_t len = b[pos++];
    if (len < 3 || pos + len > n) break;
    if (*count < cap) {
      LangName& e = out[(*count)++];
      put_lang(e.lang, b + pos);
      put_text(e.name, b + pos + 3, len - 3, r);
    } else {
      r->truncated = true;
    }
    pos += len;
  }
}

void decode_id_list(const uint8_t* b, size_t n, uint16_t* out, size_t cap, uint8_t* count,
                    AnqpRecord* r) {
  size_t pos = 0;
  while (pos + 2 <= n) {
    const uint16_t id = le16(b + pos);
    pos += 2;
    if (*count < cap) {
      out[(*count)++] = id;
    } else {
      r->truncated = true;
    }
    // A vendor-specific entry carries its own length and payload inside the list.
    if (id == anqp_id::VENDOR_SPECIFIC) {
      if (pos + 2 > n) break;
      pos += 2u + le16(b + pos);
    }
  }
}

// Printable-only text, trimmed. Returns false for empty or control-bearing input — that is the
// signal the length-prefixed probe below relies on to reject a wrong shape.
bool printable(const uint8_t* p, size_t n, size_t* start, size_t* len) {
  if (n == 0) return false;
  for (size_t i = 0; i < n; ++i) {
    if (p[i] < 0x20 || p[i] == 0x7f) return false;
  }
  size_t s = 0, e = n;
  while (s < e && p[s] == ' ') ++s;
  while (e > s && p[e - 1] == ' ') --e;
  if (s == e) return false;
  *start = s;
  *len = e - s;
  return true;
}

bool is_length_prefixed_text(const uint8_t* b, size_t n) {
  if (n == 0) return false;
  size_t pos = 0;
  while (pos < n) {
    const size_t len = b[pos++];
    size_t s = 0, l = 0;
    if (len == 0 || pos + len > n || !printable(b + pos, len, &s, &l)) return false;
    pos += len;
  }
  return true;
}

// Same probe order as core's decode_text_element: [len][text]..., then that after one leading
// descriptor octet, then the whole body as one string. Output is inert text in every case.
template <size_t N, size_t M>
void decode_text_element(const uint8_t* b, size_t n, Text<N> (&out)[M], uint8_t* count,
                         AnqpRecord* r) {
  *count = 0;
  const uint8_t* base = nullptr;
  size_t blen = 0;
  if (is_length_prefixed_text(b, n)) {
    base = b;
    blen = n;
  } else if (n > 1 && is_length_prefixed_text(b + 1, n - 1)) {
    base = b + 1;
    blen = n - 1;
  }
  if (base) {
    size_t pos = 0;
    while (pos < blen) {
      const size_t len = base[pos++];
      size_t s = 0, l = 0;
      printable(base + pos, len, &s, &l);
      if (Text<N>* t = push(out, count, r)) put_text(*t, base + pos + s, l, r);
      pos += len;
    }
    return;
  }
  size_t s = 0, l = 0;
  if (printable(b, n, &s, &l)) {
    if (Text<N>* t = push(out, count, r)) put_text(*t, b + s, l, r);
  }
}

void decode_nai_realms(const uint8_t* b, size_t n, AnqpRecord* r) {
  r->n_nai_realms = 0;
  if (n < 2) return;
  r->nai_realm_count = le16(b);
  size_t pos = 2;
  for (unsigned i = 0; i < r->nai_realm_count; ++i) {
    if (pos + 2 > n) break;
    const size_t field_len = le16(b + pos);
    pos += 2;
    const size_t end = pos + field_len;
    if (end > n || field_len < 2) break;
    NaiRealmEntry* e = push(r->nai_realms, &r->n_nai_realms, r);
    if (e) {
      e->encoding = b[pos];
      const size_t rl = b[pos + 1];
      size_t p = pos + 2;
      if (p + rl <= end) {
        put_text(e->realm, b + p, rl, r);
        p += rl;
        if (p < end) {
          e->eap_count = b[p++];
          for (unsigned m = 0; m < e->eap_count && p < end; ++m) {
            const size_t eap_len = b[p++];
            const size_t eap_end = p + eap_len;
            if (eap_end > end || eap_len < 2) break;
            NaiEap* eap = push(e->eap, &e->n_eap, r);
            if (eap) {
              eap->method = b[p];
              eap->param_count = b[p + 1];
              size_t q = p + 2;
              for (unsigned k = 0; k < eap->param_count && q + 2 <= eap_end; ++k) {
                const uint8_t id = b[q];
                const size_t plen = b[q + 1];
                q += 2;
                if (q + plen > eap_end) break;
                const uint8_t v = plen ? b[q] : 0;
                if (id == 2) eap->non_eap_inner = v;
                if (id == 3) eap->inner_eap = v;
                if (id == 5) eap->credential = v;
                if (id == 6) eap->tunneled_cred = v;
                q += plen;
              }
            }
            p = eap_end;
          }
        }
      }
    }
    pos = end;
  }
}

void bcd_digit(char* out, size_t* k, uint8_t nibble) {
  out[(*k)++] = static_cast<char>(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
}

// Same layout and bounds as core's parse_3gpp_plmns: GUD, UDHL, IEI, PLMN-list length, count.
void decode_3gpp(const uint8_t* b, size_t n, AnqpRecord* r) {
  r->n_plmns = 0;
  if (n < 5) return;
  const size_t end = (4u + b[3]) < n ? (4u + b[3]) : n;
  const size_t count = b[4];
  for (size_t pos = 5; pos + 3 <= end && r->n_plmns < count; pos += 3) {
    Plmn* p = push(r->plmns, &r->n_plmns, r);
    if (!p) break;
    const uint8_t b0 = b[pos], b1 = b[pos + 1], b2 = b[pos + 2];
    size_t k = 0;
    bcd_digit(p->mcc, &k, b0 & 0x0f);
    bcd_digit(p->mcc, &k, b0 >> 4);
    bcd_digit(p->mcc, &k, b1 & 0x0f);
    p->mcc[k] = '\0';
    k = 0;
    bcd_digit(p->mnc, &k, b2 & 0x0f);
    bcd_digit(p->mnc, &k, b2 >> 4);
    if ((b1 >> 4) != 0x0f) bcd_digit(p->mnc, &k, b1 >> 4);
    p->mnc[k] = '\0';
  }
}

void decode_auth_types(const uint8_t* b, size_t n, AnqpRecord* r) {
  r->n_auth_types = 0;
  size_t pos = 0;
  while (pos + 3 <= n) {
    AuthTypeEntry* e = push(r->auth_types, &r->n_auth_types, r);
    if (!e) break;
    e->type = b[pos];
    const size_t url_len = le16(b + pos + 1);
    pos += 3;
    if (pos + url_len > n) break;  // the rest cannot be trusted
    put_text(e->url, b + pos, url_len, r);
    pos += url_len;
  }
  // A lone indicator octet is still a legible answer, and some APs send exactly that.
  if (r->n_auth_types == 0 && n >= 1) {
    r->n_auth_types = 1;
    std::memset(static_cast<void*>(&r->auth_types[0]), 0, sizeof(AuthTypeEntry));
    r->auth_types[0].type = b[0];
  }
}

// Reads `nbits` starting `start` bits from the MSB of a big-endian bit string, as core does.
uint64_t bits_be(const uint8_t* b, unsigned start, unsigned nbits) {
  uint64_t v = 0;
  for (unsigned i = 0; i < nbits; ++i) {
    const unsigned bit = start + i;
    v = (v << 1) | ((b[bit / 8] >> (7 - bit % 8)) & 1u);
  }
  return v;
}

int64_t sign_extend(uint64_t raw, unsigned bits) {
  const uint64_t sign = 1ull << (bits - 1);
  return static_cast<int64_t>((raw ^ sign) - sign);
}

// Fixed-point decimal without float printf, which newlib-nano may not carry.
size_t fmt_fixed(char* out, size_t cap, int64_t value, int64_t denom, unsigned decimals) {
  const bool neg = value < 0;
  uint64_t mag = static_cast<uint64_t>(neg ? -value : value);
  uint64_t scale = 1;
  for (unsigned i = 0; i < decimals; ++i) scale *= 10;
  const uint64_t scaled = (mag * scale + static_cast<uint64_t>(denom) / 2) / static_cast<uint64_t>(denom);
  const int n = std::snprintf(out, cap, "%s%llu.%0*llu", neg ? "-" : "",
                              static_cast<unsigned long long>(scaled / scale),
                              static_cast<int>(decimals),
                              static_cast<unsigned long long>(scaled % scale));
  return n < 0 ? 0 : (static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1);
}

// RFC 6225 LCI, same field split and range check as core's decode_lci_field. The range check is
// what makes probing the two wrapper shapes safe: a misaligned read lands outside the globe.
bool lci_summary(const uint8_t* lci, char* out, size_t cap) {
  const int64_t lat = sign_extend(bits_be(lci, 6, 34), 34);
  const int64_t lon = sign_extend(bits_be(lci, 46, 34), 34);
  const int64_t scale = 1ll << 25;
  if (lat < -90 * scale || lat > 90 * scale || lon < -180 * scale || lon > 180 * scale) return false;
  size_t k = fmt_fixed(out, cap, lat, scale, 6);
  if (k + 1 < cap) out[k++] = ',';
  k += fmt_fixed(out + k, cap - k, lon, scale, 6);
  const unsigned alt_type = static_cast<unsigned>(bits_be(lci, 80, 4));
  const int64_t alt = sign_extend(bits_be(lci, 90, 30), 30);
  if (alt_type == 1 || alt_type == 2) {
    k += static_cast<size_t>(std::snprintf(out + k, cap - k, alt_type == 1 ? " alt " : " floor "));
    if (k < cap) k += fmt_fixed(out + k, cap - k, alt, 256, 1);
    if (alt_type == 1 && k + 2 < cap) {
      out[k++] = ' ';
      out[k++] = 'm';
      out[k] = '\0';
    }
  }
  return true;
}

void decode_geo(const uint8_t* b, size_t n, AnqpRecord* r) {
  RawBlob<64>& g = r->geo;
  g.wire_len = static_cast<uint16_t>(n);
  g.len = static_cast<uint16_t>(n < sizeof(g.raw) ? n : sizeof(g.raw));
  g.truncated = n > sizeof(g.raw);
  if (g.truncated) r->truncated = true;
  std::memcpy(g.raw, b, g.len);
  char s[96];
  bool ok = false;
  if (n >= 18 && b[0] == 0 && b[1] == 16) ok = lci_summary(b + 2, s, sizeof(s));  // subelement form
  if (!ok && n >= 16) ok = lci_summary(b, s, sizeof(s));                          // bare LCI
  if (ok) {
    put_text(r->geo_summary, reinterpret_cast<const uint8_t*>(s), std::strlen(s), r);
  } else {
    r->geo_summary.clear();
  }
}

const char* civic_type_name(uint8_t t) {
  switch (t) {
    case 0: return "language";
    case 1: return "A1";
    case 2: return "A2";
    case 3: return "A3";
    case 4: return "A4";
    case 5: return "A5";
    case 6: return "A6";
    case 16: return "PRD";
    case 17: return "POD";
    case 18: return "STS";
    case 19: return "HNO";
    case 20: return "HNS";
    case 21: return "LMK";
    case 22: return "LOC";
    case 23: return "NAM";
    case 24: return "PC";
    case 25: return "BLD";
    case 26: return "UNIT";
    case 27: return "FLR";
    case 28: return "ROOM";
    case 29: return "PLC";
    case 30: return "PCN";
    case 31: return "POBOX";
    case 32: return "ADDCODE";
    case 33: return "SEAT";
    case 34: return "RD";
    case 35: return "RDSEC";
    case 36: return "RDBR";
    case 37: return "RDSUBBR";
    case 38: return "PRM";
    case 39: return "POM";
    default: return "CA";
  }
}

// Validates an RFC 4776 body (country, then CAtype/CAlength/CAvalue consuming the slice
// exactly, all text) and returns its element count, or -1.
int civic_validate(const uint8_t* b, size_t n) {
  if (n < 2 || b[0] < 'A' || b[0] > 'Z' || b[1] < 'A' || b[1] > 'Z') return -1;
  int count = 0;
  size_t pos = 2;
  while (pos < n) {
    if (pos + 2 > n) return -1;
    const size_t len = b[pos + 1];
    pos += 2;
    if (pos + len > n) return -1;
    for (size_t i = 0; i < len; ++i) {
      if (b[pos + i] < 0x20 && b[pos + i] != '\t') return -1;
    }
    pos += len;
    ++count;
  }
  return count > 0 ? count : -1;
}

void decode_civic(const uint8_t* b, size_t n, AnqpRecord* r) {
  RawBlob<128>& c = r->civic;
  c.wire_len = static_cast<uint16_t>(n);
  c.len = static_cast<uint16_t>(n < sizeof(c.raw) ? n : sizeof(c.raw));
  c.truncated = n > sizeof(c.raw);
  if (c.truncated) r->truncated = true;
  std::memcpy(c.raw, b, c.len);
  r->civic_summary.clear();

  // The 802.11 wrapper in front of the RFC 4776 body varies between vendors; probe offsets 0..4
  // the way core does and keep the shape that yields the most elements.
  int best = -1;
  size_t best_off = 0;
  for (size_t off = 0; off <= 4 && off < n; ++off) {
    const int count = civic_validate(b + off, n - off);
    if (count > best) {
      best = count;
      best_off = off;
    }
  }
  if (best < 0) return;
  const uint8_t* p = b + best_off;
  const size_t m = n - best_off;
  char s[256];
  size_t k = static_cast<size_t>(std::snprintf(s, sizeof(s), "%c%c:", p[0], p[1]));
  for (size_t pos = 2; pos + 2 <= m && k < sizeof(s) - 1;) {
    const uint8_t type = p[pos];
    const size_t len = p[pos + 1];
    pos += 2;
    int w = std::snprintf(s + k, sizeof(s) - k, "%s %s=", k > 3 ? ";" : "", civic_type_name(type));
    if (w < 0) break;
    k += static_cast<size_t>(w);
    for (size_t i = 0; i < len && k < sizeof(s) - 1; ++i) s[k++] = static_cast<char>(p[pos + i]);
    pos += len;
  }
  if (k > sizeof(s) - 1) k = sizeof(s) - 1;
  put_text(r->civic_summary, reinterpret_cast<const uint8_t*>(s), k, r);
}

void decode_venue_urls(const uint8_t* b, size_t n, AnqpRecord* r) {
  r->n_venue_urls = 0;
  size_t pos = 0;
  while (pos < n) {
    const size_t len = b[pos++];
    if (len < 1 || pos + len > n) break;
    if (VenueUrlEntry* e = push(r->venue_urls, &r->n_venue_urls, r)) {
      e->venue_number = b[pos];
      put_text(e->url, b + pos + 1, len - 1, r);
    }
    pos += len;
  }
}

void decode_osu(const uint8_t* b, size_t n, AnqpRecord* r) {
  r->n_osu_names = 0;
  r->osu_provider_count = 0;
  if (n < 1) return;
  size_t pos = 1u + b[0];  // OSU SSID
  if (pos >= n) return;
  r->osu_provider_count = b[pos++];
  for (unsigned i = 0; i < r->osu_provider_count && pos + 2 <= n; ++i) {
    const size_t plen = le16(b + pos);
    pos += 2;
    const size_t end = pos + plen;
    if (end > n) break;
    if (plen >= 2) {
      const size_t fn_len = le16(b + pos);
      const size_t fn = pos + 2;
      // First friendly-name duple is the provider's display name.
      if (fn + fn_len <= end && fn_len >= 4) {
        const size_t dlen = b[fn];
        if (dlen >= 3 && fn + 1 + dlen <= fn + fn_len) {
          if (Text<64>* t = push(r->osu_names, &r->n_osu_names, r)) put_text(*t, b + fn + 4, dlen - 3, r);
        }
      }
    }
    pos = end;
  }
}

void decode_home_realms(const uint8_t* b, size_t n, AnqpRecord* r) {
  r->n_home_realms = 0;
  if (n < 1) return;
  const unsigned count = b[0];
  size_t pos = 1;
  for (unsigned i = 0; i < count && pos + 2 <= n; ++i) {
    const size_t len = b[pos + 1];
    pos += 2;
    if (pos + len > n) break;
    if (Text<64>* t = push(r->home_realms, &r->n_home_realms, r)) put_text(*t, b + pos, len, r);
    pos += len;
  }
}

void add_unknown(AnqpRecord* r, uint16_t id, size_t len, uint8_t subtype) {
  if (UnknownElement* u = push(r->unknown, &r->n_unknown, r)) {
    u->id = id;
    u->len = static_cast<uint16_t>(len);
    u->subtype = subtype;
  }
}

void decode_vendor(const uint8_t* b, size_t n, AnqpRecord* r) {
  if (n < 6 || std::memcmp(b, kWfaOui, 3) != 0 || b[3] != kHs20AnqpType) {
    add_unknown(r, anqp_id::VENDOR_SPECIFIC, n, n >= 5 ? b[4] : 0xFF);
    return;
  }
  const uint8_t subtype = b[4];
  const uint8_t* p = b + 6;  // past OUI, type, subtype, reserved
  const size_t m = n - 6;
  switch (subtype) {
    case hs20_subtype::QUERY_LIST:
      r->present |= AF_HS20_QUERY_LIST;
      r->n_hs20_query = 0;
      for (size_t i = 0; i < m; ++i) {
        if (uint8_t* s = push(r->hs20_query, &r->n_hs20_query, r)) *s = p[i];
      }
      break;
    case hs20_subtype::CAPABILITY_LIST:
      r->present |= AF_HS20_CAPABILITY_LIST;
      r->n_hs20_capability = 0;
      for (size_t i = 0; i < m; ++i) {
        if (uint8_t* s = push(r->hs20_capability, &r->n_hs20_capability, r)) *s = p[i];
      }
      break;
    case hs20_subtype::OPERATOR_FRIENDLY_NAME:
      r->present |= AF_OPERATOR_NAME;
      r->n_operator_names = 0;
      decode_duples(p, m, r->operator_names, 4, &r->n_operator_names, r);
      break;
    case hs20_subtype::WAN_METRICS:
      if (m >= 13) {
        r->present |= AF_WAN_METRICS;
        r->wan.info = p[0];
        r->wan.dl_kbps = le32(p + 1);
        r->wan.ul_kbps = le32(p + 5);
        r->wan.dl_load = p[9];
        r->wan.ul_load = p[10];
        r->wan.lmd = le16(p + 11);
      }
      break;
    case hs20_subtype::CONNECTION_CAPABILITY:
      r->present |= AF_CONNECTION_CAPABILITY;
      r->n_conn_caps = 0;
      for (size_t i = 0; i + 4 <= m; i += 4) {
        ConnCap* c = push(r->conn_caps, &r->n_conn_caps, r);
        if (!c) break;
        c->proto = p[i];
        c->port = le16(p + i + 1);
        c->status = p[i + 3];
      }
      break;
    case hs20_subtype::NAI_HOME_REALM:
      r->present |= AF_HOME_REALM;
      decode_home_realms(p, m, r);
      break;
    case hs20_subtype::OPERATING_CLASS:
      r->present |= AF_OPERATING_CLASS;
      r->n_op_classes = 0;
      for (size_t i = 0; i < m; ++i) {
        if (uint8_t* s = push(r->op_classes, &r->n_op_classes, r)) *s = p[i];
      }
      break;
    case hs20_subtype::OSU_PROVIDERS:
      r->present |= AF_OSU_PROVIDERS;
      decode_osu(p, m, r);
      break;
    default:
      add_unknown(r, anqp_id::VENDOR_SPECIFIC, n, subtype);
      break;
  }
}

}  // namespace

size_t decode_anqp_elements(const uint8_t* p, size_t n, AnqpRecord* r) {
  r->clear();
  size_t walked = 0;
  size_t pos = 0;
  while (pos + 4 <= n) {
    const uint16_t id = le16(p + pos);
    const size_t len = le16(p + pos + 2);
    if (pos + 4 + len > n) break;
    const uint8_t* b = p + pos + 4;
    pos += 4 + len;
    ++walked;
    switch (id) {
      case anqp_id::QUERY_LIST:
        r->present |= AF_QUERY_LIST;
        r->n_query_ids = 0;
        decode_id_list(b, len, r->query_ids, 16, &r->n_query_ids, r);
        break;
      case anqp_id::CAPABILITY_LIST:
        r->present |= AF_CAPABILITY_LIST;
        r->n_capability_ids = 0;
        decode_id_list(b, len, r->capability_ids, 32, &r->n_capability_ids, r);
        break;
      case anqp_id::VENUE_NAME:
        if (len >= 2) {
          r->present |= AF_VENUE_NAME;
          r->venue_group = b[0];
          r->venue_type = b[1];
          r->n_venue_names = 0;
          decode_duples(b + 2, len - 2, r->venue_names, 4, &r->n_venue_names, r);
        }
        break;
      case anqp_id::NETWORK_AUTH_TYPE:
        r->present |= AF_NETWORK_AUTH;
        decode_auth_types(b, len, r);
        break;
      case anqp_id::ROAMING_CONSORTIUM: {
        r->present |= AF_ROAMING_CONSORTIUM;
        r->n_roaming_ois = 0;
        size_t q = 0;
        while (q < len) {
          const size_t l = b[q++];
          if (l == 0 || q + l > len) break;
          if (OI* oi = push(r->roaming_ois, &r->n_roaming_ois, r)) {
            oi->len = static_cast<uint8_t>(l < sizeof(oi->b) ? l : sizeof(oi->b));
            if (l > sizeof(oi->b)) r->truncated = true;
            std::memcpy(oi->b, b + q, oi->len);
          }
          q += l;
        }
        break;
      }
      case anqp_id::IP_ADDR_TYPE:
        if (len >= 1) {
          r->present |= AF_IP_ADDR_TYPE;
          r->ip_addr_type = b[0];
        }
        break;
      case anqp_id::NAI_REALM:
        r->present |= AF_NAI_REALM;
        decode_nai_realms(b, len, r);
        break;
      case anqp_id::CELLULAR_3GPP:
        r->present |= AF_3GPP;
        decode_3gpp(b, len, r);
        break;
      case anqp_id::DOMAIN_NAME: {
        r->present |= AF_DOMAIN_NAME;
        r->n_domains = 0;
        size_t q = 0;
        while (q < len) {
          const size_t l = b[q++];
          if (l == 0 || q + l > len) break;
          if (Text<64>* t = push(r->domains, &r->n_domains, r)) put_text(*t, b + q, l, r);
          q += l;
        }
        break;
      }
      case anqp_id::AP_GEOSPATIAL:
        r->present |= AF_GEOSPATIAL;
        decode_geo(b, len, r);
        break;
      case anqp_id::AP_CIVIC:
        r->present |= AF_CIVIC;
        decode_civic(b, len, r);
        break;
      case anqp_id::AP_LOCATION_URI: {
        r->present |= AF_LOCATION_URI;
        RawBlob<128>& u = r->location_uri_raw;
        u.wire_len = static_cast<uint16_t>(len);
        u.len = static_cast<uint16_t>(len < sizeof(u.raw) ? len : sizeof(u.raw));
        u.truncated = len > sizeof(u.raw);
        std::memcpy(u.raw, b, u.len);
        decode_text_element(b, len, r->location_uris, &r->n_location_uris, r);
        break;
      }
      // Emergency elements: decoded as plain text for display. Never made actionable.
      case anqp_id::EMERGENCY_CALL_NUMBER:
        r->present |= AF_EMERGENCY_NUMBER;
        decode_text_element(b, len, r->emergency_numbers, &r->n_emergency_numbers, r);
        break;
      case anqp_id::EMERGENCY_ALERT_URI:
        r->present |= AF_EMERGENCY_ALERT_URI;
        decode_text_element(b, len, r->emergency_alert_uris, &r->n_emergency_alert_uris, r);
        break;
      case anqp_id::EMERGENCY_NAI: {
        r->present |= AF_EMERGENCY_NAI;
        Text<64> one[1];
        uint8_t c = 0;
        decode_text_element(b, len, one, &c, r);
        if (c) {
          r->emergency_nai = one[0];
        } else {
          r->emergency_nai.clear();
        }
        break;
      }
      case anqp_id::VENUE_URL:
        r->present |= AF_VENUE_URL;
        decode_venue_urls(b, len, r);
        break;
      case anqp_id::VENDOR_SPECIFIC:
        decode_vendor(b, len, r);
        break;
      default:
        add_unknown(r, id, len, 0xFF);
        break;
    }
  }
  return walked;
}

void merge_anqp(AnqpRecord* d, const AnqpRecord& s) {
  const uint32_t p = s.present;
  if (p & AF_QUERY_LIST) {
    d->n_query_ids = s.n_query_ids;
    std::memcpy(d->query_ids, s.query_ids, sizeof(s.query_ids));
  }
  if (p & AF_CAPABILITY_LIST) {
    d->n_capability_ids = s.n_capability_ids;
    std::memcpy(d->capability_ids, s.capability_ids, sizeof(s.capability_ids));
  }
  if (p & AF_VENUE_NAME) {
    d->venue_group = s.venue_group;
    d->venue_type = s.venue_type;
    d->n_venue_names = s.n_venue_names;
    std::memcpy(d->venue_names, s.venue_names, sizeof(s.venue_names));
  }
  if (p & AF_EMERGENCY_NUMBER) {
    d->n_emergency_numbers = s.n_emergency_numbers;
    std::memcpy(d->emergency_numbers, s.emergency_numbers, sizeof(s.emergency_numbers));
  }
  if (p & AF_NETWORK_AUTH) {
    d->n_auth_types = s.n_auth_types;
    std::memcpy(d->auth_types, s.auth_types, sizeof(s.auth_types));
  }
  if (p & AF_ROAMING_CONSORTIUM) {
    d->n_roaming_ois = s.n_roaming_ois;
    std::memcpy(d->roaming_ois, s.roaming_ois, sizeof(s.roaming_ois));
  }
  if (p & AF_IP_ADDR_TYPE) d->ip_addr_type = s.ip_addr_type;
  if (p & AF_NAI_REALM) {
    d->nai_realm_count = s.nai_realm_count;
    d->n_nai_realms = s.n_nai_realms;
    std::memcpy(d->nai_realms, s.nai_realms, sizeof(s.nai_realms));
  }
  if (p & AF_3GPP) {
    d->n_plmns = s.n_plmns;
    std::memcpy(d->plmns, s.plmns, sizeof(s.plmns));
  }
  if (p & AF_GEOSPATIAL) {
    d->geo = s.geo;
    d->geo_summary = s.geo_summary;
  }
  if (p & AF_CIVIC) {
    d->civic = s.civic;
    d->civic_summary = s.civic_summary;
  }
  if (p & AF_LOCATION_URI) {
    d->location_uri_raw = s.location_uri_raw;
    d->n_location_uris = s.n_location_uris;
    std::memcpy(d->location_uris, s.location_uris, sizeof(s.location_uris));
  }
  if (p & AF_DOMAIN_NAME) {
    d->n_domains = s.n_domains;
    std::memcpy(d->domains, s.domains, sizeof(s.domains));
  }
  if (p & AF_EMERGENCY_ALERT_URI) {
    d->n_emergency_alert_uris = s.n_emergency_alert_uris;
    std::memcpy(d->emergency_alert_uris, s.emergency_alert_uris, sizeof(s.emergency_alert_uris));
  }
  if (p & AF_EMERGENCY_NAI) d->emergency_nai = s.emergency_nai;
  if (p & AF_VENUE_URL) {
    d->n_venue_urls = s.n_venue_urls;
    std::memcpy(d->venue_urls, s.venue_urls, sizeof(s.venue_urls));
  }
  if (p & AF_HS20_QUERY_LIST) {
    d->n_hs20_query = s.n_hs20_query;
    std::memcpy(d->hs20_query, s.hs20_query, sizeof(s.hs20_query));
  }
  if (p & AF_HS20_CAPABILITY_LIST) {
    d->n_hs20_capability = s.n_hs20_capability;
    std::memcpy(d->hs20_capability, s.hs20_capability, sizeof(s.hs20_capability));
  }
  if (p & AF_OPERATOR_NAME) {
    d->n_operator_names = s.n_operator_names;
    std::memcpy(d->operator_names, s.operator_names, sizeof(s.operator_names));
  }
  if (p & AF_WAN_METRICS) d->wan = s.wan;
  if (p & AF_CONNECTION_CAPABILITY) {
    d->n_conn_caps = s.n_conn_caps;
    std::memcpy(d->conn_caps, s.conn_caps, sizeof(s.conn_caps));
  }
  if (p & AF_HOME_REALM) {
    d->n_home_realms = s.n_home_realms;
    std::memcpy(d->home_realms, s.home_realms, sizeof(s.home_realms));
  }
  if (p & AF_OPERATING_CLASS) {
    d->n_op_classes = s.n_op_classes;
    std::memcpy(d->op_classes, s.op_classes, sizeof(s.op_classes));
  }
  if (p & AF_OSU_PROVIDERS) {
    d->osu_provider_count = s.osu_provider_count;
    d->n_osu_names = s.n_osu_names;
    std::memcpy(d->osu_names, s.osu_names, sizeof(s.osu_names));
  }
  if (s.n_unknown) {
    d->n_unknown = s.n_unknown;
    std::memcpy(d->unknown, s.unknown, sizeof(s.unknown));
  }
  d->present |= p;
  d->truncated = d->truncated || s.truncated;
}

// ---------------------------------------------------------------------------------------------
// GAS Initial Request. Mirrors firmware/wlc_sniffer/wlc_sniffer.ino build_gas() byte for byte:
// the Passpoint set carries the HS2.0 query list, the location set does not, and the Query
// Response Length Limit is 0 because that is the requester's value.

namespace {
constexpr uint16_t kQidsPasspoint[] = {257, 258, 260, 261, 262, 263, 264, 268, 277};
constexpr uint16_t kQidsLocation[] = {259, 265, 266, 267, 269, 271};
constexpr uint8_t kHs20Subtypes[] = {3, 4, 5};
}  // namespace

size_t build_anqp_query_body(uint8_t* b, size_t cap, uint8_t tok, QuerySet set) {
  const bool location = set == QuerySet::Location;
  const uint16_t* ids = location ? kQidsLocation : kQidsPasspoint;
  const size_t k = location ? sizeof(kQidsLocation) / 2 : sizeof(kQidsPasspoint) / 2;
  const size_t need = 9 + 4 + 2 * k + (location ? 0 : 4 + 6 + sizeof(kHs20Subtypes));
  if (b == nullptr || cap < need) return 0;

  size_t n = 0;
  b[n++] = 0x04;  // Public Action
  b[n++] = 0x0a;  // GAS Initial Request
  b[n++] = tok;
  b[n++] = 108;   // Advertisement Protocol element
  b[n++] = 2;
  b[n++] = 0x00;  // Query Response Length Limit 0, PAME-BI 0
  b[n++] = 0x00;  // ANQP
  const size_t qlen_at = n;
  n += 2;
  const size_t q0 = n;
  b[n++] = 0x00;  // ANQP Query List, Info ID 256
  b[n++] = 0x01;
  b[n++] = static_cast<uint8_t>((k * 2) & 0xff);
  b[n++] = static_cast<uint8_t>((k * 2) >> 8);
  for (size_t i = 0; i < k; ++i) {
    b[n++] = static_cast<uint8_t>(ids[i] & 0xff);
    b[n++] = static_cast<uint8_t>(ids[i] >> 8);
  }
  if (!location) {
    b[n++] = 0xdd;  // Vendor Specific 56797
    b[n++] = 0xdd;
    const size_t vlen_at = n;
    n += 2;
    const size_t v0 = n;
    b[n++] = kWfaOui[0];
    b[n++] = kWfaOui[1];
    b[n++] = kWfaOui[2];
    b[n++] = kHs20AnqpType;
    b[n++] = hs20_subtype::QUERY_LIST;
    b[n++] = 0x00;  // reserved
    for (uint8_t st : kHs20Subtypes) b[n++] = st;
    b[vlen_at] = static_cast<uint8_t>((n - v0) & 0xff);
    b[vlen_at + 1] = static_cast<uint8_t>((n - v0) >> 8);
  }
  b[qlen_at] = static_cast<uint8_t>((n - q0) & 0xff);
  b[qlen_at + 1] = static_cast<uint8_t>((n - q0) >> 8);
  return n;
}

size_t build_anqp_query(uint8_t* out, size_t cap, uint8_t tok, QuerySet set) {
  return build_anqp_query_body(out, cap, tok, set);
}

}  // namespace wlc
