// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>

#include "wlc_survey/survey.h"

namespace wlc {

size_t copy_text(char* dst, size_t cap, const uint8_t* src, size_t n, bool* truncated) {
  if (cap == 0) {
    if (truncated) *truncated = n > 0;
    return 0;
  }
  size_t k = n;
  const bool cut = n > cap - 1;
  if (cut) {
    k = cap - 1;
    // Never leave half a UTF-8 sequence: back up to the lead byte of the one we cut through.
    while (k > 0 && (src[k] & 0xC0) == 0x80) --k;
  }
  for (size_t i = 0; i < k; ++i) {
    const uint8_t c = src[i];
    dst[i] = (c < 0x20 || c == 0x7f) ? '?' : static_cast<char>(c);
  }
  dst[k] = '\0';
  if (truncated) *truncated = cut;
  return k;
}

char* mac_to_string(const uint8_t mac[6], char out[18]) {
  std::snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
                mac[5]);
  return out;
}

const char* security_label(Security s, bool suite_b) {
  switch (s) {
    case Security::Open: return "Open";
    case Security::OWE: return "OWE";
    case Security::WEP: return "WEP";
    case Security::WPA: return "WPA";
    case Security::WPA2Personal: return "WPA2-Personal";
    case Security::WPA3SAE: return "WPA3-SAE";
    case Security::WPA2WPA3: return "WPA2/WPA3";
    case Security::Enterprise: return suite_b ? "Enterprise (Suite-B)" : "Enterprise";
    case Security::Unknown: return "Unknown";
  }
  return "Unknown";
}

const char* width_label(Width w) {
  switch (w) {
    case Width::W20: return "20 MHz";
    case Width::W40: return "40 MHz";
    case Width::W80: return "80 MHz";
    case Width::W160: return "160 MHz";
    case Width::W320: return "320 MHz";
  }
  return "?";
}

const char* phy_label(uint8_t phy) {
  if (phy & PHY_EHT) return "Wi-Fi 7";
  if (phy & PHY_HE) return "Wi-Fi 6";
  if (phy & PHY_VHT) return "Wi-Fi 5";
  if (phy & PHY_HT) return "Wi-Fi 4";
  return "Legacy";
}

const char* band_label(Band b) {
  switch (b) {
    case Band::B2G4: return "2.4 GHz";
    case Band::B5G: return "5 GHz";
    case Band::B6G: return "6 GHz";
  }
  return "?";
}

const char* access_network_type_label(uint8_t t) {
  switch (t) {
    case 0: return "Private";
    case 1: return "Private with guest";
    case 2: return "Chargeable public";
    case 3: return "Free public";
    case 4: return "Personal device";
    case 5: return "Emergency services only";
    case 14: return "Test";
    case 15: return "Wildcard";
    default: return "Reserved";
  }
}

const char* eap_method_label(uint8_t m) {
  switch (m) {
    case 13: return "EAP-TLS";
    case 18: return "EAP-SIM";
    case 21: return "EAP-TTLS";
    case 23: return "EAP-AKA";
    case 25: return "PEAP";
    case 50: return "EAP-AKA'";
    default: return "EAP";
  }
}

const char* anqp_element_label(uint16_t id) {
  switch (id) {
    case anqp_id::QUERY_LIST: return "Query List";
    case anqp_id::CAPABILITY_LIST: return "Capability List";
    case anqp_id::VENUE_NAME: return "Venue Name";
    case anqp_id::EMERGENCY_CALL_NUMBER: return "Emergency Call Number";
    case anqp_id::NETWORK_AUTH_TYPE: return "Network Auth Type";
    case anqp_id::ROAMING_CONSORTIUM: return "Roaming Consortium";
    case anqp_id::IP_ADDR_TYPE: return "IP Address Type";
    case anqp_id::NAI_REALM: return "NAI Realm";
    case anqp_id::CELLULAR_3GPP: return "3GPP Cellular";
    case anqp_id::AP_GEOSPATIAL: return "AP Geospatial Location";
    case anqp_id::AP_CIVIC: return "AP Civic Location";
    case anqp_id::AP_LOCATION_URI: return "AP Location Public Identifier URI";
    case anqp_id::DOMAIN_NAME: return "Domain Name";
    case anqp_id::EMERGENCY_ALERT_URI: return "Emergency Alert URI";
    case anqp_id::EMERGENCY_NAI: return "Emergency NAI";
    case anqp_id::VENUE_URL: return "Venue URL";
    case anqp_id::VENDOR_SPECIFIC: return "Vendor Specific";
    default: return "ANQP Element";
  }
}

char* ssid_to_string(const uint8_t* ssid, uint8_t len, char* out, size_t cap) {
  copy_text(out, cap, ssid, len, nullptr);
  return out;
}

char* oi_to_string(const OI& oi, char out[31]) {
  size_t k = 0;
  for (uint8_t i = 0; i < oi.len && i < 15; ++i) k += static_cast<size_t>(std::snprintf(out + k, 31 - k, "%02X", oi.b[i]));
  out[k] = '\0';
  return out;
}

namespace {
const char* non_eap_label(uint8_t v) {
  switch (v) {
    case 1: return "PAP";
    case 2: return "CHAP";
    case 3: return "MSCHAP";
    case 4: return "MSCHAPv2";
    default: return nullptr;
  }
}
const char* credential_label(uint8_t v) {
  switch (v) {
    case 1: return "SIM";
    case 2: return "USIM";
    case 3: return "NFC";
    case 4: return "hardware token";
    case 5: return "softoken";
    case 6: return "certificate";
    case 7: return "username/password";
    case 9: return "anonymous";
    case 10: return "vendor";
    default: return nullptr;
  }
}
}  // namespace

char* nai_eap_summary(const NaiEap& e, char* out, size_t cap) {
  if (cap == 0) return out;
  const char* parts[3] = {non_eap_label(e.non_eap_inner),
                          e.inner_eap ? eap_method_label(e.inner_eap) : nullptr,
                          credential_label(e.credential)};
  int k = std::snprintf(out, cap, "%s", eap_method_label(e.method));
  bool open = false;
  for (const char* p : parts) {
    if (!p || k < 0 || static_cast<size_t>(k) >= cap) continue;
    k += std::snprintf(out + k, cap - static_cast<size_t>(k), "%s%s", open ? ", " : " (", p);
    open = true;
  }
  if (open && k >= 0 && static_cast<size_t>(k) < cap) std::snprintf(out + k, cap - static_cast<size_t>(k), ")");
  return out;
}

}  // namespace wlc
