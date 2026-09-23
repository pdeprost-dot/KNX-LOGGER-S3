#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <SD.h>
#include <esp_adc/adc_continuous.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <atomic>

#include "config.h"
#include "raw_format.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
constexpr char WIFI_SSID[] = "";
constexpr char WIFI_PASSWORD[] = "";
#endif

struct SampleBlock {
  uint64_t firstIndex;
  uint32_t count;
  uint16_t samples[cfg::kSamplesPerBlock];
};

struct Runtime {
  std::atomic<bool> recording{false};
  std::atomic<bool> stopping{false};
  std::atomic<bool> stopRequested{false};
  std::atomic<uint64_t> acquired{0}, written{0}, lost{0};
  std::atomic<uint32_t> dmaOverflows{0}, bufferOverflows{0}, sdErrors{0};
  std::atomic<uint64_t> lastActivityUs{0};
  std::atomic<uint32_t> minHeap{UINT32_MAX}, minPsram{UINT32_MAX};
  uint64_t startUs = 0;
  int64_t browserEpochMs = 0;
  int32_t timezoneOffsetMinutes = 0;
  uint64_t browserSyncUs = 0;
  String recorderId, sessionId, sessionDir, filename;
  std::atomic<uint64_t> sessionBytes{0};
  uint32_t segmentIndex = 0;
  uint64_t segmentBytes = 0;
  bool sdReady = false;
  uint64_t sdTotal = 0, sdUsed = 0;
} rt;

WebServer server(80);
SPIClass sdSpi(FSPI);
File rawFile;
File eventsFile;
adc_continuous_handle_t adcHandle = nullptr;
QueueHandle_t freeQueue = nullptr, readyQueue = nullptr;
SampleBlock *blocks[cfg::kBlockCount]{};
alignas(4) uint8_t dma[cfg::kDmaFrameBytes];  // internal RAM, never task stack/PSRAM
TaskHandle_t adcTaskHandle = nullptr, writerTaskHandle = nullptr;
portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t recordSequence = 0;
volatile uint32_t dmaOverflowIsr = 0;

uint32_t crc32(const void *data, size_t length) {
  const uint8_t *p = static_cast<const uint8_t *>(data);
  uint32_t crc = 0xFFFFFFFFu;
  while (length--) {
    crc ^= *p++;
    for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xEDB88320u & -(int32_t)(crc & 1));
  }
  return ~crc;
}

void atomicMin(std::atomic<uint32_t> &target, uint32_t value) {
  uint32_t old = target.load();
  while (value < old && !target.compare_exchange_weak(old, value)) {}
}

bool writeAll(const void *data, size_t bytes) {
  if (!rawFile || rawFile.write(static_cast<const uint8_t *>(data), bytes) != bytes) {
    rt.sdErrors++;
    rt.stopRequested = true;
    digitalWrite(cfg::kLedError, HIGH);
    return false;
  }
  rt.sessionBytes += bytes;
  rt.segmentBytes += bytes;
  return true;
}

bool writeRecord(RecordType type, uint64_t first, const void *payload, uint32_t bytes) {
  RecordHeader h{};
  h.type = static_cast<uint32_t>(type);
  h.version = 1;
  h.header_bytes = sizeof(h);
  h.payload_bytes = bytes;
  h.sequence = recordSequence++;
  h.first_sample_index = first;
  h.monotonic_us = esp_timer_get_time();
  h.payload_crc32 = bytes ? crc32(payload, bytes) : 0;
  return writeAll(&h, sizeof(h)) && (!bytes || writeAll(payload, bytes));
}

bool IRAM_ATTR onConversionDone(adc_continuous_handle_t, const adc_continuous_evt_data_t *, void *) {
  BaseType_t mustYield = pdFALSE;
  if (adcTaskHandle) vTaskNotifyGiveFromISR(adcTaskHandle, &mustYield);
  return mustYield == pdTRUE;
}

bool IRAM_ATTR onPoolOverflow(adc_continuous_handle_t, const adc_continuous_evt_data_t *, void *) {
  ++dmaOverflowIsr;
  return false;
}

