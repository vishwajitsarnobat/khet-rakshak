/*
  KHET-RAKSHAK bench prototype (ESP32-CAM, AI-Thinker)
  PIR (GPIO13) -> photo -> AI -> buzzer + LED (GPIO12) if harmful
  Every detection: photo saved as <datetime>_<animal>.jpg + line in /log.csv
  Real time from phone hotspot at boot (IST). No WiFi -> time since power-on.
  Footage test (Serial Monitor 115200, No line ending), type TRUE animal:
    b = wild_boar, n = nilgai, m = monkey, d = deer, s = safe
*/
#include <khet-rakshak_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"
#include "esp_camera.h"
#include "img_converters.h"
#include "FS.h"
#include "SD_MMC.h"
#include <WiFi.h>
#include "time.h"

// ===== CHANGE THESE: your phone hotspot. if this is done timestamps will be proper while saving images =====
const char *WIFI_SSID = "YOUR_HOTSPOT_NAME";
const char *WIFI_PASS = "YOUR_HOTSPOT_PASSWORD";

// AI-Thinker ESP32-CAM camera pins
#define PWDN_GPIO_NUM  32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM   0
#define SIOD_GPIO_NUM  26
#define SIOC_GPIO_NUM  27
#define Y9_GPIO_NUM    35
#define Y8_GPIO_NUM    34
#define Y7_GPIO_NUM    39
#define Y6_GPIO_NUM    36
#define Y5_GPIO_NUM    21
#define Y4_GPIO_NUM    19
#define Y3_GPIO_NUM    18
#define Y2_GPIO_NUM     5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM  23
#define PCLK_GPIO_NUM  22

#define PIR_PIN   13   // PIR OUT
#define ALARM_PIN 12   // 330 ohm -> transistor base -> buzzer + red LED

#define CAP_W 320
#define CAP_H 240
const float THRESHOLD = 0.50;          // below this: UNCERTAIN, no alarm
const uint32_t ALARM_MS = 3000;
const uint32_t PIR_COOLDOWN_MS = 5000;

static uint8_t *snapshot_buf = nullptr;
static bool sd_ok = false, time_ok = false;
static uint32_t det_count = 0, test_total = 0, test_correct = 0, last_pir = 0;

// ---------- time ----------
void sync_time() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  if (WiFi.status() == WL_CONNECTED) {
    configTime(19800, 0, "pool.ntp.org", "time.google.com");   // IST = UTC+5:30
    struct tm t;
    time_ok = getLocalTime(&t, 10000);
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);              // clock keeps running without WiFi
  Serial.println(time_ok ? "Time synced (IST)" : "No WiFi: using time since power-on");
}

String now_str(bool for_file) {
  if (!time_ok) return String(for_file ? "t" : "") + String(millis() / 1000) + (for_file ? "" : "s since start");
  struct tm t;
  getLocalTime(&t);
  char b[24];
  strftime(b, sizeof(b), for_file ? "%Y%m%d_%H%M%S" : "%Y-%m-%d %H:%M:%S", &t);
  return String(b);
}

// ---------- camera ----------
bool camera_init() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM; c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href = HREF_GPIO_NUM; c.pin_sscb_sda = SIOD_GPIO_NUM; c.pin_sscb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_QVGA;
  c.jpeg_quality = 12;
  c.fb_count = 1;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  if (esp_camera_init(&c) != ESP_OK) return false;
  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 0);     // set to 1 if photos are upside down
  s->set_hmirror(s, 0);
  return true;
}

static int get_data(size_t offset, size_t length, float *out_ptr) {
  size_t px = offset * 3;
  for (size_t i = 0; i < length; i++) {
    out_ptr[i] = (snapshot_buf[px + 2] << 16) + (snapshot_buf[px + 1] << 8) + snapshot_buf[px];
    px += 3;
  }
  return 0;
}

void log_line(const String &line) {
  Serial.println(line);
  if (!sd_ok) return;
  File f = SD_MMC.open("/log.csv", FILE_APPEND);
  if (f) { f.println(line); f.close(); }
}

