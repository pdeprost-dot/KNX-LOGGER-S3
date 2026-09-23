#pragma once
#include <stdint.h>

constexpr uint32_t fourcc(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) |
         (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
}

enum class RecordType : uint32_t {
  Data = fourcc('D','A','T','A'),
  Gap  = fourcc('G','A','P',' '),
  Time = fourcc('T','I','M','E'),
  Stats = fourcc('S','T','A','T'),
  End = fourcc('E','N','D',' '),
};

#pragma pack(push, 1)
struct FileHeader {
  char magic[8];                    // "KNXRAW1\0"
  uint16_t header_bytes;
  uint16_t format_major;
  uint16_t format_minor;
  uint16_t flags;
  char recorder_id[24];
  char session_id[40];
  uint32_t requested_sample_rate_hz;
  uint32_t adc_unit;
  uint32_t adc_channel;
  uint32_t adc_gpio;
  uint32_t adc_bits;
  uint32_t adc_attenuation_db_x10;
  int64_t start_epoch_ms;           // 0 when unavailable
  uint64_t start_monotonic_us;
  char time_source[16];             // "browser" or "none"
  char firmware_version[16];
  uint8_t reserved[356];
  uint32_t header_crc32;            // CRC over first 508 bytes
};
static_assert(sizeof(FileHeader) == 512, "FileHeader must be 512 bytes");

struct RecordHeader {
  uint32_t type;
  uint16_t version;
  uint16_t header_bytes;
  uint32_t payload_bytes;
  uint32_t sequence;
  uint64_t first_sample_index;
  uint64_t monotonic_us;
  uint32_t payload_crc32;
  uint32_t reserved;
};
static_assert(sizeof(RecordHeader) == 40, "RecordHeader must be 40 bytes");

struct GapPayload {
  uint64_t lost_samples;
  uint32_t reason;                  // 1=RAM pool exhausted, 2=DMA overflow
  uint32_t reserved;
};

struct TimePayload {
  int64_t epoch_ms;
  uint64_t sample_index;
  uint64_t monotonic_us;
  char source[16];
};

struct StatsPayload {
  uint64_t acquired_samples;
  uint64_t written_samples;
  uint64_t lost_samples;
  uint32_t dma_overflows;
  uint32_t buffer_overflows;
  uint32_t sd_errors;
  uint32_t observed_rate_millihz;
  uint32_t min_free_heap;
  uint32_t min_free_psram;
  uint64_t file_bytes;
};
#pragma pack(pop)