bool initAdc() {
  adc_continuous_handle_cfg_t handleCfg{};
  handleCfg.max_store_buf_size = cfg::kDmaPoolBytes;
  handleCfg.conv_frame_size = cfg::kDmaFrameBytes;
  if (adc_continuous_new_handle(&handleCfg, &adcHandle) != ESP_OK) return false;

  adc_digi_pattern_config_t pattern{};
  pattern.atten = ADC_ATTEN_DB_12;
  pattern.channel = cfg::kAdcChannel;
  pattern.unit = ADC_UNIT_1;
  pattern.bit_width = ADC_BITWIDTH_12;
  adc_continuous_config_t adcCfg{};
  adcCfg.sample_freq_hz = cfg::kSampleRate;
  adcCfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  adcCfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
  adcCfg.pattern_num = 1;
  adcCfg.adc_pattern = &pattern;
  if (adc_continuous_config(adcHandle, &adcCfg) != ESP_OK) return false;
  adc_continuous_evt_cbs_t callbacks{};
  callbacks.on_conv_done = onConversionDone;
  callbacks.on_pool_ovf = onPoolOverflow;
  return adc_continuous_register_event_callbacks(adcHandle, &callbacks, nullptr) == ESP_OK;
}

String makeRecorderId() {
  uint64_t mac = ESP.getEfuseMac();
  char id[24];
  snprintf(id, sizeof(id), "XIAO-S3-%04X%08X", uint16_t(mac >> 32), uint32_t(mac));
  return String(id);
}

bool createSessionDirectory() {
  for (uint8_t attempt = 0; attempt < 16; ++attempt) {
    char id[32];
    snprintf(id, sizeof(id), "S-%08X%08X", esp_random(), esp_random());
    rt.sessionId = id;
    rt.sessionDir = String(cfg::kSdRoot) + "/" + rt.sessionId;
    if (!SD.exists(rt.sessionDir) && SD.mkdir(rt.sessionDir)) { rt.filename = ""; return true; }
  }
  rt.sdErrors++;
  return false;
}

int64_t currentEpochMs() {
  return rt.browserEpochMs ? rt.browserEpochMs + int64_t((esp_timer_get_time() - rt.browserSyncUs) / 1000ULL) : 0;
}

void logEvent(const char *type) {
  if (!eventsFile) return;
  eventsFile.printf("{\"event\":\"%s\",\"sample_counter\":%llu,\"monotonic_us\":%llu,\"civil_epoch_ms\":%lld}\n",
                    type, rt.acquired.load(), uint64_t(esp_timer_get_time()), currentEpochMs());
  eventsFile.flush();
}

bool writeSessionStart() {
  File f = SD.open(rt.sessionDir + "/session.json", FILE_WRITE);
  if (!f) { rt.sdErrors++; return false; }
  f.printf("{\n\"schema_version\":1,\n\"session_id\":\"%s\",\n\"recorder_id\":\"%s\",\n"
           "\"name\":\"\",\n\"description\":\"\",\n\"campaign_id\":\"\",\n\"measurement_point\":\"\",\n\"operator_note\":\"\",\n"
           "\"start_time_utc_ms\":%lld,\n\"start_time_local\":null,\n\"timezone_offset_minutes\":%ld,\n\"time_source\":\"%s\",\n"
           "\"monotonic_reference_us\":%llu,\n\"sample_counter_initial\":0,\n"
           "\"acquisition\":{\"sample_rate_requested_hz\":%u,\"sample_rate_measured_hz\":null,\"adc_gpio\":1,\"adc_unit\":1,\"adc_channel\":0,\"sample_format\":\"uint16_le_adc12_raw\",\"resolution_bits\":12,\"attenuation_db\":12,\"counter_bits\":64},\n"
           "\"afe\":{\"type\":\"experimental_resistive_divider\",\"version\":null,\"calibration_id\":null,\"calibration\":\"uncalibrated\",\"high_ohm\":390000,\"low_ohm\":27000},\n"
           "\"hardware\":{\"model\":\"Seeed Studio XIAO ESP32-S3 Sense\",\"revision\":null},\n"
           "\"software\":{\"firmware_version\":\"0.1.0\",\"git_commit\":\"unknown\",\"raw_format\":\"KXLR/1\"},\n"
           "\"storage\":{\"filesystem\":\"FAT32\",\"segmentation\":\"ordered raw-NNNN.bin; global sample counter continuous\",\"segment_max_bytes\":%llu}\n}\n",
           rt.sessionId.c_str(), rt.recorderId.c_str(), currentEpochMs(), long(rt.timezoneOffsetMinutes),
           rt.browserEpochMs ? "browser" : "none", rt.startUs, cfg::kSampleRate, cfg::kSegmentMaxBytes);
  f.close();
  eventsFile = SD.open(rt.sessionDir + "/events.jsonl", FILE_WRITE);
  if (!eventsFile) { rt.sdErrors++; return false; }
  logEvent("START");
  return true;
}

