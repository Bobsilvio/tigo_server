// Host test for the protocol decoder.
//   c++ -std=c++17 -I components/tigo_rs485 tests/host_test.cpp \
//       components/tigo_rs485/tigo_protocol.cpp -o host_test
//   ./host_test [capture.bin]
// With a capture file it prints one CSV line per power report (compared against an
// independent reference decoder by tests/compare_capture.py).
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "tigo_protocol.h"

using namespace tigo_proto;

static std::vector<uint8_t> encode_frame(std::vector<uint8_t> body) {
  uint16_t crc = Decoder::crc16(body.data(), body.size());
  body.push_back(crc & 0xFF);
  body.push_back(crc >> 8);
  std::vector<uint8_t> out = {0x00, 0xFF, 0xFF, 0x7E, 0x07};  // noise before the frame
  for (uint8_t b : body) {
    switch (b) {
      case 0x7E: out.insert(out.end(), {0x7E, 0x00}); break;
      case 0x24: out.insert(out.end(), {0x7E, 0x01}); break;
      case 0x23: out.insert(out.end(), {0x7E, 0x02}); break;
      case 0x25: out.insert(out.end(), {0x7E, 0x03}); break;
      case 0xA4: out.insert(out.end(), {0x7E, 0x04}); break;
      case 0xA3: out.insert(out.end(), {0x7E, 0x05}); break;
      case 0xA5: out.insert(out.end(), {0x7E, 0x06}); break;
      default: out.push_back(b);
    }
  }
  out.insert(out.end(), {0x7E, 0x08});
  return out;
}

// Receive response (0x0149) carrying one PV packet
static std::vector<uint8_t> receive_frame(uint8_t pv_type, uint16_t node, std::vector<uint8_t> data) {
  std::vector<uint8_t> b = {0x92, 0x01, 0x01, 0x49, 0x00, 0xFF, 0x7C, 0xDB, 0xC2};  // status 00FF: pkt lo + slot
  b.insert(b.end(), {pv_type, uint8_t(node >> 8), uint8_t(node), 0x00, 0x00, 0x00, uint8_t(data.size())});
  b.insert(b.end(), data.begin(), data.end());
  return encode_frame(b);
}

static void synthetic_tests() {
  Decoder dec;
  std::vector<std::pair<uint16_t, uint64_t>> nodes;
  std::vector<std::pair<uint16_t, uint8_t>> hw;
  std::vector<PowerReport> reports;
  dec.on_node = [&](uint16_t n, uint64_t l) { nodes.push_back({n, l}); };
  dec.on_hw_id = [&](uint16_t n, uint8_t h) { hw.push_back({n, h}); };
  dec.on_power = [&](const PowerReport &r) { reports.push_back(r); };

  // Node table response (command response 0x0B10, PV type 0x27): start 0x0000, 2 entries
  std::vector<uint8_t> nt = {0x92, 0x01, 0x0B, 0x10, 0x00, 0x0D, 0x00, 0x27, 0x6C, 0x00, 0x00, 0x00, 0x02,
                             0x04, 0xC0, 0x5B, 0x40, 0x00, 0xA2, 0x34, 0x6F, 0x00, 0x02,
                             0x04, 0xC0, 0x5B, 0x40, 0x00, 0xA2, 0x34, 0x7E, 0x00, 0x1A};
  auto f = encode_frame(nt);
  dec.feed(f.data(), f.size());
  assert(nodes.size() == 2);
  assert(nodes[0].first == 0x0002 && nodes[0].second == 0x04C05B4000A2346FULL);
  assert(nodes[1].first == 0x001A && nodes[1].second == 0x04C05B4000A2347EULL);  // contains an escaped 0x7E

  // Version string from node 0x001A
  const char *v = "Mnode Version K8.0120 (2D)\r";
  f = receive_frame(0x07, 0x001A, std::vector<uint8_t>(v, v + strlen(v)));
  dec.feed(f.data(), f.size());
  assert(hw.size() == 1 && hw[0].first == 0x001A && hw[0].second == 0x2D);

  // Power report: Vin 34.7 V, Vout 34.4 V, duty FF, Iraw 0x032, temp 34.4 C, status 0x006E (bit 0x20 set)
  f = receive_frame(0x31, 0x001A, {0x2B, 0x61, 0x58, 0xFF, 0x03, 0x21, 0x58, 0x81, 0x00, 0x6E, 0x8F, 0xA0, 0x7E});
  dec.feed(f.data(), f.size());
  assert(reports.size() == 1);
  const PowerReport &r = reports[0];
  assert(r.node_id == 0x001A && std::fabs(r.voltage_in - 34.7f) < 1e-3 && std::fabs(r.voltage_out - 34.4f) < 1e-3);
  assert(r.duty_cycle == 0xFF && r.current_raw == 0x032 && r.temperature_valid);
  assert(std::fabs(r.temperature - 34.4f) < 1e-3 && r.rssi == 0x7E && r.status == 0x006E);

  // Status bit 0x20 clear: the 12-bit field is not a temperature
  f = receive_frame(0x31, 0x001A, {0x2B, 0x61, 0x58, 0xFF, 0x03, 0x21, 0x58, 0x81, 0x00, 0x4E, 0x8F, 0xA0, 0x7E});
  dec.feed(f.data(), f.size());
  assert(reports.size() == 2 && !reports[1].temperature_valid);

  // Negative temperature (two's complement), 12-byte report without RSSI
  f = receive_frame(0x31, 0x001A, {0x2B, 0x61, 0x58, 0xFF, 0x03, 0x2F, 0xFF, 0x81, 0x00, 0x6E, 0x8F, 0xA0});
  dec.feed(f.data(), f.size());
  assert(reports.size() == 3 && std::fabs(reports[2].temperature + 0.1f) < 1e-3 && reports[2].rssi == 0);

  // Corrupted CRC is rejected
  f = receive_frame(0x31, 0x001A, {0x2B, 0x61, 0x58, 0xFF, 0x03, 0x21, 0x58, 0x81, 0x00, 0x6E, 0x8F, 0xA0, 0x7E});
  f[f.size() - 4] ^= 0x01;
  dec.feed(f.data(), f.size());
  assert(reports.size() == 3 && dec.frames_bad_crc == 1);

  assert(current_scale(-1) == 0.005f && current_scale(0x10) == 0.0025f && current_scale(0x23) == 0.0025f);
  assert(current_scale(0x24) == 0.005f && current_scale(0x2D) == 0.005f && current_scale(0x5F) == 0.005f);
  assert(current_scale(0x60) == 0.025f);
  printf("synthetic tests: OK\n");
}

int main(int argc, char **argv) {
  synthetic_tests();
  if (argc < 2)
    return 0;
  FILE *fp = fopen(argv[1], "rb");
  if (!fp) {
    perror(argv[1]);
    return 1;
  }
  Decoder dec;
  dec.on_power = [](const PowerReport &r) {
    printf("%04X,%.2f,%.1f,%u,%u,%s,%u,%04X\n", r.node_id, r.voltage_in, r.voltage_out, r.duty_cycle,
           r.current_raw, r.temperature_valid ? std::to_string(r.temperature).substr(0, 5).c_str() : "-",
           r.rssi, r.slot_counter);
  };
  uint8_t buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
    dec.feed(buf, n);
  fclose(fp);
  fprintf(stderr, "frames ok=%u bad_crc=%u too_long=%u power=%u\n", dec.frames_ok, dec.frames_bad_crc,
          dec.frames_too_long, dec.power_reports);
  return 0;
}
