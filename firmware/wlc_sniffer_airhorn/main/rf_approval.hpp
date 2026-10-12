// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace wlc::rf {
constexpr uint32_t kMaxSessionMs = 900000; // 15 minutes; includes slow 5 GHz traversal.
constexpr const char *kWarning = "RF output can disrupt nearby networks and trigger DFS channel changes. Use only inside a verified RF shielded enclosure / Faraday cage or a properly terminated conducted test setup. Approval does not verify shielding.";
constexpr const char *kApprovalUrl = "https://pcapai.app/rf-approval/";
struct Session {
    uint32_t deadline = 0;
    bool armed = false;
    bool active(uint32_t now) const { return armed && (int32_t)(deadline - now) > 0; }
    uint32_t remaining(uint32_t now) const { return active(now) ? deadline - now : 0; }
    bool unlock(bool approved, bool acknowledged, uint32_t now, uint32_t limit = kMaxSessionMs) {
        if (!approved || !acknowledged || active(now) || !limit || limit > kMaxSessionMs) return false;
        deadline = now + limit; armed = true; return true;
    }
    void lock() { armed = false; deadline = 0; }
    bool allows(uint32_t now, uint32_t duration) const { return duration > 0 && duration <= remaining(now); }
};
// Signed, board-bound approval. No private signing key or local override exists here.
bool verify(const char *permit, const uint8_t mac[6], uint32_t &limit_ms);
bool approved(uint32_t *limit_ms = nullptr);
bool install(const char *permit);
bool forget();
}