void writeSessionEnd(uint64_t durationUs) {
  File f = SD.open(rt.sessionDir + "/session-end.json", FILE_WRITE);
  if (!f) { rt.sdErrors++; return; }
  f.printf("{\"schema_version\":1,\"session_id\":\"%s\",\"status\":\"CLOSED\",\"end_time_utc_ms\":%lld,\"duration_us\":%llu,\"samples_acquired\":%llu,\"samples_written\":%llu,\"samples_lost\":%llu,\"dma_overflows\":%u,\"buffer_overflows\":%u,\"sd_errors\":%u,\"raw_segments\":%u,\"raw_bytes\":%llu}\n",
           rt.sessionId.c_str(), currentEpochMs(), durationUs, rt.acquired.load(), rt.written.load(), rt.lost.load(),
           rt.dmaOverflows.load(), rt.bufferOverflows.load(), rt.sdErrors.load(), rt.segmentIndex, rt.sessionBytes.load());
  f.close();
}

void ensureDeviceMetadata() {
  if (!rt.sdReady) return;
  SD.mkdir(cfg::kSdRoot);
  String path = String(cfg::kSdRoot) + "/device.json";
  if (SD.exists(path)) return;
  File f = SD.open(path, FILE_WRITE);
  if (f) { f.printf("{\"schema_version\":1,\"recorder_id\":\"%s\",\"hardware\":\"Seeed Studio XIAO ESP32-S3 Sense\",\"adc_gpio\":1,\"sd_pins\":{\"cs\":21,\"sck\":7,\"miso\":8,\"mosi\":9}}\n", rt.recorderId.c_str()); f.close(); }
}
bool writeHeader() {
  FileHeader h{};
  memcpy(h.magic, "KNXRAW1", 7);
  h.header_bytes = sizeof(h);
  h.format_major = 1;
  h.requested_sample_rate_hz = cfg::kSampleRate;
  h.adc_unit = 1; h.adc_channel = cfg::kAdcChannel; h.adc_gpio = cfg::kAdcGpio;
  h.adc_bits = 12; h.adc_attenuation_db_x10 = 120;
  h.start_monotonic_us = rt.startUs;
  strlcpy(h.recorder_id, rt.recorderId.c_str(), sizeof(h.recorder_id));
  strlcpy(h.session_id, rt.sessionId.c_str(), sizeof(h.session_id));
  strlcpy(h.firmware_version, "0.1.0", sizeof(h.firmware_version));
  if (rt.browserEpochMs) {
    h.start_epoch_ms = rt.browserEpochMs + int64_t((rt.startUs - rt.browserSyncUs) / 1000ULL);
    strlcpy(h.time_source, "browser", sizeof(h.time_source));
  } else strlcpy(h.time_source, "none", sizeof(h.time_source));
  h.header_crc32 = crc32(&h, sizeof(h) - sizeof(h.header_crc32));
  return writeAll(&h, sizeof(h));
}

