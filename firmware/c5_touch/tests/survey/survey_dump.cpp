// SPDX-License-Identifier: MIT
//
// Feeds a pcap through wlc::Survey and prints the resulting tables as JSON, for golden.py to
// compare against tshark. Strings are emitted as hex of the stored bytes so the comparison is
// byte-exact and indifferent to encoding.
#include <cstdio>
#include <memory>
#include <string>

#include "pcap_reader.h"
#include "wlc_survey/survey.h"

namespace {

std::string hex(const void* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) {
    s += d[b[i] >> 4];
    s += d[b[i] & 15];
  }
  return s;
}
std::string q(const std::string& s) { return "\"" + s + "\""; }
std::string mac(const uint8_t* m) {
  char b[18];
  return q(wlc::mac_to_string(m, b));
}

template <size_t N>
std::string thex(const wlc::Text<N>& t) {
  return q(hex(t.s, t.len));
}

std::string anqp_json(const wlc::AnqpRecord& r) {
  std::string o = "{\"responses\": " + std::to_string(r.responses) + ", \"present\": " + std::to_string(r.present) +
                  ", \"truncated\": " + (r.truncated ? "true" : "false");
  o += ", \"domains\": [";
  for (int i = 0; i < r.n_domains; ++i) o += (i ? ", " : "") + thex(r.domains[i]);
  o += "], \"nai_realms\": [";
  for (int i = 0; i < r.n_nai_realms; ++i) {
    const auto& n = r.nai_realms[i];
    o += std::string(i ? ", " : "") + "{\"realm\": " + thex(n.realm) + ", \"eap\": [";
    for (int k = 0; k < n.n_eap; ++k) o += (k ? ", " : "") + std::to_string(n.eap[k].method);
    o += "]}";
  }
  o += "], \"venue_names\": [";
  for (int i = 0; i < r.n_venue_names; ++i) o += (i ? ", " : "") + thex(r.venue_names[i].name);
  o += "], \"roaming_ois\": [";
  for (int i = 0; i < r.n_roaming_ois; ++i) o += (i ? ", " : "") + q(hex(r.roaming_ois[i].b, r.roaming_ois[i].len));
  o += "], \"plmns\": [";
  for (int i = 0; i < r.n_plmns; ++i) o += (i ? ", " : "") + q(std::string(r.plmns[i].mcc) + "-" + r.plmns[i].mnc);
  o += "], \"operator_names\": [";
  for (int i = 0; i < r.n_operator_names; ++i) o += (i ? ", " : "") + thex(r.operator_names[i].name);
  o += "]}";
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: survey_dump file.pcap[ng]\n");
    return 2;
  }
  pcap::Reader rd;
  std::string err;
  if (!rd.open(argv[1], &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }
  wlc::Config cfg;
  cfg.max_aps = 2048;
  cfg.max_clients = 2048;
  cfg.max_anqp = 512;
  cfg.max_pending = 256;
  wlc::Survey s;
  if (!s.init(cfg)) return 2;
  pcap::Packet p;
  uint32_t ingested = 0;
  while (rd.next(&p)) {
    s.ingest(p.frame, p.len, p.meta);
    ++ingested;
  }

  static wlc::ApInfo aps[2048];
  static wlc::ClientInfo cls[2048];
  auto d = std::make_unique<wlc::ApDetail>();
  auto rec = std::make_unique<wlc::AnqpRecord>();
  const size_t n = s.aps(aps, 2048, wlc::ApSort::LastSeen);
  const auto st = s.stats();

  std::printf("{\"records\": %u, \"bad_fcs\": %u, \"skipped\": %u, \"ingested\": %u, \"malformed\": %u,\n",
              rd.records, rd.bad_fcs, rd.skipped, ingested, st.malformed);
  std::printf(" \"gas_responses\": %u, \"anqp_completed\": %u, \"gas_dropped\": %u,\n \"aps\": [\n",
              st.gas_responses, st.anqp_completed, st.gas_dropped);
  for (size_t i = 0; i < n; ++i) {
    const auto& a = aps[i];
    std::string akms;
    for (int k = 0; k < 32; ++k) {
      if (a.akm_mask & (1u << k)) akms += (akms.empty() ? "" : ", ") + std::to_string(k);
    }
    std::string ois;
    for (int k = 0; k < a.rc_oi_count; ++k) ois += (k ? ", " : "") + q(hex(a.rc_ois[k].b, a.rc_ois[k].len));
    std::string anqp = "null";
    if (a.has_anqp && s.ap_detail(a.bssid, d.get()) && d->has_anqp) anqp = anqp_json(d->anqp);
    std::printf(
        "  {\"bssid\": %s, \"ssid\": %s, \"hidden\": %s, \"band\": %d, \"channel\": %u, \"rx_channel\": %u, "
        "\"width\": %s, \"security\": %s, \"akms\": [%s], \"pmf_capable\": %s, \"pmf_required\": %s, "
        "\"ht\": %s, \"vht\": %s, \"he\": %s, \"eht\": %s, \"interworking\": %s, \"hs20\": %s, "
        "\"hs20_release\": %u, \"passpoint\": %s, \"rc_ois\": [%s], \"rssi_avg\": %d, \"beacons\": %u, "
        "\"probe_resps\": %u, \"anqp\": %s}%s\n",
        mac(a.bssid).c_str(), q(hex(a.ssid, a.ssid_len)).c_str(), a.hidden ? "true" : "false", int(a.band),
        a.channel, a.rx_channel, q(wlc::width_label(a.width)).c_str(),
        q(wlc::security_label(a.security, a.suite_b)).c_str(), akms.c_str(), a.pmf_capable ? "true" : "false",
        a.pmf_required ? "true" : "false", (a.phy & wlc::PHY_HT) ? "true" : "false",
        (a.phy & wlc::PHY_VHT) ? "true" : "false", (a.phy & wlc::PHY_HE) ? "true" : "false",
        (a.phy & wlc::PHY_EHT) ? "true" : "false", a.interworking ? "true" : "false", a.hs20 ? "true" : "false",
        a.hs20_release, a.passpoint ? "true" : "false", ois.c_str(), a.rssi_avg, a.beacons, a.probe_resps,
        anqp.c_str(), i + 1 < n ? "," : "");
  }
  std::printf(" ],\n \"pending\": [\n");
  static uint8_t pend[256][6];
  const size_t np = s.pending_bssids(pend, 256);
  for (size_t i = 0; i < np; ++i) {
    s.anqp_for(pend[i], rec.get());
    std::printf("  {\"bssid\": %s, \"anqp\": %s}%s\n", mac(pend[i]).c_str(), anqp_json(*rec).c_str(),
                i + 1 < np ? "," : "");
  }
  std::printf(" ],\n \"clients\": [\n");
  const size_t nc = s.clients(cls, 2048, wlc::ClientSort::Mac);
  for (size_t i = 0; i < nc; ++i) {
    const auto& c = cls[i];
    std::printf("  {\"mac\": %s, \"randomized\": %s, \"associated\": %s, \"bssid\": %s, \"frames\": %u, "
                "\"probes\": %u, \"wildcard_probes\": %u}%s\n",
                mac(c.mac).c_str(), c.randomized ? "true" : "false", c.associated ? "true" : "false",
                c.associated ? mac(c.bssid).c_str() : "null", c.frames, c.n_probed, c.wildcard_probes,
                i + 1 < nc ? "," : "");
  }
  std::printf(" ]\n}\n");
  return 0;
}
