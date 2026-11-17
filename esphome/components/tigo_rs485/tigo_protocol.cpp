#include "tigo_protocol.h"

#include <cstring>

namespace tigo_proto {

namespace {

constexpr uint8_t ESC = 0x7E;
constexpr uint8_t ESC_START = 0x07;
constexpr uint8_t ESC_END = 0x08;

constexpr uint16_t GW_RECEIVE_RESPONSE = 0x0149;
constexpr uint16_t GW_COMMAND_RESPONSE = 0x0B10;

constexpr uint8_t PV_STRING_RESPONSE = 0x07;
constexpr uint8_t PV_NODE_TABLE_RESPONSE = 0x27;
constexpr uint8_t PV_POWER_REPORT = 0x31;

inline uint16_t be16(const uint8_t *p) { return (uint16_t(p[0]) << 8) | p[1]; }

int hex_value(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

}  // namespace

float current_scale(int hw_id) {
  // CCA meshdcd 4.0.8: CNodeVersion::DetermineHwScaling + TS4Utils::IsUnitHigherRated
  if (hw_id < 0)
    return 0.005f;
  if (hw_id > 0x5F)
    return 0.025f;  // "higher rated" units: raw * 5 * 0.005
  if (hw_id > 0x23)
    return 0.005f;
  return 0.0025f;
}

uint16_t Decoder::crc16(const uint8_t *data, size_t len) {
  // CRC-16 with the reflected CCITT polynomial and a non-standard initial value 0x8408
  static uint16_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint16_t i = 0; i < 256; i++) {
      uint16_t crc = i;
      for (int j = 0; j < 8; j++)
        crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
      table[i] = crc;
    }
    ready = true;
  }
  uint16_t crc = 0x8408;
  for (size_t i = 0; i < len; i++)
    crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFF];
  return crc;
}

void Decoder::feed(uint8_t byte) {
  if (escape_) {
    escape_ = false;
    switch (byte) {
      case ESC_START:  // start of frame (also resynchronizes a broken one)
        in_frame_ = true;
        overflow_ = false;
        len_ = 0;
        return;
      case ESC_END:
        if (in_frame_) {
          if (overflow_)
            frames_too_long++;
          else
            process_frame(buf_, len_);
        }
        in_frame_ = false;
        return;
      case 0x00: byte = 0x7E; break;
      case 0x01: byte = 0x24; break;
      case 0x02: byte = 0x23; break;
      case 0x03: byte = 0x25; break;
      case 0x04: byte = 0xA4; break;
      case 0x05: byte = 0xA3; break;
      case 0x06: byte = 0xA5; break;
      default:
        in_frame_ = false;  // invalid escape: drop the frame
        return;
    }
  } else if (byte == ESC) {
    escape_ = true;
    return;
  }
  if (!in_frame_)
    return;
  if (len_ >= MAX_FRAME) {
    overflow_ = true;
    return;
  }
  buf_[len_++] = byte;
}

void Decoder::process_frame(const uint8_t *frame, size_t len) {
  // address(2) type(2) payload(...) crc(2, little endian)
  if (len < 6)
    return;
  uint16_t crc = uint16_t(frame[len - 2]) | (uint16_t(frame[len - 1]) << 8);
  if (crc16(frame, len - 2) != crc) {
    frames_bad_crc++;
    return;
  }
  frames_ok++;
  uint16_t type = be16(frame + 2);
  const uint8_t *payload = frame + 4;
  size_t payload_len = len - 6;
  if (type == GW_RECEIVE_RESPONSE)
    process_receive_response(payload, payload_len);
  else if (type == GW_COMMAND_RESPONSE)
    process_command_response(payload, payload_len);
}