bool openNextSegment() {
  if (rawFile) { logEvent("SD_SEGMENT_CLOSE"); rawFile.flush(); rawFile.close(); }
  char name[24];
  snprintf(name, sizeof(name), "/raw-%04u.bin", ++rt.segmentIndex);
  rt.filename = rt.sessionDir + name;
  rt.segmentBytes = 0;
  rawFile = SD.open(rt.filename, FILE_WRITE);
  if (!rawFile) { rt.sdErrors++; digitalWrite(cfg::kLedError, HIGH); return false; }
  const bool ok = writeHeader();
  if (ok) logEvent("SD_SEGMENT_OPEN");
  return ok;
}

void adcTask(void *) {
  while (true) {
    if (rt.stopRequested) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    if (!rt.recording) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    uint32_t bytes = 0;
    esp_err_t err = adc_continuous_read(adcHandle, dma, sizeof(dma), &bytes, 50);
    if (dmaOverflowIsr != rt.dmaOverflows.load()) rt.dmaOverflows = dmaOverflowIsr;
    if (err != ESP_OK || !bytes) { vTaskDelay(1); continue; }
    const uint32_t n = bytes / SOC_ADC_DIGI_RESULT_BYTES;
    rt.acquired += n;
    SampleBlock *block = nullptr;
    if (xQueueReceive(freeQueue, &block, 0) != pdTRUE) {
      rt.lost += n; rt.bufferOverflows++;
      continue;
    }
    block->firstIndex = rt.acquired.load() - n;
    block->count = 0;
    for (uint32_t i = 0; i < n && i < cfg::kSamplesPerBlock; ++i) {
      const auto *result = reinterpret_cast<const adc_digi_output_data_t *>(dma + i * SOC_ADC_DIGI_RESULT_BYTES);
      block->samples[block->count++] = result->type2.data;
    }
    if (block->count) {
      uint16_t minV = 4095, maxV = 0;
      for (uint32_t i = 0; i < block->count; ++i) { minV = min(minV, block->samples[i]); maxV = max(maxV, block->samples[i]); }
      if (maxV - minV > 20) rt.lastActivityUs = esp_timer_get_time();
      xQueueSend(readyQueue, &block, portMAX_DELAY);
    } else xQueueSend(freeQueue, &block, portMAX_DELAY);
    vTaskDelay(1);  // feed core-0 idle/WDT; DMA pool absorbs this tick
  }
}

void writerTask(void *) {
  uint32_t seenDma = 0, seenBuffer = 0, seenSd = 0;
  while (true) {
    SampleBlock *block = nullptr;
    if (xQueueReceive(readyQueue, &block, pdMS_TO_TICKS(100)) == pdTRUE) {
      const uint32_t payloadBytes = block->count * sizeof(uint16_t);
      if (rawFile && rt.segmentBytes + sizeof(RecordHeader) + payloadBytes > cfg::kSegmentMaxBytes && !openNextSegment()) {
        rt.lost += block->count;
        xQueueSend(freeQueue, &block, portMAX_DELAY);
        continue;
      }
      if (writeRecord(RecordType::Data, block->firstIndex, block->samples, payloadBytes)) rt.written += block->count;
      else { rt.lost += block->count; vTaskDelay(pdMS_TO_TICKS(10)); }
      xQueueSend(freeQueue, &block, portMAX_DELAY);
      vTaskDelay(1);  // let IDLE0 feed the task watchdog
    }
    if (rt.recording) {
      if (rt.dmaOverflows.load() != seenDma) { seenDma = rt.dmaOverflows; logEvent("DMA_OVERFLOW"); }
      if (rt.bufferOverflows.load() != seenBuffer) { seenBuffer = rt.bufferOverflows; logEvent("BUFFER_OVERFLOW"); }
      if (rt.sdErrors.load() != seenSd) { seenSd = rt.sdErrors; digitalWrite(cfg::kLedError, HIGH); }
    }
    if (rt.stopping && uxQueueMessagesWaiting(readyQueue) == 0) {
      if (rawFile) rawFile.flush();
      rt.stopping = false;
    }
  }
}

