#include "tigo_rs485.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace tigo_rs485 {

static const char *const TAG = "tigo_rs485";

// Node map persisted in flash: the CCA sends the node table rarely (at discovery),
// so after a reboot the long address of each node would otherwise stay unknown.
static const size_t MAX_STORED_NODES = 128;
static const uint32_t SAVE_INTERVAL_MS = 60000;

struct StoredNode {
  uint16_t node_id;
  int16_t hw_id;
  uint32_t long_hi;
  uint32_t long_lo;
} __attribute__((packed));

struct StoredNodes {
  uint16_t count;
  StoredNode nodes[MAX_STORED_NODES];
} __attribute__((packed));

static std::string long_hex(uint64_t v) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%08" PRIX32 "%08" PRIX32, uint32_t(v >> 32), uint32_t(v));
  return buf;
}

void TigoRS485::setup() {
  this->decoder_.on_power = [this](const tigo_proto::PowerReport &r) { this->handle_power_(r); };
  this->decoder_.on_node = [this](uint16_t n, uint64_t l) { this->handle_node_(n, l); };
  this->decoder_.on_hw_id = [this](uint16_t n, uint8_t h) { this->handle_hw_id_(n, h); };
  this->pref_ = global_preferences->make_preference<StoredNodes>(fnv1_hash("tigo_rs485_nodes_v1"));
  this->load_nodes_();
#ifdef USE_TEXT_SENSOR
  if (this->unconfigured_sensor_ != nullptr)
    this->unconfigured_sensor_->publish_state("");
#endif
}

void TigoRS485::loop() {
  uint8_t buf[64];
  size_t avail;
  while ((avail = this->available()) > 0) {
    size_t n = std::min(avail, sizeof(buf));
    if (!this->read_array(buf, n))
      break;
    this->decoder_.feed(buf, n);
  }

  uint32_t now = millis();
  if (now - this->last_check_ >= 1000) {
    this->last_check_ = now;
    for (auto *p : this->panels_)
      if (p->online && now - p->last_seen > this->offline_timeout_)
        this->mark_offline_(*p);
#ifdef USE_TEXT_SENSOR
    if (this->unconfigured_dirty_ && this->unconfigured_sensor_ != nullptr) {
      // Home Assistant states are limited to 255 characters
      std::string s;
      size_t shown = 0;
      for (const auto &u : this->unconfigured_) {
        if (s.size() + u.size() + 12 > 255)
          break;
        if (!s.empty())
          s += ",";
        s += u;
        shown++;
      }
      if (shown < this->unconfigured_.size())
        s += " +" + to_string(this->unconfigured_.size() - shown) + " more";
      this->unconfigured_sensor_->publish_state(s);
    }
#endif
    this->unconfigured_dirty_ = false;
  }

  if (this->nodes_dirty_ && now - this->last_save_ >= SAVE_INTERVAL_MS)
    this->save_nodes_();
}

#ifdef USE_SENSOR
void TigoRS485::set_panel_sensor(uint64_t long_address, int node_id, uint8_t index, sensor::Sensor *s) {
  if (index >= PANEL_SENSOR_COUNT)
    return;
  Panel *panel = nullptr;
  for (auto *p : this->panels_)
    if (p->long_address == long_address && p->node_id == node_id)
      panel = p;
  if (panel == nullptr) {
    panel = new Panel();  // NOLINT(cppcoreguidelines-owning-memory)
    panel->long_address = long_address;
    panel->node_id = node_id;
    this->panels_.push_back(panel);
  }
  panel->sensors[index] = s;
}
#endif

NodeInfo *TigoRS485::node_(uint16_t node_id, bool create) {
  for (auto &n : this->nodes_)
    if (n.node_id == node_id)
      return &n;
  if (!create)
    return nullptr;
  this->nodes_.push_back(NodeInfo{node_id, 0, -1});
  return &this->nodes_.back();
}

Panel *TigoRS485::panel_for_(uint16_t node_id, uint64_t long_address) {
  if (long_address != 0)
    for (auto *p : this->panels_)
      if (p->long_address == long_address)
        return p;
  for (auto *p : this->panels_)
    if (p->node_id == int(node_id))
      return p;
  return nullptr;
}

void TigoRS485::handle_node_(uint16_t node_id, uint64_t long_address) {
  if (long_address == 0)
    return;
  // A long address belongs to one node only: the CCA may have reassigned it
  for (auto &n : this->nodes_)
    if (n.long_address == long_address && n.node_id != node_id) {
      ESP_LOGI(TAG, "%s moved from node %04X to %04X", long_hex(long_address).c_str(), n.node_id, node_id);
      n.long_address = 0;
      n.hw_id = -1;
    }
  NodeInfo *n = this->node_(node_id, true);
  if (n->long_address != long_address) {
    ESP_LOGI(TAG, "Node %04X = %s", node_id, long_hex(long_address).c_str());
    if (n->long_address != 0)
      n->hw_id = -1;  // a different optimizer now: its hardware ID is not known yet
    n->long_address = long_address;
    this->nodes_dirty_ = true;
  }
}

