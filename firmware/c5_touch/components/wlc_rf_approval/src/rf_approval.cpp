// SPDX-License-Identifier: MIT
#include "rf_approval.hpp"
#include "rf_public_key.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/sha256.h"
#include "esp_mac.h"
#include "nvs.h"
#include <cstring>
namespace wlc::rf {
namespace {
bool decode(const char *s, size_t n, uint8_t *out, size_t bytes) {
    size_t count = 0; uint32_t acc = 0; unsigned bits = 0;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i]; int v = c >= 'A' && c <= 'Z' ? c-'A' : c >= 'a' && c <= 'z' ? c-'a'+26 : c >= '0' && c <= '9' ? c-'0'+52 : c == '-' ? 62 : c == '_' ? 63 : -1;
        if (v < 0) return false;
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; if (count >= bytes) return false; out[count++] = (uint8_t)(acc >> bits); }
    }
    return count == bytes && (!bits || !(acc & ((1u << bits)-1)));
}
}
bool verify(const char *permit, const uint8_t mac[6], uint32_t &limit_ms) {
    limit_ms = 0;
    if (!permit || strlen(permit) != 140 || strncmp(permit,"WRF1.",5) || permit[53] != '.') return false;
    uint8_t payload[36], signature[64], hash[32];
    if (!decode(permit+5,48,payload,sizeof(payload)) || !decode(permit+54,86,signature,sizeof(signature)) ||
        memcmp(payload,"WLCRF001",8) || memcmp(payload+8,mac,6) || payload[14] || payload[15] != 7) return false;
    uint32_t seconds = (uint32_t)payload[32]<<24 | (uint32_t)payload[33]<<16 | (uint32_t)payload[34]<<8 | payload[35];
    if (!seconds || seconds > kMaxSessionMs/1000 || mbedtls_sha256(payload,sizeof(payload),hash,0)) return false;
    mbedtls_ecp_group group; mbedtls_ecp_point key; mbedtls_mpi r,s;
    mbedtls_ecp_group_init(&group); mbedtls_ecp_point_init(&key); mbedtls_mpi_init(&r); mbedtls_mpi_init(&s);
    bool ok = mbedtls_ecp_group_load(&group,MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_ecp_point_read_binary(&group,&key,kRfApprovalKey,sizeof(kRfApprovalKey)) == 0 &&
        mbedtls_mpi_read_binary(&r,signature,32) == 0 && mbedtls_mpi_read_binary(&s,signature+32,32) == 0 &&
        mbedtls_ecdsa_verify(&group,hash,sizeof(hash),&key,&r,&s) == 0;
    mbedtls_mpi_free(&s); mbedtls_mpi_free(&r); mbedtls_ecp_point_free(&key); mbedtls_ecp_group_free(&group);
    if (ok) limit_ms = seconds*1000;
    return ok;
}
bool approved(uint32_t *limit_ms) {
    nvs_handle_t h; char permit[141]; size_t n=sizeof(permit); uint8_t mac[6]; uint32_t limit=0;
    if (nvs_open("wlc_rf",NVS_READONLY,&h) != ESP_OK) return false;
    bool ok = nvs_get_str(h,"permit",permit,&n) == ESP_OK;
    nvs_close(h); esp_read_mac(mac,ESP_MAC_WIFI_STA);
    ok = ok && verify(permit,mac,limit);
    if (limit_ms) *limit_ms = ok ? limit : 0;
    return ok;
}
bool install(const char *permit) {
    uint8_t mac[6]; uint32_t limit; esp_read_mac(mac,ESP_MAC_WIFI_STA);
    if (!verify(permit,mac,limit)) return false;
    nvs_handle_t h; if (nvs_open("wlc_rf",NVS_READWRITE,&h) != ESP_OK) return false;
    bool ok=nvs_set_str(h,"permit",permit)==ESP_OK && nvs_commit(h)==ESP_OK;
    nvs_close(h); return ok;
}
bool forget() {
    nvs_handle_t h; if (nvs_open("wlc_rf",NVS_READWRITE,&h) != ESP_OK) return false;
    esp_err_t err=nvs_erase_key(h,"permit");
    bool ok=(err==ESP_OK || err==ESP_ERR_NVS_NOT_FOUND) && nvs_commit(h)==ESP_OK;
    nvs_close(h); return ok;
}
}
