#pragma once

#include <vector>

#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

#include "tigo_protocol.h"

namespace esphome {
namespace tigo_rs485 {

// Index of each per-panel sensor, as passed by sensor.py
enum PanelSensor : uint8_t {
  PANEL_POWER = 0,
  PANEL_VOLTAGE_IN,
  PANEL_VOLTAGE_OUT,
  PANEL_CURRENT,
  PANEL_TEMPERATURE,
  PANEL_RSSI,
  PANEL_DUTY_CYCLE,
  PANEL_SENSOR_COUNT,
};

struct Panel {
  uint64_t long_address{0};  // 0 when the panel is configured by node_id
  int node_id{-1};           // -1 when the panel is configured by long_address
#ifdef USE_SENSOR
  sensor::Sensor *sensors[PANEL_SENSOR_COUNT]{};
#endif
  uint32_t last_seen{0};
  bool online{false};
};

// What the bus taught us about a node: its long address and hardware ID
struct NodeInfo {
  uint16_t node_id;
  uint64_t long_address;  // 0 = unknown
  int16_t hw_id;          // -1 = unknown
};

class TigoRS485 : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_offline_timeout(uint32_t ms) { this->offline_timeout_ = ms; }
#ifdef USE_SENSOR
  void set_panel_sensor(uint64_t long_address, int node_id, uint8_t index, sensor::Sensor *s);
#endif
#ifdef USE_TEXT_SENSOR
  void set_unconfigured_text_sensor(text_sensor::TextSensor *s) { this->unconfigured_sensor_ = s; }
#endif

 protected:
  void handle_power_(const tigo_proto::PowerReport &r);
  void handle_node_(uint16_t node_id, uint64_t long_address);
  void handle_hw_id_(uint16_t node_id, uint8_t hw_id);
  NodeInfo *node_(uint16_t node_id, bool create);
  Panel *panel_for_(uint16_t node_id, uint64_t long_address);
  void mark_offline_(Panel &p);
  void note_unconfigured_(uint16_t node_id, uint64_t long_address);
  void load_nodes_();
  void save_nodes_();

  tigo_proto::Decoder decoder_;
  std::vector<Panel *> panels_;
  std::vector<NodeInfo> nodes_;
  std::vector<std::string> unconfigured_;
  bool unconfigured_dirty_{false};
  bool nodes_dirty_{false};
  uint32_t last_save_{0};
  uint32_t last_check_{0};
  uint32_t offline_timeout_{300000};
  ESPPreferenceObject pref_;
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *unconfigured_sensor_{nullptr};
#endif
};

}  // namespace tigo_rs485
}  // namespace esphome
