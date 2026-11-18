#include "tigo_rs485.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>

#include "esphome/core/application.h"
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

// Automatic slot assignments persisted in flash (long address of each slot, 0 = free)
static const size_t MAX_SLOTS = 64;

struct StoredSlots {
  uint16_t count;
  uint32_t addr[MAX_SLOTS][2];  // high, low
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
  this->slots_pref_ = global_preferences->make_preference<StoredSlots>(fnv1_hash("tigo_rs485_slots_v1"));
  this->load_slots_();
#ifdef USE_TEXT_SENSOR
  if (this->unconfigured_sensor_ != nullptr)
    this->unconfigured_sensor_->publish_state("");
#endif
  for (auto *p : this->slots_)
    this->publish_slot_address_(*p);
#ifdef USE_WEBSERVER
  // Registered before web_server's own handler (this component sets up earlier), so the
  // dashboard answers "/"; "/?esphome" still reaches the standard ESPHome page.
  if (this->dashboard_html_ != nullptr && web_server_base::global_web_server_base != nullptr)
    web_server_base::global_web_server_base->add_handler(new DashboardHandler(this));  // NOLINT
#endif
}

uint64_t TigoRS485::uptime_ms_() {
  uint32_t now = millis();
  this->uptime_ += uint32_t(now - this->last_millis_);
  this->last_millis_ = now;
  return this->uptime_;
}

void TigoRS485::advance_history_(Panel &p, uint64_t bucket) {
  if (bucket <= p.history_bucket)
    return;
  uint64_t steps = bucket - p.history_bucket;
  if (steps >= HISTORY_BUCKETS) {
    p.history.fill(0);
  } else {
    for (uint64_t b = p.history_bucket + 1; b <= bucket; b++)
      p.history[b % HISTORY_BUCKETS] = 0;
  }
  p.history_bucket = bucket;
}

