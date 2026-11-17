#pragma once
// Tigo TAP <-> CCA RS-485 protocol decoder (receive only).
//
// No ESPHome dependency: it is compiled and tested on the host too
// (tests/host_test.cpp). Field layouts follow the CCA's own decoder in firmware
// 4.0.8 (meshdcd); see the tigo_server project, docs/PROTOCOL-NOTES.md.

#include <cstddef>
#include <cstdint>
#include <functional>

namespace tigo_proto {

// One decoded power report (PV packet type 0x31).
struct PowerReport {
  uint16_t node_id = 0;       // PV node ID (TUID) of the sender
  float voltage_in = 0;       // V
  float voltage_out = 0;      // V
  uint8_t duty_cycle = 0;     // 0-255
  uint16_t current_raw = 0;   // 12-bit raw input current, scale depends on hardware ID
  bool temperature_valid = false;  // Status bit 0x20
  float temperature = 0;      // degrees C, only meaningful when temperature_valid
  uint8_t jstatus = 0;        // payload byte 7
  uint16_t status = 0;        // payload bytes 8-9
  uint16_t slot_counter = 0;  // payload bytes 10-11
  uint8_t rssi = 0;           // raw 0-255, 0 = no usable value
};

// Input current scale (A per unit) for a hardware ID, as the CCA does it.
// hw_id < 0 means unknown: 0.005, the most common TS4 value.
float current_scale(int hw_id);

class Decoder {
 public:
  std::function<void(const PowerReport &)> on_power;
  // Node table response entry: PV node ID <-> 64-bit long address
  std::function<void(uint16_t node_id, uint64_t long_address)> on_node;
  // "Mnode Version K8.0120 (2D)" -> hardware ID 0x2D
  std::function<void(uint16_t node_id, uint8_t hw_id)> on_hw_id;

  // Feed raw bytes from the RS-485 bus.
  void feed(uint8_t byte);
  void feed(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++)
      feed(data[i]);
  }

  // Counters for diagnostics
  uint32_t frames_ok = 0;
  uint32_t frames_bad_crc = 0;
  uint32_t frames_too_long = 0;
  uint32_t power_reports = 0;

  // Exposed for tests
  static uint16_t crc16(const uint8_t *data, size_t len);
  void process_frame(const uint8_t *frame, size_t len);

 private:
  static constexpr size_t MAX_FRAME = 512;
  uint8_t buf_[MAX_FRAME];
  size_t len_ = 0;
  bool in_frame_ = false;
  bool escape_ = false;
  bool overflow_ = false;

  void process_receive_response(const uint8_t *payload, size_t len);
  void process_command_response(const uint8_t *payload, size_t len);
  void process_pv_packet(uint8_t type, uint16_t node_id, const uint8_t *data, size_t len);
  void process_string(uint16_t node_id, const uint8_t *data, size_t len);
};

}  // namespace tigo_proto
