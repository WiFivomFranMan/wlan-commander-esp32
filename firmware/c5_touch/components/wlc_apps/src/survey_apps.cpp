// SPDX-License-Identifier: MIT
#include "survey_apps.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "wlc_radio.hpp"
#include "channel_plan.hpp"

namespace wlc::apps {
namespace {


// Brookesia keeps up to two apps running. Only the app that last started the radio may stop it,
// or closing a backgrounded tool from Recents would silence the one on screen.
const SurveyApp *g_radio_owner;

const char *name_for(SurveyApp::Kind k)
{
    return k == SurveyApp::Kind::Scan ? "Scan" : k == SurveyApp::Kind::Clients ? "Clients" : "Passpoint";
}

// Short forms for list rows; the detail page keeps the full labels.
const char *short_security(wlc::Security sec, bool suite_b)
{
    switch (sec) {
    case wlc::Security::Open: return "Open";
    case wlc::Security::OWE: return "OWE";
    case wlc::Security::WEP: return "WEP";
    case wlc::Security::WPA: return "WPA";
    case wlc::Security::WPA2Personal: return "WPA2";
    case wlc::Security::WPA3SAE: return "WPA3";
    case wlc::Security::WPA2WPA3: return "WPA2/3";
    case wlc::Security::Enterprise: return suite_b ? "Ent-192" : "802.1X";
    default: return "?";
    }
}

const char *short_width(wlc::Width w)
{
    switch (w) {
    case wlc::Width::W20: return "20";
    case wlc::Width::W40: return "40";
    case wlc::Width::W80: return "80";
    case wlc::Width::W160: return "160";
    case wlc::Width::W320: return "320";
    }
    return "?";
}

uint32_t age_s(uint32_t last_ms) { return ((uint32_t)(esp_timer_get_time() / 1000) - last_ms) / 1000; }

void write_ap(DetailList &w, const wlc::ApDetail &d)
{
    const wlc::ApInfo &a = d.ap;
    char mac[18], ssid[80];
    wlc::mac_to_string(a.bssid, mac);
    wlc::ssid_to_string(a.ssid, a.ssid_len, ssid, sizeof(ssid));
    w.section("NETWORK");
    w.field("SSID", "%s", a.hidden ? "(hidden)" : ssid);
    w.field("BSSID", "%s", mac);
    w.field("Channel", "%u  (%s)", a.channel, wlc::band_label(a.band));
    w.field("Width", "%s", wlc::width_label(a.width));
    w.field("Standard", "%s", wlc::phy_label(a.phy));
    w.section("SIGNAL");
    w.field("Now", "%d dBm", a.rssi_last);
    w.field("Avg / peak", "%d / %d dBm", a.rssi_avg, a.rssi_peak);
    w.field("Beacons", "%lu  (every %u TU)", (unsigned long)a.beacons, a.beacon_interval);
    w.field("Last seen", "%lu s ago", (unsigned long)age_s(a.last_seen_ms));
    if (a.has_bss_load) {
        w.field("Stations", "%u", a.sta_count);
        w.field("Utilisation", "%u%%", a.channel_util * 100 / 255);
    }
    w.section("SECURITY");
    w.field("Auth", "%s", wlc::security_label(a.security, a.suite_b));
    w.field("PMF", "%s", a.pmf_required ? "required" : a.pmf_capable ? "capable" : "off");
    if (a.hs20 || a.interworking) {
        w.section(a.hs20 ? "PASSPOINT" : "802.11u INTERWORKING");
        if (a.hs20) w.field("Hotspot 2.0", "release %u%s", a.hs20_release, a.hs20_dgaf_disabled ? ", DGAF off" : "");
        if (a.interworking) w.field("Network", "%s%s", wlc::access_network_type_label(a.access_network_type),
                                    a.internet ? ", internet" : "");
        if (a.has_venue) w.field("Venue", "group %u, type %u", a.venue_group, a.venue_type);
        if (a.has_roaming_consortium) w.field("RCOIs", "%u in beacon, %u more via ANQP", a.rc_oi_count, a.rc_anqp_oi_count);
    }
    if (!d.has_anqp) { if (a.passpoint) w.field("ANQP", "not answered yet"); return; }
    const wlc::AnqpRecord &r = d.anqp;
    w.section("ANQP");
    w.field("Responses", "%u", r.responses);
    for (int i = 0; i < r.n_operator_names; i++) w.field("Operator", "%s", r.operator_names[i].name.s);
    for (int i = 0; i < r.n_venue_names; i++) w.field("Venue", "%s", r.venue_names[i].name.s);
    for (int i = 0; i < r.n_domains; i++) w.field("Domain", "%s", r.domains[i].s);
    for (int i = 0; i < r.n_nai_realms; i++) {
        w.field("Realm", "%s", r.nai_realms[i].realm.s);
        for (int k = 0; k < r.nai_realms[i].n_eap; k++) {
            char eap[96];
            w.field("  EAP", "%s", wlc::nai_eap_summary(r.nai_realms[i].eap[k], eap, sizeof(eap)));
        }
    }
    for (int i = 0; i < r.n_roaming_ois; i++) { char oi[31]; w.field("RCOI", "%s", wlc::oi_to_string(r.roaming_ois[i], oi)); }
    for (int i = 0; i < r.n_plmns; i++) w.field("PLMN", "%s-%s", r.plmns[i].mcc, r.plmns[i].mnc);
    for (int i = 0; i < r.n_home_realms; i++) w.field("Home realm", "%s", r.home_realms[i].s);
    if (r.present & wlc::AF_WAN_METRICS)
        w.field("WAN", "%lu / %lu kbps", (unsigned long)r.wan.dl_kbps, (unsigned long)r.wan.ul_kbps);
    if (!r.civic_summary.empty()) w.field("Civic loc.", "%s", r.civic_summary.s);
    if (!r.geo_summary.empty()) w.field("Geo loc.", "%s", r.geo_summary.s);
    for (int i = 0; i < r.n_location_uris; i++) w.field("Location URI", "%s", r.location_uris[i].s);
    // Survey data, displayed as text. Not a control: nothing here can be tapped or dialled.
    for (int i = 0; i < r.n_emergency_numbers; i++) w.field("Emerg. no.", "%s (data)", r.emergency_numbers[i].s);
    for (int i = 0; i < r.n_emergency_alert_uris; i++) w.field("Emerg. alert", "%s (data)", r.emergency_alert_uris[i].s);
    if (!r.emergency_nai.empty()) w.field("Emerg. NAI", "%s (data)", r.emergency_nai.s);
    if (r.n_unknown) w.field("Other", "%u elements", r.n_unknown);
    if (r.truncated) w.field("Note", "some lists truncated");
}

}  // namespace

static const void *icon_for(SurveyApp::Kind k)
{
    return k == SurveyApp::Kind::Scan ? &wlc_icon_scan : k == SurveyApp::Kind::Clients ? &wlc_icon_clients
                                                                                      : &wlc_icon_passpoint;
}

SurveyApp::SurveyApp(Kind kind) : ToolApp(name_for(kind), icon_for(kind)), kind_(kind) {}

void SurveyApp::build(lv_obj_t *body)
{
    aps_ = static_cast<wlc::ApInfo *>(heap_caps_calloc(ListView::kRows, sizeof(wlc::ApInfo), MALLOC_CAP_SPIRAM));
    clients_ = static_cast<wlc::ClientInfo *>(heap_caps_calloc(ListView::kRows, sizeof(wlc::ClientInfo), MALLOC_CAP_SPIRAM));
    entries_ = static_cast<DetailEntry *>(heap_caps_malloc(sizeof(DetailEntry) * DetailList::kMax, MALLOC_CAP_SPIRAM));

    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 4, 0);
    if (kind_ != Kind::Clients) {
        lv_obj_t *bands = lv_obj_create(body);
        lv_obj_remove_style_all(bands);
        lv_obj_set_size(bands, LV_PCT(100), 28);
        lv_obj_set_flex_flow(bands, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(bands, 4, 0);
        static const char *labels[3] = {"All", "2.4", "5 GHz"};
        for (int i = 0; i < 3; i++) {
            lv_obj_t *b = lv_button_create(bands);
            lv_obj_set_height(b, 26);
            lv_obj_set_flex_grow(b, 1);
            lv_obj_center(make_label(b, labels[i], &lv_font_montserrat_12, lv_color_white()));
            lv_obj_set_user_data(b, (void *)(intptr_t)i);
            lv_obj_add_event_cb(b, [](lv_event_t *e) {
                auto *self = static_cast<SurveyApp *>(lv_event_get_user_data(e));
                int i = (int)(intptr_t)lv_obj_get_user_data(static_cast<lv_obj_t *>(lv_event_get_current_target(e)));
                self->set_band(i == 0 ? (wlc::BAND_2G4 | wlc::BAND_5G) : i == 1 ? wlc::BAND_2G4 : wlc::BAND_5G);
            }, LV_EVENT_CLICKED, this);
            band_buttons_[i] = b;
        }
    }
    {
        // Channel picker lives on the filter row; Clients has no band filter, so it gets its own row.
        lv_obj_t *row = kind_ == Kind::Clients ? body : lv_obj_get_child(body, 0);
        lv_obj_t *gear = lv_button_create(row);
        lv_obj_set_size(gear, kind_ == Kind::Clients ? LV_PCT(100) : 40, 26);
        lv_obj_set_style_bg_color(gear, lv_color_hex(0x2c3a48), 0);
        lv_obj_center(make_label(gear, kind_ == Kind::Clients ? LV_SYMBOL_SETTINGS "  Channels" : LV_SYMBOL_SETTINGS,
                                 &lv_font_montserrat_14, lv_color_white()));
        lv_obj_add_event_cb(gear, [](lv_event_t *e) {
            auto *self = static_cast<SurveyApp *>(lv_event_get_user_data(e));
            self->picker_.open(lv_screen_active(), [self]() { self->start_radio(); });
        }, LV_EVENT_CLICKED, this);
    }
    if (kind_ == Kind::Passpoint) {
        lv_obj_t *scope = lv_checkbox_create(body);
        lv_checkbox_set_text(scope, "Include Interworking-only");
        lv_obj_set_style_text_font(scope, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(scope, lv_color_white(), 0);
        if (include_interworking_) lv_obj_add_state(scope, LV_STATE_CHECKED);
        lv_obj_add_event_cb(scope, [](lv_event_t *e) {
            auto *self = static_cast<SurveyApp *>(lv_event_get_user_data(e));
            self->include_interworking_ = lv_obj_has_state(
                static_cast<lv_obj_t *>(lv_event_get_current_target(e)), LV_STATE_CHECKED);
            self->refresh_aps();
            if (g_radio_owner == self) refresh_passpoint_targets(self->include_interworking_);
        }, LV_EVENT_VALUE_CHANGED, this);
    }
    lv_obj_t *rows = lv_obj_create(body);
    lv_obj_remove_style_all(rows);
    lv_obj_set_width(rows, LV_PCT(100));
    lv_obj_set_flex_grow(rows, 1);
    list_.create(rows, [this](int i) { kind_ == Kind::Clients ? show_client(i) : show_ap(i); });
    set_band(band_mask_);

    start_radio();
    refresh();
}

void SurveyApp::start_radio()
{
    radio::Plan plan;
    // Passpoint needs dwell long enough for a GAS reply to come back on the same channel.
    build_plan(plan, kind_ == Kind::Clients, kind_ == Kind::Passpoint ? 400 : 0);
    radio::start(plan);
    g_radio_owner = this;
    // Only the Passpoint tool transmits; every other tool listens.
    if (kind_ == Kind::Passpoint) refresh_passpoint_targets(include_interworking_); else radio::set_targets(nullptr, 0, 0);
}

void SurveyApp::set_band(uint8_t mask)
{
    band_mask_ = mask;
    for (int i = 0; i < 3; i++) {
        if (!band_buttons_[i]) continue;
        bool on = (i == 0 && mask == (wlc::BAND_2G4 | wlc::BAND_5G)) || (i == 1 && mask == wlc::BAND_2G4) ||
                  (i == 2 && mask == wlc::BAND_5G);
        lv_obj_set_style_bg_color(band_buttons_[i], lv_color_hex(on ? 0x1e88e5 : 0x2c3a48), 0);
    }
    if (aps_) refresh_aps();
}

void SurveyApp::refresh()
{
    ticks_++;
    radio::Stats s = radio::stats();
    char status[48];
    if (kind_ == Kind::Passpoint) {
        // New Passpoint APs keep turning up while hopping; re-aim the pokes every 5 s.
        if (ticks_ % 10 == 0 && g_radio_owner == this) refresh_passpoint_targets(include_interworking_);
        snprintf(status, sizeof(status), "ch %u  q %lu", s.channel, (unsigned long)s.queries_sent);
    } else {
        wlc::Stats st = survey().stats();
        snprintf(status, sizeof(status), "ch %u  %u %s", s.channel,
                 kind_ == Kind::Clients ? st.clients : st.aps, kind_ == Kind::Clients ? "STA" : "APs");
    }
    if (status_) lv_label_set_text(status_, status);
    if (kind_ == Kind::Clients) refresh_clients(); else refresh_aps();

    if (detail_.is_open() && entries_ && detail_is_ap_) {
        DetailList w{entries_};
        auto *d = static_cast<wlc::ApDetail *>(heap_caps_malloc(sizeof(wlc::ApDetail), MALLOC_CAP_SPIRAM));
        if (d && survey().ap_detail(detail_mac_, d)) { write_ap(w, *d); detail_.show(w); }
        free(d);
    }
}

void SurveyApp::refresh_aps()
{
    size_t total = 0;
    size_t n = survey().aps(aps_, ListView::kRows, wlc::ApSort::Rssi, band_mask_, kind_ == Kind::Passpoint, &total, include_interworking_);
    list_.set_count((int)n);
    if (!n) list_.set_empty_text(total ? "Nothing on this band yet" : "Listening...");
    for (size_t i = 0; i < n; i++) {
        const wlc::ApInfo &a = aps_[i];
        char ssid[80], tag[12], sub[96], badge[8];
        wlc::ssid_to_string(a.ssid, a.ssid_len, ssid, sizeof(ssid));
        snprintf(tag, sizeof(tag), "ch %u", a.channel);
        if (kind_ == Kind::Passpoint)
            snprintf(sub, sizeof(sub), "%s  %s  %s  %s", a.hs20 ? "HS2.0" : "11u only",
                     short_security(a.security, a.suite_b),
                     a.band == wlc::Band::B2G4 ? "2.4 GHz" : "5 GHz",
                     a.has_anqp ? "ANQP " LV_SYMBOL_OK : "ANQP waiting");
        else
            snprintf(sub, sizeof(sub), "%s MHz   %s   %s", short_width(a.width),
                     short_security(a.security, a.suite_b), wlc::phy_label(a.phy));
        snprintf(badge, sizeof(badge), "%d", a.rssi_avg);
        list_.set_row((int)i, a.hidden ? "(hidden)" : ssid, tag, sub, badge, rssi_color(a.rssi_avg));
    }
}

void SurveyApp::refresh_clients()
{
    size_t n = survey().clients(clients_, ListView::kRows, wlc::ClientSort::Rssi);
    list_.set_count((int)n);
    wlc::ApDetail *ap = nullptr;
    for (size_t i = 0; i < n; i++) {
        const wlc::ClientInfo &c = clients_[i];
        char mac[18], tag[12], sub[96], badge[8];
        wlc::mac_to_string(c.mac, mac);
        snprintf(tag, sizeof(tag), "ch %u", c.channel);
        if (c.associated) {
            if (!ap) ap = static_cast<wlc::ApDetail *>(heap_caps_malloc(sizeof(wlc::ApDetail), MALLOC_CAP_SPIRAM));
            char ssid[80] = "";
            if (ap && survey().ap_detail(c.bssid, ap)) wlc::ssid_to_string(ap->ap.ssid, ap->ap.ssid_len, ssid, sizeof(ssid));
            snprintf(sub, sizeof(sub), "%son %s", c.randomized ? "random  " : "", ssid[0] ? ssid : "hidden AP");
        } else if (c.n_probed) {
            char ssid[40];
            wlc::ssid_to_string(c.probed[0], c.probed_len[0], ssid, sizeof(ssid));
            snprintf(sub, sizeof(sub), "%sprobing %s%s", c.randomized ? "random  " : "", ssid,
                     c.n_probed > 1 ? " +more" : "");
        } else {
            snprintf(sub, sizeof(sub), "%s%lu frames", c.randomized ? "random  " : "", (unsigned long)c.frames);
        }
        snprintf(badge, sizeof(badge), "%d", c.rssi_avg);
        list_.set_row((int)i, mac, tag, sub, badge, rssi_color(c.rssi_avg));
    }
    free(ap);
}

void SurveyApp::show_ap(int index)
{
    if (!aps_ || !entries_) return;
    memcpy(detail_mac_, aps_[index].bssid, 6);
    detail_is_ap_ = true;
    auto *d = static_cast<wlc::ApDetail *>(heap_caps_malloc(sizeof(wlc::ApDetail), MALLOC_CAP_SPIRAM));
    if (!d || !survey().ap_detail(detail_mac_, d)) { free(d); return; }
    DetailList w{entries_};
    write_ap(w, *d);
    char ssid[80];
    wlc::ssid_to_string(d->ap.ssid, d->ap.ssid_len, ssid, sizeof(ssid));
    detail_.open(lv_screen_active(), d->ap.hidden ? "(hidden)" : ssid);
    detail_.show(w);
    free(d);
}

void SurveyApp::show_client(int index)
{
    if (!clients_ || !entries_) return;
    const wlc::ClientInfo &c = clients_[index];
    detail_is_ap_ = false;
    DetailList w{entries_};
    char mac[18], bssid[18];
    wlc::mac_to_string(c.mac, mac);
    w.section("STATION");
    w.field("MAC", "%s", mac);
    w.field("Address", "%s", c.randomized ? "randomised (locally administered)" : "global (vendor OUI)");
    w.field("Channel", "%u", c.channel);
    if (c.associated) w.field("Associated", "%s", wlc::mac_to_string(c.bssid, bssid));
    if (c.power_save) w.field("Power save", "yes");
    w.section("SIGNAL");
    w.field("Now", "%d dBm", c.rssi_last);
    w.field("Avg / peak", "%d / %d dBm", c.rssi_avg, c.rssi_peak);
    w.field("Frames", "%lu", (unsigned long)c.frames);
    w.field("Last seen", "%lu s ago", (unsigned long)age_s(c.last_seen_ms));
    w.section("PROBES");
    w.field("Wildcard", "%lu", (unsigned long)c.wildcard_probes);
    for (int i = 0; i < c.n_probed; i++) {
        char ssid[80];
        w.field("SSID", "%s", wlc::ssid_to_string(c.probed[i], c.probed_len[i], ssid, sizeof(ssid)));
    }
    detail_.open(lv_screen_active(), mac);
    detail_.show(w);
}

void SurveyApp::teardown()
{
    detail_.close();
    picker_.close();
    if (g_radio_owner == this) { radio::stop(); g_radio_owner = nullptr; }
    else if (kind_ == Kind::Passpoint) radio::set_targets(nullptr, 0, 0);
    free(aps_); free(clients_); free(entries_);
    aps_ = nullptr; clients_ = nullptr; entries_ = nullptr;
    for (auto &b : band_buttons_) b = nullptr;
}

}