// ---------- one detection ----------
void detect(const char *source, const String &truth) {
  camera_fb_t *fb = esp_camera_fb_get();          // discard stale frame
  if (fb) esp_camera_fb_return(fb);
  fb = esp_camera_fb_get();
  if (!fb) { Serial.println("Camera capture failed"); return; }
  String when = now_str(false);
  String when_file = now_str(true);
  det_count++;

  if (!fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, snapshot_buf)) {
    esp_camera_fb_return(fb);
    Serial.println("JPEG decode failed");
    return;
  }
  ei::image::processing::crop_and_interpolate_rgb888(
      snapshot_buf, CAP_W, CAP_H, snapshot_buf,
      EI_CLASSIFIER_INPUT_WIDTH, EI_CLASSIFIER_INPUT_HEIGHT);

  signal_t signal;
  signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
  signal.get_data = &get_data;
  ei_impulse_result_t result = {0};
  uint32_t t0 = millis();
  if (run_classifier(&signal, &result, false) != EI_IMPULSE_OK) {
    esp_camera_fb_return(fb);
    Serial.println("Classifier failed");
    return;
  }
  uint32_t ms = millis() - t0;

  const char *animal = "";
  float conf = 0;
  for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    if (result.classification[i].value > conf) {
      conf = result.classification[i].value;
      animal = result.classification[i].label;
    }
  }
  bool uncertain = conf < THRESHOLD;
  bool harmful = strcmp(animal, "safe") != 0 && !uncertain;
  const char *harm_txt = harmful ? "YES" : (uncertain ? "UNCERTAIN" : "NO");

  // save photo named by time + animal
  String photo = "/" + when_file + "_" + animal + ".jpg";
  if (sd_ok) {
    File f = SD_MMC.open(photo.c_str(), FILE_WRITE);
    if (f) { f.write(fb->buf, fb->len); f.close(); }
  }
  esp_camera_fb_return(fb);

  String correct = "";
  if (truth.length()) {
    test_total++;
    bool right = truth.equals(animal);
    if (right) test_correct++;
    correct = right ? "YES" : "NO";
  }
  String acc = test_total ? String(100.0f * test_correct / test_total, 1) : "";

  // date_time,trigger,animal_detected,harmful,confidence,photo,true_label,correct,running_acc,ai_ms
  log_line(when + "," + source + "," + animal + "," + harm_txt + "," + String(conf, 2) + "," +
           photo + "," + truth + "," + correct + "," + acc + "," + String(ms));

  if (harmful) {
    digitalWrite(ALARM_PIN, HIGH);
    delay(ALARM_MS);
    digitalWrite(ALARM_PIN, LOW);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(ALARM_PIN, OUTPUT);
  digitalWrite(ALARM_PIN, LOW);
  pinMode(PIR_PIN, INPUT);

  if (!camera_init()) { Serial.println("Camera init FAILED: check ribbon cable"); while (true) delay(1000); }
  snapshot_buf = (uint8_t *)ps_malloc(CAP_W * CAP_H * 3);
  if (!snapshot_buf) { Serial.println("No PSRAM: enable PSRAM in Tools menu"); while (true) delay(1000); }

  sync_time();

  sd_ok = SD_MMC.begin("/sdcard", true);   // 1-bit mode keeps GPIO12/13 free
  Serial.println(sd_ok ? "SD card OK" : "SD card not found (continuing without saving)");
  if (sd_ok && !SD_MMC.exists("/log.csv")) {
    File f = SD_MMC.open("/log.csv", FILE_WRITE);
    if (f) { f.println("date_time,trigger,animal_detected,harmful,confidence,photo,true_label,correct,running_acc,ai_ms"); f.close(); }
  }
  Serial.println("KHET-RAKSHAK ready. Type b/n/m/d/s for a footage test, or wave at the PIR.");
  delay(2000);
}

void loop() {
  if (Serial.available()) {
    char k = Serial.read();
    String truth = "";
    if (k == 'b') truth = "wild_boar";
    else if (k == 'n') truth = "nilgai";
    else if (k == 'm') truth = "monkey";
    else if (k == 'd') truth = "deer";
    else if (k == 's') truth = "safe";
    if (truth.length()) detect("TEST", truth);
  }
  if (digitalRead(PIR_PIN) == HIGH && millis() - last_pir > PIR_COOLDOWN_MS) {
    detect("PIR", "");
    last_pir = millis();
  }
  delay(20);
}
