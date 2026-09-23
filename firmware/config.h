#pragma once

#include <Arduino.h>

namespace cfg {
constexpr uint32_t kSampleRate = 83333;
constexpr gpio_num_t kAdcGpio = GPIO_NUM_1;       // XIAO D0, ADC1_CH0
constexpr adc_channel_t kAdcChannel = ADC_CHANNEL_0;
constexpr int kSdSck = 7;
constexpr int kSdMiso = 8;
constexpr int kSdMosi = 9;
constexpr int kSdCs = 21;
constexpr uint32_t kSdFrequency = 10000000;       // conservative; measured throughput margin
constexpr int kButton = 2;                         // XIAO D1, active low
constexpr int kLedBus = 3;                         // external LED + resistor, XIAO D2
constexpr int kLedRec = 4;                         // external LED + resistor, XIAO D3
constexpr int kLedError = 5;                       // external LED + resistor, XIAO D4
constexpr int kReservedSync = 6;                    // XIAO D5, reserved; do not drive
constexpr size_t kSamplesPerBlock = 1024;
constexpr size_t kBlockCount = 512;                // 1 MiB, about 6.3 s at 83.3 kS/s
constexpr size_t kDmaFrameBytes = 4096;
constexpr size_t kDmaPoolBytes = 32768;
constexpr uint64_t kSegmentMaxBytes = 1024ULL * 1024ULL * 1024ULL; // FAT32 safety
constexpr char kApPassword[] = "knxlogger";
constexpr char kFormatVersion[] = "KXLR/1";
constexpr char kSdRoot[] = "/KNXLOGGER";
}

