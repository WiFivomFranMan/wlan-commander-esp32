// SPDX-License-Identifier: MIT
//
// On-device 802.11 survey: AP table, client table and ANQP results, fed one MPDU at a time.
//
// Threading: ingest() runs on one task; the snapshot functions may run on another. Every table
// access takes the Lock supplied in Config (default: a no-op). Snapshots copy into caller
// buffers, so the UI never holds a pointer into the tables.
//
// Memory: init() makes every allocation, once, through Config::alloc (PSRAM on the device).
// Nothing on the per-frame path allocates.
#pragma once

#include <cstddef>
#include <cstdint>

#include "wlc_survey/anqp.h"
#include "wlc_survey/types.h"

namespace wlc {

// Implement with a FreeRTOS mutex or critical section on the device. Hold times are short:
// one frame's table update, or one sort of at most max_aps indices.
class Lock {
 public:
  virtual void lock() = 0;
  virtual void unlock() = 0;

 protected:
  ~Lock() = default;
};

class NullLock final : public Lock {
 public:
  void lock() override {}
  void unlock() override {}
};

enum class Width : uint8_t { W20 = 0, W40, W80, W160, W320 };

enum class Security : uint8_t {
  Open = 0,
  OWE,
  WEP,
  WPA,           // WPA1 vendor IE only
  WPA2Personal,  // PSK
  WPA3SAE,
  WPA2WPA3,      // PSK + SAE transition
  Enterprise,    // any 802.1X AKM, including Suite-B; see ApInfo::suite_b
  Unknown,       // RSN present with no AKM we recognise
};

enum PhyFlags : uint8_t { PHY_HT = 1u << 0, PHY_VHT = 1u << 1, PHY_HE = 1u << 2, PHY_EHT = 1u << 3 };

// Cipher suite bits (00-0F-AC suite types): 1 << type.
enum CipherBits : uint16_t {
  CIPHER_WEP40 = 1u << 1,
  CIPHER_TKIP = 1u << 2,
  CIPHER_CCMP = 1u << 4,
  CIPHER_WEP104 = 1u << 5,
  CIPHER_GCMP = 1u << 8,
  CIPHER_GCMP256 = 1u << 9,
  CIPHER_CCMP256 = 1u << 10,
};

struct ApInfo {
  uint8_t bssid[6];
  uint8_t ssid[32];  // raw octets as advertised
  uint8_t ssid_len;
  bool hidden;  // beacons carry a zero-length or all-NUL SSID

  Band band;
  uint8_t channel;     // AP's primary: DS Parameter Set / HT Operation / HE 6 GHz Operation
  uint8_t rx_channel;  // where the radio heard it
  Width width;

  int8_t rssi_last, rssi_avg, rssi_peak;
  uint32_t beacons;
  uint32_t probe_resps;
  uint32_t first_seen_ms, last_seen_ms;
  uint16_t beacon_interval;  // TU
  uint16_t capability;       // fixed-field capability information

  Security security;
  bool suite_b;
  bool pmf_capable, pmf_required;
  bool wpa_ie;               // WPA1 vendor IE present
  uint32_t akm_mask;         // RSN AKM suite types (00-0F-AC), 1 << type for type < 32
  uint8_t wpa_akm_mask;      // WPA1 AKM suite types (00-50-F2), 1 << type
  uint16_t pairwise_ciphers; // CipherBits
  uint8_t group_cipher;      // suite type

  uint8_t phy;  // PhyFlags

  bool has_bss_load;
  uint16_t sta_count;
  uint8_t channel_util;  // 0..255
  char country[3];

  // Hotspot 2.0 / 802.11u, from the beacon or probe response.
  bool hs20;
  uint8_t hs20_release;  // 1-based: the indication's version field + 1
  bool hs20_dgaf_disabled;
  bool interworking;
  uint8_t access_network_type;
  bool internet, asra, esr, uesa;
  bool has_venue;
  uint8_t venue_group, venue_type;
  bool has_hessid;
  uint8_t hessid[6];
  bool has_roaming_consortium;
  uint8_t rc_anqp_oi_count;
  uint8_t rc_oi_count;
  OI rc_ois[3];
  bool passpoint;  // HS2.0 indication present

