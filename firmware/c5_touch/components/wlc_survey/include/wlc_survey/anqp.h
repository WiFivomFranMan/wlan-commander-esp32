// SPDX-License-Identifier: MIT
//
// ANQP (802.11u / Hotspot 2.0) element records, the decoder that fills them, and the GAS
// Initial Request builder the touch firmware transmits.
//
// SAFETY: 259 (Emergency Call Number), 267 (AP Location Public Identifier URI), 269 (Emergency
// Alert URI) and 271 (Emergency NAI) are survey data and nothing else. They are decoded to inert
// text for display. Nothing in this library builds a dialable or openable form of them, and a UI
// must never render them as an actionable control — placing a test call to an emergency number is
// illegal and ties up a real call-taker.
#pragma once

#include <cstddef>
#include <cstdint>

#include "wlc_survey/types.h"

namespace wlc {

namespace anqp_id {
constexpr uint16_t QUERY_LIST = 256;
constexpr uint16_t CAPABILITY_LIST = 257;
constexpr uint16_t VENUE_NAME = 258;
constexpr uint16_t EMERGENCY_CALL_NUMBER = 259;
constexpr uint16_t NETWORK_AUTH_TYPE = 260;
constexpr uint16_t ROAMING_CONSORTIUM = 261;
constexpr uint16_t IP_ADDR_TYPE = 262;
constexpr uint16_t NAI_REALM = 263;
constexpr uint16_t CELLULAR_3GPP = 264;
constexpr uint16_t AP_GEOSPATIAL = 265;
constexpr uint16_t AP_CIVIC = 266;
constexpr uint16_t AP_LOCATION_URI = 267;
constexpr uint16_t DOMAIN_NAME = 268;
constexpr uint16_t EMERGENCY_ALERT_URI = 269;
constexpr uint16_t EMERGENCY_NAI = 271;
constexpr uint16_t VENUE_URL = 277;
constexpr uint16_t VENDOR_SPECIFIC = 56797;  // 0xDDDD
}  // namespace anqp_id

namespace hs20_subtype {
constexpr uint8_t QUERY_LIST = 1;
constexpr uint8_t CAPABILITY_LIST = 2;
constexpr uint8_t OPERATOR_FRIENDLY_NAME = 3;
constexpr uint8_t WAN_METRICS = 4;
constexpr uint8_t CONNECTION_CAPABILITY = 5;
constexpr uint8_t NAI_HOME_REALM = 6;
constexpr uint8_t OPERATING_CLASS = 7;
constexpr uint8_t OSU_PROVIDERS = 8;
}  // namespace hs20_subtype

// One bit per decoded element, in AnqpRecord::present.
enum AnqpField : uint32_t {
  AF_QUERY_LIST = 1u << 0,
  AF_CAPABILITY_LIST = 1u << 1,
  AF_VENUE_NAME = 1u << 2,
  AF_EMERGENCY_NUMBER = 1u << 3,
  AF_NETWORK_AUTH = 1u << 4,
  AF_ROAMING_CONSORTIUM = 1u << 5,
  AF_IP_ADDR_TYPE = 1u << 6,
  AF_NAI_REALM = 1u << 7,
  AF_3GPP = 1u << 8,
  AF_GEOSPATIAL = 1u << 9,
  AF_CIVIC = 1u << 10,
  AF_LOCATION_URI = 1u << 11,
  AF_DOMAIN_NAME = 1u << 12,
  AF_EMERGENCY_ALERT_URI = 1u << 13,
  AF_EMERGENCY_NAI = 1u << 14,
  AF_VENUE_URL = 1u << 15,
  AF_HS20_QUERY_LIST = 1u << 16,
  AF_HS20_CAPABILITY_LIST = 1u << 17,
  AF_OPERATOR_NAME = 1u << 18,
  AF_WAN_METRICS = 1u << 19,
  AF_CONNECTION_CAPABILITY = 1u << 20,
  AF_HOME_REALM = 1u << 21,
  AF_OPERATING_CLASS = 1u << 22,
  AF_OSU_PROVIDERS = 1u << 23,
};

struct LangName {
  char lang[4];  // ISO-639 code, NUL padded
  Text<64> name;
};

struct AuthTypeEntry {
  uint8_t type;  // 0 terms & conditions, 1 online enrollment, 2 http redirect, 3 dns redirect
  Text<64> url;  // display text only
};

// One EAP method of an NAI realm, with the auth parameters a UI summarises. 0 = not present.
struct NaiEap {
  uint8_t method;          // EAP type: 13 TLS, 18 SIM, 21 TTLS, 23 AKA, 25 PEAP, 50 AKA'
  uint8_t param_count;     // as declared
  uint8_t non_eap_inner;   // auth param 2: 1 PAP, 2 CHAP, 3 MSCHAP, 4 MSCHAPv2
  uint8_t inner_eap;       // auth param 3
  uint8_t credential;      // auth param 5: 1 SIM, 2 USIM, 3 NFC, 4 HW token, 5 softoken, 6 cert, 7 user/pass, 9 anon
  uint8_t tunneled_cred;   // auth param 6
};

struct NaiRealmEntry {
  Text<64> realm;  // may itself be a ';'-separated list, as transmitted
  uint8_t encoding;
  uint8_t eap_count;  // as declared
  uint8_t n_eap;      // stored
  NaiEap eap[4];
};

struct Plmn {
  char mcc[4];
  char mnc[4];
};

struct ConnCap {
  uint8_t proto;
  uint16_t port;
  uint8_t status;  // 0 closed, 1 open, 2 unknown
};

struct WanMetrics {
  uint8_t info;  // link status bits 0-1, symmetric bit 2, at capacity bit 3
  uint32_t dl_kbps;
  uint32_t ul_kbps;
  uint8_t dl_load;
  uint8_t ul_load;
  uint16_t lmd;
};

struct VenueUrlEntry {
  uint8_t venue_number;
  Text<96> url;  // display text only
};

struct UnknownElement {
  uint16_t id;
  uint16_t len;
  uint8_t subtype;  // HS2.0 subtype for 0xDDDD, else 0xFF
};

// A location element: kept raw (it is the authoritative copy) plus a printable summary.
template <size_t N>
struct RawBlob {
  uint8_t raw[N];
  uint16_t len;  // stored bytes
  uint16_t wire_len;
  bool truncated;
};

struct AnqpRecord {
  uint32_t present;  // AnqpField bits
  uint32_t first_ms;
  uint32_t updated_ms;
  uint16_t responses;
  bool truncated;  // some list or string hit its bound