void TigoRS485::loop() {
  LockGuard guard(this->lock_);
  if (this->reset_requested_) {
    this->reset_requested_ = false;
    this->reset_slots();
  }
  uint8_t buf[64];
  size_t avail;
  while ((avail = this->available()) > 0) {
    size_t n = std::min(avail, sizeof(buf));
    if (!this->read_array(buf, n))
      break;
    this->decoder_.feed(buf, n);
  }

  uint32_t now = millis();
  this->uptime_ms_();
  if (now - this->last_check_ >= 1000) {
    this->last_check_ = now;
    for (auto *list : {&this->panels_, &this->slots_})
      for (auto *p : *list)
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

Panel *TigoRS485::slot_(size_t slot) {
  if (slot >= MAX_SLOTS)
    return nullptr;
  while (this->slots_.size() <= slot)
    this->slots_.push_back(new Panel());  // NOLINT(cppcoreguidelines-owning-memory)
  return this->slots_[slot];
}

#ifdef USE_SENSOR
void TigoRS485::set_slot_sensor(size_t slot, uint8_t index, sensor::Sensor *s) {
  Panel *p = this->slot_(slot);
  if (p != nullptr && index < PANEL_SENSOR_COUNT)
    p->sensors[index] = s;
}
#endif

#ifdef USE_TEXT_SENSOR
void TigoRS485::set_slot_address_sensor(size_t slot, text_sensor::TextSensor *s) {
  Panel *p = this->slot_(slot);
  if (p != nullptr)
    p->address_sensor = s;
}
#endif

void TigoRS485::publish_slot_address_(Panel &p) {
#ifdef USE_TEXT_SENSOR
  if (p.address_sensor != nullptr)
    p.address_sensor->publish_state(p.long_address != 0 ? long_hex(p.long_address) : std::string());
#endif
}

Panel *TigoRS485::assign_slot_(uint64_t long_address) {
  for (size_t i = 0; i < this->slots_.size(); i++) {
    Panel *p = this->slots_[i];
    if (p->long_address != 0)
      continue;
    p->long_address = long_address;
    ESP_LOGI(TAG, "%s assigned to panel slot %u", long_hex(long_address).c_str(), unsigned(i + 1));
    this->publish_slot_address_(*p);
    this->save_slots_();
    return p;
  }
  return nullptr;
}

void TigoRS485::reset_slots() {
  ESP_LOGI(TAG, "Resetting panel slot assignments");
  for (auto *p : this->slots_) {
    this->mark_offline_(*p);
    p->long_address = 0;
    p->energy_wh = 0;
    p->history.fill(0);
    p->temperature = p->rssi = NAN;
    p->last_report = 0;
    p->last_node = -1;
    this->publish_slot_address_(*p);
  }
  this->save_slots_();
  // Panels already on the bus take a slot again with their next report
  this->unconfigured_.clear();
  this->unconfigured_dirty_ = true;
}

void TigoRS485::load_slots_() {
  StoredSlots stored{};
  if (this->slots_.empty() || !this->slots_pref_.load(&stored) || stored.count > MAX_SLOTS)
    return;
  size_t n = std::min<size_t>(stored.count, this->slots_.size());
  size_t used = 0;
  for (size_t i = 0; i < n; i++) {
    uint64_t addr = (uint64_t(stored.addr[i][0]) << 32) | stored.addr[i][1];
    // A panel configured by address in YAML does not keep a slot
    bool manual = false;
    for (auto *p : this->panels_)
      if (addr != 0 && p->long_address == addr)
        manual = true;
    if (manual)
      continue;
    this->slots_[i]->long_address = addr;
    if (addr != 0)
      used++;
  }
  ESP_LOGI(TAG, "Loaded %u panel slot assignments from flash", unsigned(used));
}

void TigoRS485::save_slots_() {
  StoredSlots stored{};
  stored.count = std::min<size_t>(this->slots_.size(), MAX_SLOTS);
  for (size_t i = 0; i < stored.count; i++) {
    stored.addr[i][0] = uint32_t(this->slots_[i]->long_address >> 32);
    stored.addr[i][1] = uint32_t(this->slots_[i]->long_address);
  }
  this->slots_pref_.save(&stored);
}

void TigoRS485::set_panel_label(uint64_t long_address, int node_id, const char *label) {
  for (auto *p : this->panels_)
    if (p->long_address == long_address && p->node_id == node_id)
      p->label = label;
}

void TigoRS485::set_slot_label(size_t slot, const char *label) {
  Panel *p = this->slot_(slot);
  if (p != nullptr)
    p->label = label;
}

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
  if (long_address == 0 || this->slots_.empty())
    return nullptr;
  for (auto *p : this->slots_)
    if (p->long_address == long_address)
      return p;
  return this->assign_slot_(long_address);
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
  // Values and energy for the web dashboard
  uint64_t up = this->uptime_ms_();
  if (p->last_report != 0 && !std::isnan(p->power)) {
    uint64_t dt = std::min<uint64_t>(up - p->last_report, 120000);  // gaps longer than 2 min are not integrated
    float wh = p->power * float(dt) / 3600000.0f;
    p->energy_wh += wh;
    this->advance_history_(*p, up / HISTORY_BUCKET_MS);
    p->history[p->history_bucket % HISTORY_BUCKETS] += wh;
  } else {
    this->advance_history_(*p, up / HISTORY_BUCKET_MS);
  }
  p->last_report = up;
  p->last_node = r.node_id;
  p->power = power;
  p->voltage_in = r.voltage_in;
  p->voltage_out = r.voltage_out;
  p->current = current;
  p->duty = r.duty_cycle * 100.0f / 255.0f;
  if (r.temperature_valid)
    p->temperature = r.temperature;
  if (r.rssi != 0)
    p->rssi = r.rssi;
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
  p.power = p.voltage_in = p.voltage_out = p.current = p.duty = NAN;
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
  if (!this->slots_.empty()) {
    ESP_LOGCONFIG(TAG, "  Automatic panel slots: %u", (unsigned) this->slots_.size());
    for (size_t i = 0; i < this->slots_.size(); i++)
      ESP_LOGCONFIG(TAG, "    slot %u: %s", unsigned(i + 1),
                    this->slots_[i]->long_address ? long_hex(this->slots_[i]->long_address).c_str() : "free");
  }
  ESP_LOGCONFIG(TAG, "  Nodes known: %u", (unsigned) this->nodes_.size());
  for (const auto &n : this->nodes_)
    ESP_LOGCONFIG(TAG, "    node %04X = %s, hardware ID %s", n.node_id,
                  n.long_address ? long_hex(n.long_address).c_str() : "?",
                  n.hw_id >= 0 ? to_string(n.hw_id).c_str() : "?");
  this->check_uart_settings(38400);
}

#ifdef USE_WEBSERVER
static void json_num(std::string &out, const char *key, float v, int decimals) {
  char buf[40];
  if (std::isnan(v))
    snprintf(buf, sizeof(buf), "\"%s\":null,", key);
  else
    snprintf(buf, sizeof(buf), "\"%s\":%.*f,", key, decimals, v);
  out += buf;
}

static void json_str(std::string &out, const char *key, const std::string &v) {
  out += '"';
  out += key;
  out += "\":\"";
  for (char ch : v) {
    if (ch == '"' || ch == '\\')
      out += '\\';
    if (uint8_t(ch) >= 0x20)
      out += ch;
  }
  out += "\",";
}

static void end_object(std::string &out) {
  if (!out.empty() && out.back() == ',')
    out.back() = '}';
  else
    out += '}';
}

std::string TigoRS485::live_json() {
  LockGuard guard(this->lock_);
  uint64_t up = this->uptime_ms_();
  std::string out;
  out.reserve(256 + 320 * (this->panels_.size() + this->slots_.size()));
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"uptime\":%" PRIu32 ",\"timeout\":%" PRIu32 ",\"slots\":%u,",
           uint32_t(up / 1000), this->offline_timeout_ / 1000, unsigned(this->slots_.size()));
  out += buf;
  json_str(out, "device", std::string(App.get_friendly_name().empty() ? App.get_name().c_str()
                                                                      : App.get_friendly_name().c_str()));
  out += "\"panels\":[";
  bool first = true;
  this->for_each_panel_([&](Panel &p) {
    bool is_slot = std::find(this->slots_.begin(), this->slots_.end(), &p) != this->slots_.end();
    if (!first)
      out += ',';
    first = false;
    out += '{';
    json_str(out, "name", p.label);
    json_str(out, "addr", p.long_address ? long_hex(p.long_address) : std::string());
    // name of the power entity: lets the add-on find its Home Assistant statistics
    if (p.sensors[PANEL_POWER] != nullptr)
      json_str(out, "ent", std::string(p.sensors[PANEL_POWER]->get_name().c_str()));
    int node = p.last_node >= 0 ? p.last_node : p.node_id;
    NodeInfo *n = node >= 0 ? this->node_(node, false) : nullptr;
    snprintf(buf, sizeof(buf), "\"node\":%d,\"hw\":%d,\"slot\":%d,\"online\":%d,", node,
             n != nullptr ? n->hw_id : -1, is_slot ? 1 : 0, p.online ? 1 : 0);
    out += buf;
    snprintf(buf, sizeof(buf), "\"age\":%ld,",
             p.last_report ? long((up - p.last_report) / 1000) : -1L);
    out += buf;
    json_num(out, "p", p.power, 1);
    json_num(out, "vin", p.voltage_in, 2);
    json_num(out, "vout", p.voltage_out, 1);
    json_num(out, "i", p.current, 3);
    json_num(out, "t", p.temperature, 1);
    json_num(out, "rssi", p.rssi, 0);
    json_num(out, "duty", p.duty, 0);
    json_num(out, "e", p.energy_wh, 1);
    this->advance_history_(p, up / HISTORY_BUCKET_MS);
    float e24 = 0;
    for (float v : p.history)
      e24 += v;
    json_num(out, "e24", e24, 1);
    end_object(out);
  });
  out += "],\"unconfigured\":[";
  for (size_t i = 0; i < this->unconfigured_.size(); i++) {
    if (i)
      out += ',';
    out += '"' + this->unconfigured_[i] + '"';
  }
  out += "]}";
  return out;
}