void TigoRS485::handle_hw_id_(uint16_t node_id, uint8_t hw_id) {
  NodeInfo *n = this->node_(node_id, true);
  if (n->hw_id != hw_id) {
    ESP_LOGI(TAG, "Node %04X hardware ID %02X (current scale %.4f A)", node_id, hw_id,
             tigo_proto::current_scale(hw_id));
    n->hw_id = hw_id;
    this->nodes_dirty_ = true;
  }
}

void TigoRS485::handle_power_(const tigo_proto::PowerReport &r) {
  NodeInfo *n = this->node_(r.node_id, false);
  uint64_t long_address = n != nullptr ? n->long_address : 0;
  int hw_id = n != nullptr ? n->hw_id : -1;
  Panel *p = this->panel_for_(r.node_id, long_address);
  if (p == nullptr) {
    this->note_unconfigured_(r.node_id, long_address);
    return;
  }
  float current = float(r.current_raw) * tigo_proto::current_scale(hw_id);
  float power = r.voltage_in * current;  // the CCA computes Pin = Vin * Iin
  ESP_LOGV(TAG, "Node %04X: Vin %.2f Vout %.1f I %.3f P %.1f T %s RSSI %u", r.node_id, r.voltage_in,
           r.voltage_out, current, power, r.temperature_valid ? to_string(r.temperature).c_str() : "-", r.rssi);
  p->last_seen = millis();
  p->online = true;
#ifdef USE_SENSOR
  auto pub = [p](uint8_t i, float v) {
    if (p->sensors[i] != nullptr)
      p->sensors[i]->publish_state(v);
  };
  pub(PANEL_POWER, power);
  pub(PANEL_VOLTAGE_IN, r.voltage_in);
  pub(PANEL_VOLTAGE_OUT, r.voltage_out);
  pub(PANEL_CURRENT, current);
  pub(PANEL_DUTY_CYCLE, r.duty_cycle * 100.0f / 255.0f);
  if (r.temperature_valid)  // otherwise the field holds "Details", not a temperature
    pub(PANEL_TEMPERATURE, r.temperature);
  if (r.rssi != 0)  // 0 = no usable value
    pub(PANEL_RSSI, r.rssi);
#endif
}

void TigoRS485::mark_offline_(Panel &p) {
  p.online = false;
#ifdef USE_SENSOR
  for (auto *s : p.sensors)
    if (s != nullptr)
      s->publish_state(NAN);
#endif
}

void TigoRS485::note_unconfigured_(uint16_t node_id, uint64_t long_address) {
  char node_label[12];
  snprintf(node_label, sizeof(node_label), "node %04X", node_id);
  std::string id = long_address != 0 ? long_hex(long_address) : std::string(node_label);
  for (const auto &u : this->unconfigured_)
    if (u == id)
      return;
  ESP_LOGI(TAG, "Unconfigured panel reporting: %s (node %04X)", id.c_str(), node_id);
  this->unconfigured_.push_back(id);
  this->unconfigured_dirty_ = true;
}

void TigoRS485::load_nodes_() {
  StoredNodes stored{};
  if (!this->pref_.load(&stored) || stored.count > MAX_STORED_NODES)
    return;
  for (size_t i = 0; i < stored.count; i++) {
    const StoredNode &s = stored.nodes[i];
    this->nodes_.push_back(NodeInfo{s.node_id, (uint64_t(s.long_hi) << 32) | s.long_lo, s.hw_id});
  }
  ESP_LOGI(TAG, "Loaded %u nodes from flash", stored.count);
}

void TigoRS485::save_nodes_() {
  StoredNodes stored{};
  for (const auto &n : this->nodes_) {
    if (stored.count >= MAX_STORED_NODES)
      break;
    if (n.long_address == 0 && n.hw_id < 0)
      continue;
    stored.nodes[stored.count++] =
        StoredNode{n.node_id, n.hw_id, uint32_t(n.long_address >> 32), uint32_t(n.long_address)};
  }
  this->pref_.save(&stored);
  this->nodes_dirty_ = false;
  this->last_save_ = millis();
  ESP_LOGD(TAG, "Saved %u nodes", stored.count);
}

void TigoRS485::dump_config() {
  ESP_LOGCONFIG(TAG, "Tigo RS485 Monitor:");
  ESP_LOGCONFIG(TAG, "  Offline timeout: %" PRIu32 " s", this->offline_timeout_ / 1000);
  ESP_LOGCONFIG(TAG, "  Panels configured: %u", (unsigned) this->panels_.size());
  for (auto *p : this->panels_) {
    if (p->long_address != 0)
      ESP_LOGCONFIG(TAG, "    long address %s", long_hex(p->long_address).c_str());
    else
      ESP_LOGCONFIG(TAG, "    node %04X", p->node_id);
  }
  ESP_LOGCONFIG(TAG, "  Nodes known: %u", (unsigned) this->nodes_.size());
  for (const auto &n : this->nodes_)
    ESP_LOGCONFIG(TAG, "    node %04X = %s, hardware ID %s", n.node_id,
                  n.long_address ? long_hex(n.long_address).c_str() : "?",
                  n.hw_id >= 0 ? to_string(n.hw_id).c_str() : "?");
  this->check_uart_settings(38400);
}

}  // namespace tigo_rs485
}  // namespace esphome