  uint8_t n_query_ids;
  uint16_t query_ids[16];
  uint8_t n_capability_ids;
  uint16_t capability_ids[32];

  uint8_t venue_group, venue_type;
  uint8_t n_venue_names;
  LangName venue_names[4];

  uint8_t n_auth_types;
  AuthTypeEntry auth_types[4];

  uint8_t n_roaming_ois;
  OI roaming_ois[8];

  uint8_t ip_addr_type;  // raw octet: IPv6 bits 0-1, IPv4 bits 2-7

  uint16_t nai_realm_count;  // as declared
  uint8_t n_nai_realms;
  NaiRealmEntry nai_realms[8];

  uint8_t n_plmns;
  Plmn plmns[8];

  uint8_t n_domains;
  Text<64> domains[8];

  RawBlob<64> geo;
  Text<96> geo_summary;
  RawBlob<128> civic;
  Text<128> civic_summary;
  RawBlob<128> location_uri_raw;
  uint8_t n_location_uris;
  Text<96> location_uris[2];

  // Survey data only. Never actionable: see the note at the top of this file.
  uint8_t n_emergency_numbers;
  Text<32> emergency_numbers[4];
  uint8_t n_emergency_alert_uris;
  Text<96> emergency_alert_uris[2];
  Text<64> emergency_nai;

  uint8_t n_venue_urls;
  VenueUrlEntry venue_urls[2];

  uint8_t n_hs20_query;
  uint8_t hs20_query[8];
  uint8_t n_hs20_capability;
  uint8_t hs20_capability[16];
  uint8_t n_operator_names;
  LangName operator_names[4];
  WanMetrics wan;
  uint8_t n_conn_caps;
  ConnCap conn_caps[16];
  uint8_t n_home_realms;
  Text<64> home_realms[4];
  uint8_t n_op_classes;
  uint8_t op_classes[16];
  uint8_t osu_provider_count;  // as declared
  uint8_t n_osu_names;
  Text<64> osu_names[4];

  uint8_t n_unknown;
  UnknownElement unknown[8];

  void clear();
};

// Decode an ANQP element stream (Info ID, Length, body ...) into `out`, which is cleared first.
// Bounded and total: a truncated element stops the walk, never reads past `n`.
// Returns the number of elements walked.
size_t decode_anqp_elements(const uint8_t* p, size_t n, AnqpRecord* out);

// Overwrite each element group of `dst` that `src` carries. A response carrying an element is the
// AP's complete answer for that element, so the latest answer replaces rather than unions.
void merge_anqp(AnqpRecord* dst, const AnqpRecord& src);

// The two query sets the firmware alternates between, per AP.
enum class QuerySet : uint8_t { Passpoint = 0, Location = 1 };

// GAS Initial Request, action body only (category onwards) — byte-identical to the shipped
// firmware's build_gas(). esp_wifi_action_tx_req writes the 802.11 header, so the source address
// is the interface's own and the reply is directed and ACKed. Returns bytes written, or 0 if
// `cap` is too small.
size_t build_anqp_query(uint8_t* out, size_t cap, uint8_t dialog_token, QuerySet set);

// Same as build_anqp_query; the name the first firmware integration used.
size_t build_anqp_query_body(uint8_t* out, size_t cap, uint8_t dialog_token, QuerySet set);

}  // namespace wlc
