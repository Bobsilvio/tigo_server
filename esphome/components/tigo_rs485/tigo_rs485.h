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
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#include "esphome/core/helpers.h"
#ifdef USE_WEBSERVER
#include "esphome/components/web_server_base/web_server_base.h"
#endif
#include <array>
#include <cmath>
#include <string>

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

// Energy history kept in RAM for the web dashboard: 96 buckets of 15 minutes = 24 hours
static const uint32_t HISTORY_BUCKET_MS = 15 * 60 * 1000;
static const size_t HISTORY_BUCKETS = 96;

struct Panel {
  uint64_t long_address{0};  // 0 when the panel is configured by node_id
  int node_id{-1};           // -1 when the panel is configured by long_address
  std::string label;         // name shown on the web dashboard
  // Last values received, kept for the web dashboard
  int last_node{-1};
  float power{NAN}, voltage_in{NAN}, voltage_out{NAN}, current{NAN}, temperature{NAN}, rssi{NAN}, duty{NAN};
  float energy_wh{0};  // since boot
  uint64_t last_report{0};
  std::array<float, HISTORY_BUCKETS> history{};  // Wh per bucket
  uint64_t history_bucket{0};                    // bucket index of the newest entry
#ifdef USE_SENSOR
  sensor::Sensor *sensors[PANEL_SENSOR_COUNT]{};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *address_sensor{nullptr};  // automatic slots only
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
  void set_panel_label(uint64_t long_address, int node_id, const char *label);
  void set_slot_label(size_t slot, const char *label);
#ifdef USE_TEXT_SENSOR
  void set_unconfigured_text_sensor(text_sensor::TextSensor *s) { this->unconfigured_sensor_ = s; }
#endif

  // Automatic slots: panels not configured by address take the first free slot as soon
  // as their long address is known; the assignment is kept in flash.
#ifdef USE_SENSOR
  void set_slot_sensor(size_t slot, uint8_t index, sensor::Sensor *s);
#endif
#ifdef USE_TEXT_SENSOR
  void set_slot_address_sensor(size_t slot, text_sensor::TextSensor *s);
#endif
  void reset_slots();
  // Thread-safe: the reset runs in the main loop (the web handler runs in another task)
  void request_reset() { this->reset_requested_ = true; }

#ifdef USE_WEBSERVER
  // Web dashboard (served at "/" when web_server is configured)
  void set_dashboard(const uint8_t *html, size_t size) {
    this->dashboard_html_ = html;
    this->dashboard_size_ = size;
  }
  const uint8_t *dashboard_html() const { return this->dashboard_html_; }
  size_t dashboard_size() const { return this->dashboard_size_; }
  std::string live_json();
  std::string history_json();
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
  Panel *slot_(size_t slot);
  Panel *assign_slot_(uint64_t long_address);
  void publish_slot_address_(Panel &p);
  void advance_history_(Panel &p, uint64_t bucket);
  uint64_t uptime_ms_();
  template<typename F> void for_each_panel_(F f) {
    for (auto *p : this->panels_)
      f(*p);
    for (auto *p : this->slots_)
      f(*p);
  }
  void load_slots_();
  void save_slots_();

  tigo_proto::Decoder decoder_;
  std::vector<Panel *> panels_;
  std::vector<Panel *> slots_;
  std::vector<NodeInfo> nodes_;
  std::vector<std::string> unconfigured_;
  bool unconfigured_dirty_{false};
  bool nodes_dirty_{false};
  uint32_t last_save_{0};
  uint32_t last_check_{0};
  uint32_t offline_timeout_{300000};
  ESPPreferenceObject pref_;
  ESPPreferenceObject slots_pref_;
  // Guards panels, nodes and history between loop() and the web handler task
  Mutex lock_;
  volatile bool reset_requested_{false};
  uint64_t uptime_{0};
  uint32_t last_millis_{0};
#ifdef USE_WEBSERVER
  const uint8_t *dashboard_html_{nullptr};
  size_t dashboard_size_{0};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *unconfigured_sensor_{nullptr};
#endif
};

#ifdef USE_WEBSERVER
class DashboardHandler : public AsyncWebHandler {
 public:
  explicit DashboardHandler(TigoRS485 *parent) : parent_(parent) {}
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  bool isRequestHandlerTrivial() const override { return false; }

 protected:
  TigoRS485 *parent_;
};
#endif

#ifdef USE_BUTTON
class ResetSlotsButton : public button::Button, public Parented<TigoRS485> {
 protected:
  void press_action() override { this->parent_->request_reset(); }
};
#endif

}  // namespace tigo_rs485
}  // namespace esphome