  bool has_anqp;
  uint32_t anqp_updated_ms;
};

struct ApDetail {
  ApInfo ap;
  bool has_anqp;
  AnqpRecord anqp;
};

struct ClientInfo {
  uint8_t mac[6];
  bool randomized;  // locally administered
  int8_t rssi_last, rssi_avg, rssi_peak;
  uint32_t first_seen_ms, last_seen_ms;
  uint32_t frames;
  bool associated;
  uint8_t bssid[6];  // valid when associated
  Band band;
  uint8_t channel;
  bool power_save;  // seen sending PS-Poll or (QoS) Null data
  uint32_t wildcard_probes;
  uint8_t n_probed;
  uint8_t probed_len[8];
  uint8_t probed[8][32];  // raw SSID octets
};

enum class ApSort : uint8_t { Rssi, Ssid, Channel, LastSeen };
enum class ClientSort : uint8_t { Rssi, LastSeen, Mac, Frames };

struct Stats {
  uint32_t frames;
  uint32_t by_type[4][16];  // [type][subtype]
  uint32_t malformed;
  uint32_t ap_evictions, client_evictions, anqp_evictions, pending_evictions, gas_evictions;
  uint32_t gas_responses;      // Initial + Comeback responses carrying ANQP
  uint32_t gas_non_anqp;       // GAS with another advertisement protocol
  uint32_t gas_dropped;        // fragment gap, overflow or non-zero status
  uint32_t anqp_completed;
  uint32_t anqp_pending;       // completions parked for an AP not yet seen
  uint32_t last_ingest_ms;
  uint16_t aps, clients, pending;
};

struct Config {
  uint16_t max_aps = 256;
  uint16_t max_clients = 256;
  uint16_t max_anqp = 32;      // ANQP records; attached and pending share this pool
  uint16_t max_pending = 24;   // ANQP results waiting for their AP's first beacon
  uint16_t max_gas = 4;        // concurrent comeback reassemblies
  uint16_t gas_buf_bytes = 2048;
  uint8_t rssi_avg_shift = 3;  // EMA weight 1/8
  void* (*alloc)(size_t) = nullptr;  // nullptr: malloc
  void (*dealloc)(void*) = nullptr;  // nullptr: free
  Lock* lock = nullptr;              // nullptr: no locking
};

class Survey {
 public:
  Survey() = default;
  ~Survey();
  Survey(const Survey&) = delete;
  Survey& operator=(const Survey&) = delete;

  // One-shot allocation of every table. Returns false on allocation failure (nothing is kept).
  bool init(const Config& cfg = Config());
  static size_t memory_required(const Config& cfg);

  // `frame` starts at Frame Control, no radiotap, FCS stripped.
  void ingest(const uint8_t* frame, size_t len, const RxMeta& meta);

  // Copy up to `max` rows, sorted, into `out`. Returns rows written; `*total` gets the match count.
  // passpoint_only means HS2.0; include_interworking additionally admits IE 107-only APs.
  size_t aps(ApInfo* out, size_t max, ApSort sort, uint8_t band_mask = BAND_ALL,
             bool passpoint_only = false, size_t* total = nullptr,
             bool include_interworking = false) const;
  bool ap_detail(const uint8_t bssid[6], ApDetail* out) const;
  // ANQP for a BSSID whether attached to an AP or still pending.
  bool anqp_for(const uint8_t bssid[6], AnqpRecord* out) const;
  size_t pending_bssids(uint8_t (*out)[6], size_t max) const;

  size_t clients(ClientInfo* out, size_t max, ClientSort sort,
                 const uint8_t* associated_to = nullptr, size_t* total = nullptr) const;

  Stats stats() const;
  void reset();

  struct Impl;

 private:
  Impl* impl_ = nullptr;
};

// UI formatting. Returned strings are static.
const char* security_label(Security s, bool suite_b = false);
const char* width_label(Width w);
const char* phy_label(uint8_t phy_flags);  // "Wi-Fi 7" / "Wi-Fi 6" / "Wi-Fi 5" / "Wi-Fi 4" / "Legacy"
const char* band_label(Band b);
const char* access_network_type_label(uint8_t t);
const char* eap_method_label(uint8_t m);
const char* anqp_element_label(uint16_t info_id);

// Writes "<SSID>" for display (non-printable octets as '?'), or "" for hidden. Returns `out`.
char* ssid_to_string(const uint8_t* ssid, uint8_t len, char* out, size_t cap);
// Hex OI, uppercase, no separators (e.g. "5A03BA0000"). `out` needs 31 bytes.
char* oi_to_string(const OI& oi, char out[31]);
// "EAP-TTLS (MSCHAPv2, username/password)". Returns `out`.
char* nai_eap_summary(const NaiEap& e, char* out, size_t cap);

}  // namespace wlc