std::string TigoRS485::history_json() {
  LockGuard guard(this->lock_);
  uint64_t up = this->uptime_ms_();
  uint64_t bucket = up / HISTORY_BUCKET_MS;
  std::string out;
  out.reserve(128 + 600 * (this->panels_.size() + this->slots_.size()));
  char buf[64];
  // "into": milliseconds already elapsed in the newest bucket, to scale it on the chart
  snprintf(buf, sizeof(buf), "{\"bucket\":%" PRIu32 ",\"valid\":%u,\"into\":%" PRIu32 ",\"panels\":[",
           HISTORY_BUCKET_MS / 1000, unsigned(std::min<uint64_t>(bucket + 1, HISTORY_BUCKETS)),
           uint32_t(up % HISTORY_BUCKET_MS));
  out += buf;
  bool first = true;
  this->for_each_panel_([&](Panel &p) {
    this->advance_history_(p, bucket);
    if (!first)
      out += ',';
    first = false;
    out += '[';
    for (size_t k = 0; k < HISTORY_BUCKETS; k++) {
      // oldest first
      float v = p.history[(bucket + 1 + k) % HISTORY_BUCKETS];
      snprintf(buf, sizeof(buf), k ? ",%.1f" : "%.1f", v);
      out += buf;
    }
    out += ']';
  });
  out += "]}";
  return out;
}

bool DashboardHandler::canHandle(AsyncWebServerRequest *request) const {
#ifdef USE_ESP32
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(url_buf);
#else
  const auto &url = request->url();
#endif
  if (url == "/tigo/live" || url == "/tigo/history")
    return request->method() == HTTP_GET;
  if (url == "/tigo/reset")
    return request->method() == HTTP_POST;
  // "/" is the dashboard, unless the standard ESPHome page is asked for with "/?esphome"
  return url == "/" && request->method() == HTTP_GET &&
         !const_cast<AsyncWebServerRequest *>(request)->hasArg("esphome");
}

void DashboardHandler::handleRequest(AsyncWebServerRequest *request) {
#ifdef USE_ESP32
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(url_buf);
#else
  const auto &url = request->url();
#endif
  if (url == "/tigo/live" || url == "/tigo/history") {
    std::string body = url == "/tigo/live" ? this->parent_->live_json() : this->parent_->history_json();
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", body);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
    return;
  }
  if (url == "/tigo/reset") {
    this->parent_->request_reset();
    request->send(200, "application/json", "{\"ok\":true}");
    return;
  }
  AsyncWebServerResponse *response =
      request->beginResponse(200, "text/html", this->parent_->dashboard_html(), this->parent_->dashboard_size());
  response->addHeader("Content-Encoding", "gzip");
  request->send(response);
}
#endif

}  // namespace tigo_rs485
}  // namespace esphome