void Decoder::process_receive_response(const uint8_t *p, size_t len) {
  if (len < 2)
    return;
  uint16_t status = be16(p);
  if ((status & 0x00E0) != 0x00E0)
    return;
  // Optional header fields are present when their status bit is clear
  size_t header = 2;
  if (!(status & 0x01)) header += 1;  // rx buffers used
  if (!(status & 0x02)) header += 1;  // tx buffers free
  if (!(status & 0x04)) header += 2;  // unknown A
  if (!(status & 0x08)) header += 2;  // unknown B
  header += (status & 0x10) ? 1 : 2;  // packet number (low byte only, or both)
  header += 2;                        // slot counter
  if (len < header)
    return;
  p += header;
  len -= header;
  // PV packets: type(1) node_id(2) short_address(2) dsn(1) length(1) data(length)
  while (len >= 7) {
    size_t data_len = p[6];
    if (len < 7 + data_len)
      break;
    process_pv_packet(p[0], be16(p + 1), p + 7, data_len);
    p += 7 + data_len;
    len -= 7 + data_len;
  }
}

void Decoder::process_command_response(const uint8_t *p, size_t len) {
  // unknown(1) tx_buffers_free(1) unknown(1) pv_packet_type(1) sequence(1) data
  if (len < 5)
    return;
  uint8_t pv_type = p[3];
  const uint8_t *d = p + 5;
  size_t n = len - 5;
  if (pv_type == PV_NODE_TABLE_RESPONSE && n >= 4) {
    // start node id(2) entry count(2), then entries: long address(8) node id(2)
    size_t count = be16(d + 2);
    d += 4;
    n -= 4;
    for (size_t i = 0; i < count && n >= 10; i++, d += 10, n -= 10) {
      uint64_t long_address = 0;
      for (int b = 0; b < 8; b++)
        long_address = (long_address << 8) | d[b];
      if (on_node)
        on_node(be16(d + 8), long_address);
    }
  }
}

void Decoder::process_pv_packet(uint8_t type, uint16_t node_id, const uint8_t *d, size_t len) {
  if (type == PV_STRING_RESPONSE) {
    process_string(node_id, d, len);
    return;
  }
  if (type != PV_POWER_REPORT || len < 12)
    return;  // the CCA requires at least 12 bytes
  PowerReport r;
  r.node_id = node_id;
  r.voltage_in = float((uint16_t(d[0]) << 4) | (d[1] >> 4)) * 0.05f;
  r.voltage_out = float((uint16_t(d[1] & 0x0F) << 8) | d[2]) * 0.1f;
  r.duty_cycle = d[3];
  r.current_raw = (uint16_t(d[4]) << 4) | (d[5] >> 4);
  int t = (int(d[5] & 0x0F) << 8) | d[6];
  if (t & 0x800)
    t -= 0x1000;  // 12-bit two's complement
  r.temperature = float(t) * 0.1f;
  r.jstatus = d[7];
  r.status = be16(d + 8);
  r.temperature_valid = (r.status & 0x20) != 0;
  r.slot_counter = be16(d + 10);
  r.rssi = len > 12 ? d[12] : 0;
  power_reports++;
  if (on_power)
    on_power(r);
}

void Decoder::process_string(uint16_t node_id, const uint8_t *d, size_t len) {
  // Answer to "^00Version": "Mnode Version K8.0120 (2D)"; hex in parentheses = hardware ID
  char text[128];
  size_t n = 0;
  for (size_t i = 0; i < len && n < sizeof(text) - 1; i++)
    if (d[i] >= 0x20 && d[i] < 0x7F)
      text[n++] = char(d[i]);
  text[n] = '\0';
  const char *v = strstr(text, "Version");
  if (v == nullptr)
    return;
  const char *open = strchr(v, '(');
  if (open == nullptr)
    return;
  int value = 0, digits = 0;
  for (const char *c = open + 1; *c && *c != ')'; c++) {
    int h = hex_value(*c);
    if (h < 0 || ++digits > 2)
      return;
    value = value * 16 + h;
  }
  if (digits == 0 || value == 0)
    return;
  if (on_hw_id)
    on_hw_id(node_id, uint8_t(value));
}

}  // namespace tigo_proto
