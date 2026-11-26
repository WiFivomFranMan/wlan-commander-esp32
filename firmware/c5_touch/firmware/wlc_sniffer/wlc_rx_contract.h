#pragma once
#include <stdint.h>

// AGENT C5-RX: qualified ESP32-C5 Arduino3.3.11 / ESP-IDF5.5.5 adapter.
// Actual hardware reports dump_len == sig_len + 4, opposite the header comment.
// Byte-exact simultaneous Pro/C5 captures show sig_len includes four unavailable
// FCS bytes. Copy only sig_len - 4; never read that tail or invent an FCS.
// Other chips/SDK layouts require separate hardware qualification.
static constexpr uint8_t WLC_RX_RADIOTAP_FLAGS = 0; // FCS absent; not software-validated
enum class WlcRxRefusal { None, ReceiveError, IncompleteDump };
struct WlcRxDecision { uint16_t payload_length; WlcRxRefusal refusal; };
constexpr WlcRxDecision wlc_rx_decision(unsigned signal_length, unsigned dump_length,
                                      unsigned receive_state) {
  return receive_state != 0
      ? WlcRxDecision{0, WlcRxRefusal::ReceiveError}
      : (signal_length < 4 || signal_length > 16379 ||
         dump_length != signal_length + 4)
          ? WlcRxDecision{0, WlcRxRefusal::IncompleteDump}
          : WlcRxDecision{static_cast<uint16_t>(signal_length - 4), WlcRxRefusal::None};
}

// SDK reception metadata supplies channel/RSSI, but no qualified PHY-rate mapping
// is implemented here. Omit Rate, MCS, NSS and modulation claims instead of
// substituting a 1 Mbps/CCK value. Missing channel remains absent as well.
inline int wlc_build_rx_radiotap(uint8_t* out, int8_t rssi, uint8_t ch) {
  const bool channel_known = (ch >= 1 && ch <= 14) || (ch >= 36 && ch <= 177);
  const uint32_t present = (1u << 1) | (1u << 5) | (channel_known ? (1u << 3) : 0);
  out[0] = 0; out[1] = 0;
  out[4] = present & 0xff; out[5] = (present >> 8) & 0xff;
  out[6] = (present >> 16) & 0xff; out[7] = (present >> 24) & 0xff;
  int o = 8;
  out[o++] = WLC_RX_RADIOTAP_FLAGS;
  if (channel_known) {
    out[o++] = 0; // Channel's required 16-bit alignment; not an invented Rate field.
    const uint16_t freq = ch <= 14 ? (ch == 14 ? 2484 : 2407 + ch * 5) : 5000 + ch * 5;
    const uint16_t band = ch <= 14 ? 0x0080 : 0x0100; // No invented CCK/OFDM modulation.
    out[o++] = freq & 0xff; out[o++] = (freq >> 8) & 0xff;
    out[o++] = band & 0xff; out[o++] = (band >> 8) & 0xff;
  }
  out[o++] = static_cast<uint8_t>(rssi);
  out[2] = o & 0xff; out[3] = (o >> 8) & 0xff;
  return o;
}