bool startRecording() {
  if (rt.recording || !rt.sdReady) return false;
  rt.stopRequested = false;
  rt.acquired = rt.written = rt.lost = 0;
  rt.dmaOverflows = rt.bufferOverflows = rt.sdErrors = 0;
  rt.minHeap = ESP.getFreeHeap(); rt.minPsram = ESP.getFreePsram();
  rt.segmentIndex = 0; rt.sessionBytes = 0; recordSequence = 0;
  rt.startUs = esp_timer_get_time();
  if (!createSessionDirectory() || !writeSessionStart() || !openNextSegment()) return false;
  if (adc_continuous_start(adcHandle) != ESP_OK) { rawFile.close(); eventsFile.close(); return false; }
  if (rt.browserEpochMs) {
    TimePayload t{rt.browserEpochMs, 0, rt.browserSyncUs, {}};
    strlcpy(t.source, "browser", sizeof(t.source));
    writeRecord(RecordType::Time, 0, &t, sizeof(t));
  }
  rt.recording = true;
  digitalWrite(cfg::kLedRec, HIGH);
  return true;
}
void stopRecording() {
  if (!rt.recording) return;
  rt.recording = false;
  adc_continuous_stop(adcHandle);
  rt.stopping = true;
  const uint64_t deadline = millis() + 10000;
  while (rt.stopping && millis() < deadline) delay(10);
  const uint64_t elapsed = esp_timer_get_time() - rt.startUs;
  StatsPayload stats{rt.acquired, rt.written, rt.lost, rt.dmaOverflows, rt.bufferOverflows,
                     rt.sdErrors, 0, rt.minHeap, rt.minPsram, rt.sessionBytes};
  if (elapsed) stats.observed_rate_millihz = uint32_t((rt.acquired.load() * 1000000000ULL) / elapsed);
  if (rt.lost) { GapPayload gap{rt.lost, rt.bufferOverflows ? 1u : 2u, 0}; writeRecord(RecordType::Gap, rt.written, &gap, sizeof(gap)); logEvent("ADC_GAP"); }
  writeRecord(RecordType::Stats, rt.acquired, &stats, sizeof(stats));
  writeRecord(RecordType::End, rt.acquired, nullptr, 0);
  logEvent("STOP");
  if (rawFile) { logEvent("SD_SEGMENT_CLOSE"); rawFile.flush(); rawFile.close(); }
  if (eventsFile) eventsFile.close();
  writeSessionEnd(elapsed);
  rt.sdUsed = SD.usedBytes();
  digitalWrite(cfg::kLedRec, LOW);
  rt.stopRequested = false;
}
String jsonStatus() {
  const uint64_t now = esp_timer_get_time();
  const double seconds = rt.recording ? (now - rt.startUs) / 1e6 : 0;
  const double rate = seconds > 0 ? rt.acquired.load() / seconds : 0;
  const uint64_t fileBytes = rt.sessionBytes.load();
  String s = "{";
  s += "\"recorder_id\":\"" + rt.recorderId + "\",\"recording\":" + String(rt.recording ? "true" : "false");
  s += ",\"duration_s\":" + String(seconds, 1) + ",\"requested_hz\":" + String(cfg::kSampleRate);
  s += ",\"observed_hz\":" + String(rate, 1) + ",\"samples\":" + String(rt.acquired.load());
  s += ",\"written\":" + String(rt.written.load()) + ",\"lost\":" + String(rt.lost.load());
  s += ",\"dma_overflow\":" + String(rt.dmaOverflows.load()) + ",\"buffer_overflow\":" + String(rt.bufferOverflows.load());
  s += ",\"sd_ready\":" + String(rt.sdReady ? "true" : "false") + ",\"sd_errors\":" + String(rt.sdErrors.load());
  s += ",\"sd_free\":" + String(rt.sdTotal > rt.sdUsed ? rt.sdTotal - rt.sdUsed : 0) + ",\"file\":\"" + rt.filename + "\"";
  s += ",\"file_bytes\":" + String(fileBytes) + ",\"write_Bps\":" + String(seconds > 0 ? fileBytes / seconds : 0, 0);
  s += ",\"heap_free\":" + String(ESP.getFreeHeap()) + ",\"heap_min\":" + String(rt.minHeap.load());
  s += ",\"psram_free\":" + String(ESP.getFreePsram()) + ",\"last_bus_ms\":" + String(rt.lastActivityUs ? (now - rt.lastActivityUs.load()) / 1000 : 0);
  s += ",\"epoch_ms\":" + String(rt.browserEpochMs ? rt.browserEpochMs + int64_t((now - rt.browserSyncUs) / 1000) : 0);
  s += ",\"time_source\":\"" + String(rt.browserEpochMs ? "browser" : "none") + "\"}";
  return s;
}

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang=fr><meta name=viewport content="width=device-width,initial-scale=1"><title>KNX LOGGER S3</title><style>body{font:16px system-ui;background:#101820;color:#e9f1f7;margin:auto;max-width:760px;padding:18px}h1{color:#66d9ef}.card{background:#1b2936;padding:16px;border-radius:12px;margin:12px 0}button{font-size:17px;padding:12px 18px;margin:4px;border:0;border-radius:8px}.start{background:#37c978}.stop{background:#ff657a}dl{display:grid;grid-template-columns:1fr 1fr;gap:7px}dd{margin:0;text-align:right}.bad{color:#ff657a}.ok{color:#37c978}</style><h1>KNX LOGGER S3</h1><div class=card><button class=start onclick="cmd('start')">START RECORDING</button><button class=stop onclick="cmd('stop')">STOP RECORDING</button></div><div class=card id=s>Connexion…</div><script>async function sync(){await fetch('/api/time',{method:'POST',headers:{'Content-Type':'text/plain'},body:String(Date.now())+','+String(new Date().getTimezoneOffset())})}async function cmd(x){await fetch('/api/'+x,{method:'POST'});load()}async function load(){try{let x=await(await fetch('/api/status')).json(), rows={Recorder:x.recorder_id,Etat:x.recording?'RECORDING':'STOPPED','Durée':x.duration_s+' s','ADC réel':x.observed_hz+' Hz','Samples acquis':x.samples,'Samples écrits':x.written,'Samples perdus':x.lost,'DMA overflow':x.dma_overflow,'Buffer overflow':x.buffer_overflow,'SD':x.sd_ready?'OK':'ABSENTE','SD libre':(x.sd_free/1073741824).toFixed(2)+' GiB','Fichier':x.file,'Taille':(x.file_bytes/1048576).toFixed(2)+' MiB','Débit écriture':(x.write_Bps/1024).toFixed(1)+' KiB/s','Erreurs SD':x.sd_errors,'Heap libre/min':x.heap_free+' / '+x.heap_min,'PSRAM libre':x.psram_free,'Dernière activité BUS':x.last_bus_ms+' ms','Heure/source':x.epoch_ms+' / '+x.time_source};s.innerHTML='<dl>'+Object.entries(rows).map(([a,b])=>'<dt>'+a+'</dt><dd>'+b+'</dd>').join('')+'</dl>'}catch(e){s.textContent=e}}sync();load();setInterval(load,1000)</script></html>)HTML";

void setupWeb() {
  WiFi.mode(WIFI_AP_STA);
  const String ap = "KNX-LOGGER-" + rt.recorderId.substring(rt.recorderId.length() - 4);
  WiFi.softAP(ap.c_str(), cfg::kApPassword);
  if (strlen(WIFI_SSID)) WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", PAGE); });
  server.on("/api/status", HTTP_GET, [] { server.send(200, "application/json", jsonStatus()); });
  server.on("/api/start", HTTP_POST, [] { bool ok = startRecording(); server.send(ok ? 200 : 409, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}"); });
  server.on("/api/stop", HTTP_POST, [] { stopRecording(); server.send(200, "application/json", "{\"ok\":true}"); });
  server.on("/api/time", HTTP_POST, [] { String body = server.arg("plain"); int comma = body.indexOf(','); rt.browserEpochMs = strtoll(body.c_str(), nullptr, 10); if (comma >= 0) rt.timezoneOffsetMinutes = body.substring(comma + 1).toInt(); rt.browserSyncUs = esp_timer_get_time(); if (rt.recording) logEvent("TIME_SYNC"); server.send(200, "application/json", "{\"ok\":true}"); });
  server.begin();
  Serial.printf("AP: %s  http://%s\n", ap.c_str(), WiFi.softAPIP().toString().c_str());
}

void setup() {
  Serial.begin(115200); delay(1500);
  pinMode(cfg::kButton, INPUT_PULLUP);
  pinMode(cfg::kLedBus, OUTPUT); pinMode(cfg::kLedRec, OUTPUT); pinMode(cfg::kLedError, OUTPUT);
  rt.recorderId = makeRecorderId();
  Serial.printf("KNX LOGGER S3 0.1.0 | %s\n", rt.recorderId.c_str());
  if (!psramFound()) Serial.println("ERROR: PSRAM unavailable");
  freeQueue = xQueueCreate(cfg::kBlockCount, sizeof(SampleBlock *));
  readyQueue = xQueueCreate(cfg::kBlockCount, sizeof(SampleBlock *));
  for (size_t i = 0; i < cfg::kBlockCount; ++i) {
    blocks[i] = static_cast<SampleBlock *>(heap_caps_malloc(sizeof(SampleBlock), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!blocks[i]) blocks[i] = static_cast<SampleBlock *>(malloc(sizeof(SampleBlock)));
    if (!blocks[i]) { Serial.printf("ERROR: block %u allocation failed\n", unsigned(i)); digitalWrite(cfg::kLedError, HIGH); while (true) delay(1000); }
    xQueueSend(freeQueue, &blocks[i], portMAX_DELAY);
  }
  sdSpi.begin(cfg::kSdSck, cfg::kSdMiso, cfg::kSdMosi, cfg::kSdCs);
  rt.sdReady = SD.begin(cfg::kSdCs, sdSpi, cfg::kSdFrequency);
  if (rt.sdReady) { rt.sdTotal = SD.totalBytes(); rt.sdUsed = SD.usedBytes(); ensureDeviceMetadata(); }
  Serial.printf("SD: %s, total=%llu, free=%llu\n", rt.sdReady ? "OK" : "ERROR", rt.sdTotal, rt.sdTotal - rt.sdUsed);
  if (!initAdc()) { Serial.println("ERROR: ADC init failed"); digitalWrite(cfg::kLedError, HIGH); }
  xTaskCreatePinnedToCore(adcTask, "adc", 4096, nullptr, configMAX_PRIORITIES - 2, &adcTaskHandle, 1);
  xTaskCreatePinnedToCore(writerTask, "sd-writer", 4096, nullptr, 3, &writerTaskHandle, 0);
  setupWeb();
  Serial.println("Commands: START, STOP, STATUS");
}

void loop() {
  server.handleClient();
  if (rt.stopRequested && rt.recording) stopRecording();
  atomicMin(rt.minHeap, ESP.getFreeHeap()); atomicMin(rt.minPsram, ESP.getFreePsram());
  const uint64_t now = esp_timer_get_time();
  digitalWrite(cfg::kLedBus, rt.lastActivityUs && now - rt.lastActivityUs.load() < 25000);
  static bool oldButton = HIGH; static uint32_t changed = 0;
  bool button = digitalRead(cfg::kButton);
  if (button != oldButton && millis() - changed > 40) {
    oldButton = button; changed = millis();
    if (!button) { if (rt.recording) stopRecording(); else startRecording(); }
  }
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n'); cmd.trim(); cmd.toUpperCase();
    if (cmd == "START") Serial.println(startRecording() ? "OK START" : "ERROR START");
    else if (cmd == "STOP") { stopRecording(); Serial.println("OK STOP"); }
    else if (cmd == "STATUS") Serial.println(jsonStatus());
  }
  delay(2);
}

