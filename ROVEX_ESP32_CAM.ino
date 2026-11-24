/*
#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"

// =====================================================
// 1. WIFI SETTINGS
// =====================================================

// Enter your iPhone hotspot name or part of its name
const char *WIFI_NAME_PART = "Pukada Balanne-5G";

// Enter your NEW hotspot password
const char *WIFI_PASSWORD = "bath3972";

// =====================================================
// 2. AI THINKER ESP32-CAM PIN CONFIGURATION
// =====================================================

#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27

#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5

#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

#define FLASH_LED_PIN 4
#define STATUS_LED_PIN 33

// =====================================================
// 3. STREAM SETTINGS
// =====================================================

#define PART_BOUNDARY "123456789000000000000987654321"

static const char *STREAM_CONTENT_TYPE =
  "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;

static const char *STREAM_BOUNDARY =
  "\r\n--" PART_BOUNDARY "\r\n";

static const char *STREAM_PART =
  "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// =====================================================
// 4. GLOBAL VARIABLES
// =====================================================

httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;

volatile uint32_t framesSent = 0;
volatile int streamClients = 0;

float currentFps = 0;
bool flashState = false;

// =====================================================
// 5. WEB PAGE
// =====================================================

const char INDEX_HTML[] PROGMEM = R"rawliteral(

<!DOCTYPE html>
<html>
<head>

<meta charset="UTF-8">

<meta name="viewport"
content="width=device-width, initial-scale=1">

<title>ROVEX LIVE CAMERA</title>

<style>

body {
  background: #111;
  color: white;
  text-align: center;
  font-family: Arial, sans-serif;
  margin: 0;
  padding: 16px;
}

h1 {
  font-size: 26px;
}

img {
  width: 100%;
  max-width: 640px;
  height: auto;
  border-radius: 10px;
  background: black;
  min-height: 180px;
}

#status {
  display: inline-block;
  padding: 8px 16px;
  border-radius: 20px;
  margin: 10px;
  font-weight: bold;
}

.ok {
  background: #1e8e3e;
}

.bad {
  background: #b3261e;
}

.wait {
  background: #8a6d00;
}

button, a {
  display: inline-block;
  padding: 14px 22px;
  margin: 8px;
  background: #333;
  color: white;
  text-decoration: none;
  border: none;
  border-radius: 8px;
  font-size: 16px;
  cursor: pointer;
}

#info {
  color: #aaa;
  font-size: 13px;
  margin: 12px;
}

</style>
</head>

<body>

<h1>ROVEX LIVE CAMERA</h1>

<div id="status" class="wait">
CONNECTING...
</div>

<br>

<img id="camera" alt="Live camera">

<div id="info">
Waiting for camera...
</div>

<br>

<a href="/capture" target="_blank">
TAKE PHOTO
</a>

<button id="flash">
FLASH OFF
</button>

<button id="mode">
MODE: STREAM
</button>

<script>

const img = document.getElementById("camera");

const status = document.getElementById("status");

const info = document.getElementById("info");

let flashState = false;

let snapshotMode = false;

function startStream() {

  img.src =
    "http://" + location.hostname +
    ":81/stream?" + Date.now();

}

function takeSnapshot() {

  img.src = "/capture?" + Date.now();

}

img.onload = function() {

  if (snapshotMode) {
    setTimeout(takeSnapshot, 500);
  }

};

img.onerror = function() {

  setTimeout(function() {

    if (snapshotMode) {
      takeSnapshot();
    } else {
      startStream();
    }

  }, 1500);

};

// FLASH CONTROL

document.getElementById("flash").onclick =
async function() {

  flashState = !flashState;

  try {

    await fetch("/flash?on=" + (flashState ? 1 : 0));

    document.getElementById("flash").textContent =
      flashState ? "FLASH ON" : "FLASH OFF";

  } catch(e) {

    console.log("Flash control failed");

  }

};

// STREAM / SNAPSHOT MODE

document.getElementById("mode").onclick =
function() {

  snapshotMode = !snapshotMode;

  document.getElementById("mode").textContent =
    snapshotMode ? "MODE: SNAPSHOTS" : "MODE: STREAM";

  if (snapshotMode) {
    takeSnapshot();
  } else {
    startStream();
  }

};

// STATUS MONITORING

async function checkStatus() {

  try {

    const response = await fetch(
      "/status?time=" + Date.now()
    );

    const data = await response.json();

    if (snapshotMode) {

      status.textContent = "SNAPSHOT MODE";
      status.className = "ok";

    }

    else if (data.clients > 0 && data.fps > 0.5) {

      status.textContent =
        "LIVE " + data.fps.toFixed(1) + " FPS";

      status.className = "ok";

    }

    else {

      status.textContent = "NO VIDEO YET";
      status.className = "wait";

    }

    info.textContent =
      "IP: " + data.ip +
      " | Signal: " + data.rssi + " dBm" +
      " | Frames: " + data.frames;

  }

  catch(e) {

    status.textContent = "OFFLINE";
    status.className = "bad";

  }

}

startStream();

setInterval(checkStatus, 1500);

checkStatus();

</script>

</body>
</html>

)rawliteral";

// =====================================================
// 6. MAIN WEB PAGE HANDLER
// =====================================================

static esp_err_t index_handler(httpd_req_t *req) {

  httpd_resp_set_type(req, "text/html");

  return httpd_resp_send(
    req,
    INDEX_HTML,
    HTTPD_RESP_USE_STRLEN);
}

// =====================================================
// 7. PHOTO CAPTURE HANDLER
// =====================================================

static esp_err_t capture_handler(httpd_req_t *req) {

  camera_fb_t *fb = esp_camera_fb_get();

  if (!fb) {

    Serial.println("Camera capture failed");

    httpd_resp_send_500(req);

    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");

  httpd_resp_set_hdr(
    req,
    "Cache-Control",
    "no-store");

  esp_err_t result = httpd_resp_send(
    req,
    (const char *)fb->buf,
    fb->len);

  esp_camera_fb_return(fb);

  return result;
}

// =====================================================
// 8. FLASH LED HANDLER
// =====================================================

static esp_err_t flash_handler(httpd_req_t *req) {

  char query[32];
  char value[8];

  if (
    httpd_req_get_url_query_str(
      req, query, sizeof(query))
    == ESP_OK) {

    if (
      httpd_query_key_value(
        query, "on", value, sizeof(value))
      == ESP_OK) {

      flashState = (value[0] == '1');

      digitalWrite(
        FLASH_LED_PIN,
        flashState ? HIGH : LOW);

      Serial.println(
        flashState ? "FLASH ON" : "FLASH OFF");
    }
  }

  httpd_resp_set_type(req, "text/plain");

  return httpd_resp_send(
    req, "OK", HTTPD_RESP_USE_STRLEN);
}

// =====================================================
// 9. STATUS HANDLER
// =====================================================

static esp_err_t status_handler(httpd_req_t *req) {

  char buffer[220];

  snprintf(
    buffer,
    sizeof(buffer),

    "{\"ip\":\"%s\",\"rssi\":%d,\"clients\":%d,"
    "\"fps\":%.1f,\"frames\":%lu,\"heap\":%u}",

    WiFi.localIP().toString().c_str(),
    WiFi.RSSI(),
    (int)streamClients,
    currentFps,
    (unsigned long)framesSent,
    (unsigned)ESP.getFreeHeap());

  httpd_resp_set_type(req, "application/json");

  httpd_resp_set_hdr(
    req,
    "Cache-Control",
    "no-store");

  return httpd_resp_send(
    req, buffer, HTTPD_RESP_USE_STRLEN);
}

// =====================================================
// 10. LIVE VIDEO STREAM HANDLER
// =====================================================

static esp_err_t stream_handler(httpd_req_t *req) {

  camera_fb_t *fb = NULL;

  esp_err_t result = httpd_resp_set_type(
    req, STREAM_CONTENT_TYPE);

  if (result != ESP_OK) {
    return result;
  }

  streamClients++;

  Serial.println("Viewer connected to stream");

  char partBuffer[64];

  while (true) {

    fb = esp_camera_fb_get();

    if (!fb) {

      Serial.println("Camera frame failed");

      delay(30);

      continue;
    }

    size_t headerLength = snprintf(
      partBuffer,
      sizeof(partBuffer),
      STREAM_PART,
      fb->len);

    result = httpd_resp_send_chunk(
      req,
      STREAM_BOUNDARY,
      strlen(STREAM_BOUNDARY));

    if (result == ESP_OK) {

      result = httpd_resp_send_chunk(
        req,
        partBuffer,
        headerLength);
    }

    if (result == ESP_OK) {

      result = httpd_resp_send_chunk(
        req,
        (const char *)fb->buf,
        fb->len);
    }

    esp_camera_fb_return(fb);

    if (result == ESP_OK) {

      framesSent++;

    } else {

      break;
    }
  }

  streamClients--;

  Serial.println("Viewer disconnected");

  return result;
}

// =====================================================
// 11. START WEB SERVERS
// =====================================================

void startServers() {

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();

  config.server_port = 80;

  httpd_uri_t index_uri = {};
  index_uri.uri = "/";
  index_uri.method = HTTP_GET;
  index_uri.handler = index_handler;

  httpd_uri_t capture_uri = {};
  capture_uri.uri = "/capture";
  capture_uri.method = HTTP_GET;
  capture_uri.handler = capture_handler;

  httpd_uri_t flash_uri = {};
  flash_uri.uri = "/flash";
  flash_uri.method = HTTP_GET;
  flash_uri.handler = flash_handler;

  httpd_uri_t status_uri = {};
  status_uri.uri = "/status";
  status_uri.method = HTTP_GET;
  status_uri.handler = status_handler;

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {

    httpd_register_uri_handler(camera_httpd, &index_uri);

    httpd_register_uri_handler(camera_httpd, &capture_uri);

    httpd_register_uri_handler(camera_httpd, &flash_uri);

    httpd_register_uri_handler(camera_httpd, &status_uri);

    Serial.println("Web server started on port 80");

  } else {

    Serial.println("Web server failed to start");
  }

  config.server_port = 81;

  config.ctrl_port = 32769;

  httpd_uri_t stream_uri = {};

  stream_uri.uri = "/stream";

  stream_uri.method = HTTP_GET;

  stream_uri.handler = stream_handler;

  if (httpd_start(&stream_httpd, &config) == ESP_OK) {

    httpd_register_uri_handler(
      stream_httpd, &stream_uri);

    Serial.println("Stream server started on port 81");

  } else {

    Serial.println("Stream server failed to start");
  }
}

// =====================================================
// 12. WIFI NETWORK SCANNING
// =====================================================

String findHotspot() {

  Serial.println("Scanning Wi-Fi networks...");

  int networks = WiFi.scanNetworks();

  String found = "";

  for (int i = 0; i < networks; i++) {

    String name = WiFi.SSID(i);

    Serial.printf(
      "%d. %s | Signal: %d dBm\n",
      i + 1,
      name.c_str(),
      WiFi.RSSI(i));

    String lowerName = name;

    lowerName.toLowerCase();

    String searchName = String(WIFI_NAME_PART);

    searchName.toLowerCase();

    if (
      found == "" && lowerName.indexOf(searchName) >= 0) {

      found = name;
    }
  }

  WiFi.scanDelete();

  return found;
}

// =====================================================
// 13. WIFI CONNECTION
// =====================================================

bool connectWiFi() {

  WiFi.mode(WIFI_STA);

  WiFi.setSleep(false);

  for (int attempt = 1; attempt <= 5; attempt++) {

    Serial.printf(
      "\nWi-Fi attempt %d of 5\n",
      attempt);

    String hotspot = findHotspot();

    if (hotspot == "") {

      Serial.println("Hotspot not found.");

      Serial.println(
        "Check hotspot name and 2.4 GHz compatibility.");

      delay(3000);

      continue;
    }

    Serial.print("Connecting to: ");

    Serial.println(hotspot);

    WiFi.begin(
      hotspot.c_str(),
      WIFI_PASSWORD);

    for (
      int i = 0;
      i < 30 && WiFi.status() != WL_CONNECTED;
      i++) {

      delay(500);

      Serial.print(".");

      digitalWrite(
        STATUS_LED_PIN,
        (i % 2) ? HIGH : LOW);
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {

      Serial.println("Wi-Fi connected!");

      Serial.print("IP address: ");

      Serial.println(WiFi.localIP());

      return true;
    }

    Serial.println("Wi-Fi connection failed.");

    WiFi.disconnect(false, false);

    delay(1000);
  }

  return false;
}

// =====================================================
// 14. CAMERA INITIALIZATION
// =====================================================

bool initializeCamera() {

  camera_config_t config = {};

  config.ledc_channel = LEDC_CHANNEL_0;

  config.ledc_timer = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;

  config.pin_pclk = PCLK_GPIO_NUM;

  config.pin_vsync = VSYNC_GPIO_NUM;

  config.pin_href = HREF_GPIO_NUM;

  config.pin_sccb_sda = SIOD_GPIO_NUM;

  config.pin_sccb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn = PWDN_GPIO_NUM;

  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 10000000;

  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {

    Serial.println("PSRAM FOUND");

    config.frame_size = FRAMESIZE_QVGA;

    config.jpeg_quality = 15;

    config.fb_count = 2;

    config.fb_location = CAMERA_FB_IN_PSRAM;

    config.grab_mode = CAMERA_GRAB_LATEST;

  } else {

    Serial.println("PSRAM NOT FOUND");

    config.frame_size = FRAMESIZE_QVGA;

    config.jpeg_quality = 15;

    config.fb_count = 1;

    config.fb_location = CAMERA_FB_IN_DRAM;

    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }

  esp_err_t error = esp_camera_init(&config);

  if (error != ESP_OK) {

    Serial.printf(
      "Camera initialization failed: 0x%x\n",
      error);

    return false;
  }

  Serial.println("Camera initialized successfully");

  sensor_t *sensor = esp_camera_sensor_get();

  if (sensor != NULL) {

    sensor->set_vflip(sensor, 0);

    sensor->set_hmirror(sensor, 0);
  }

  return true;
}

// =====================================================
// 15. SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  delay(1500);

  Serial.println();

  Serial.println("==============================");

  Serial.println("ROVEX ESP32-CAM");

  Serial.println("==============================");

  pinMode(FLASH_LED_PIN, OUTPUT);

  digitalWrite(FLASH_LED_PIN, LOW);

  pinMode(STATUS_LED_PIN, OUTPUT);

  digitalWrite(STATUS_LED_PIN, HIGH);

  // Initialize camera

  if (!initializeCamera()) {

    Serial.println("Camera failed. Restarting...");

    delay(5000);

    ESP.restart();
  }

  // Connect Wi-Fi

  if (!connectWiFi()) {

    Serial.println("Wi-Fi failed. Restarting...");

    delay(5000);

    ESP.restart();
  }

  digitalWrite(STATUS_LED_PIN, LOW);

  // Start servers

  startServers();

  // Print camera information

  Serial.println();

  Serial.println("==============================");

  Serial.println("ROVEX CAMERA READY");

  Serial.println("==============================");

  Serial.print("IP ADDRESS: ");

  Serial.println(WiFi.localIP());

  Serial.print("MAIN PAGE: http://");

  Serial.print(WiFi.localIP());

  Serial.println("/");

  Serial.print("LIVE STREAM: http://");

  Serial.print(WiFi.localIP());

  Serial.println(":81/stream");

  Serial.println("==============================");
}

// =====================================================
// 16. MAIN LOOP
// =====================================================

void loop() {

  static unsigned long lastTime = millis();

  static uint32_t lastFrames = 0;

  static int statusCounter = 0;

  delay(1000);

  unsigned long now = millis();

  uint32_t currentFrames = framesSent;

  currentFps =
    (currentFrames - lastFrames) * 1000.0f / (float)(now - lastTime);

  lastTime = now;

  lastFrames = currentFrames;

  // Check Wi-Fi connection

  if (WiFi.status() != WL_CONNECTED) {

    digitalWrite(STATUS_LED_PIN, HIGH);

    Serial.println("Wi-Fi disconnected. Reconnecting...");

    WiFi.reconnect();

    return;
  }

  digitalWrite(STATUS_LED_PIN, LOW);

  // Print status every 3 seconds

  if (++statusCounter >= 3) {

    statusCounter = 0;

    Serial.printf(
      "[STATUS] IP: %s | Signal: %d dBm | Viewers: %d | FPS: %.1f\n",
      WiFi.localIP().toString().c_str(),
      WiFi.RSSI(),
      (int)streamClients,
      currentFps);
  }
}



*/








/*
 * ROVEX ESP32-CAM  -  LIVE STREAM + SELF-CHECK VERSION
 * ----------------------------------------------------
 * Fixes / additions compared to the previous sketch:
 *  1. Scans Wi-Fi and prints EVERY network it can see. It finds your hotspot by PART of its
 *     name, so iPhone's curly apostrophe (') or "iPhone" vs "Iphone" no longer breaks it.
 *  2. Never gives up: retries, then restarts itself.
 *  3. Prints the IP address again every 3 seconds (you can't miss it any more).
 *  4. Streaming self-check: Serial shows viewers connected + frames per second.
 *  5. /status page (JSON) and a LIVE / OFFLINE indicator on the main page.
 *  6. Red LED on the back of the ESP32-CAM: blinks while connecting, solid ON when Wi-Fi is OK.
 *
 * Board: AI Thinker ESP32-CAM   PSRAM: Enabled   Serial Monitor: 115200 baud
 */

#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"

// ===================== EDIT THESE =====================
const char* WIFI_NAME_PART = "Pukada Balanne-5G";     // part of your hotspot name (not case sensitive)
const char* WIFI_PASSWORD  = "bath3972";
// ======================================================

// AI THINKER ESP32-CAM PINS
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define FLASH_LED_PIN      4     // bright white LED
#define STATUS_LED_PIN    33     // small red LED on the back (ON = LOW)

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART         = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;

volatile uint32_t framesSent    = 0;
volatile int      streamClients = 0;
float             currentFps    = 0;

// =====================================================
// MAIN PAGE
// =====================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ROVEX LIVE</title>
<style>
body{background:#111;color:#fff;text-align:center;font-family:Arial,sans-serif;margin:0;padding:16px}
h1{font-size:26px;margin:6px 0 10px}
img{width:100%;max-width:640px;height:auto;border-radius:10px;background:#000;min-height:180px}
#st{display:inline-block;padding:6px 14px;border-radius:20px;font-weight:700;margin:6px 0}
.ok{background:#1e8e3e}.bad{background:#b3261e}.wait{background:#8a6d00}
a,button{display:inline-block;padding:14px 22px;margin:8px;background:#333;color:#fff;text-decoration:none;border:0;border-radius:8px;font-size:16px}
#info{color:#9aa4b2;font-size:13px}
</style></head><body>
<h1>ROVEX LIVE CAMERA</h1>
<div id="st" class="wait">CONNECTING...</div><br>
<img id="cam" alt="live stream">
<div id="info">-</div>
<a href="/capture" target="_blank">TAKE PHOTO</a>
<button id="fl">FLASH</button>
<button id="mode">MODE: STREAM</button>
<script>
const img=document.getElementById('cam'),st=document.getElementById('st'),info=document.getElementById('info');
let flash=0,snapMode=false;
function start(){img.src='http://'+location.hostname+':81/stream?'+Date.now();}
function snap(){img.src='/capture?'+Date.now();}
img.onload=()=>{if(snapMode)setTimeout(snap,100);};
img.onerror=()=>{setTimeout(snapMode?snap:start,1500);};
document.getElementById('fl').onclick=()=>{flash=1-flash;fetch('/flash?on='+flash);};
document.getElementById('mode').onclick=()=>{
  snapMode=!snapMode;
  document.getElementById('mode').textContent='MODE: '+(snapMode?'SNAPSHOTS':'STREAM');
  snapMode?snap():start();
};
async function poll(){
  try{
    const j=await (await fetch('/status',{cache:'no-store'})).json();
    if(snapMode){st.textContent='SNAPSHOT MODE';st.className='ok';}
    else if(j.clients>0&&j.fps>0.5){st.textContent='LIVE  '+j.fps.toFixed(1)+' FPS';st.className='ok';}
    else{st.textContent='NO VIDEO YET';st.className='wait';}
    info.textContent='IP '+j.ip+'   Signal '+j.rssi+' dBm   Frames sent '+j.frames;
  }catch(e){st.textContent='OFFLINE';st.className='bad';}
}
start();setInterval(poll,1500);poll();
</script></body></html>
)rawliteral";

static esp_err_t index_handler(httpd_req_t *req)
{
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

// =====================================================
// CAPTURE PHOTO
// =====================================================
static esp_err_t capture_handler(httpd_req_t *req)
{
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb)
  {
    Serial.println("Camera capture failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t result = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return result;
}

// =====================================================
// FLASH
// =====================================================
static esp_err_t flash_handler(httpd_req_t *req)
{
  char query[32];
  char value[8];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "on", value, sizeof(value)) == ESP_OK)
  {
    digitalWrite(FLASH_LED_PIN, value[0] == '1' ? HIGH : LOW);
    Serial.println(value[0] == '1' ? "FLASH ON" : "FLASH OFF");
  }
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

// =====================================================
// STATUS (JSON)  ->  http://IP/status
// =====================================================
static esp_err_t status_handler(httpd_req_t *req)
{
  char buf[220];
  snprintf(buf, sizeof(buf),
    "{\"ip\":\"%s\",\"rssi\":%d,\"clients\":%d,\"fps\":%.1f,\"frames\":%lu,\"heap\":%u}",
    WiFi.localIP().toString().c_str(), WiFi.RSSI(), (int)streamClients, currentFps,
    (unsigned long)framesSent, (unsigned)ESP.getFreeHeap());
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

// =====================================================
// LIVE STREAM  ->  http://IP:81/stream
// =====================================================
static esp_err_t stream_handler(httpd_req_t *req)
{
  camera_fb_t *fb = NULL;
  esp_err_t result = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (result != ESP_OK) return result;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char part_buffer[64];
  streamClients++;
  Serial.println(">>> Viewer CONNECTED to stream");

  uint32_t capSum = 0, sendSum = 0, count = 0, lastLog = millis();
  size_t lastLen = 0;

  while (true)
  {
    uint32_t t0 = millis();
    fb = esp_camera_fb_get();
    uint32_t t1 = millis();

    if (!fb)
    {
      Serial.println("Camera frame failed");
      result = ESP_FAIL;
    }
    else
    {
      lastLen = fb->len;
      size_t header_length = snprintf(part_buffer, sizeof(part_buffer), STREAM_PART, fb->len);
      result = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
      if (result == ESP_OK) result = httpd_resp_send_chunk(req, part_buffer, header_length);
      if (result == ESP_OK) result = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
      esp_camera_fb_return(fb);
      if (result == ESP_OK) framesSent++;
    }

    uint32_t t2 = millis();
    capSum  += (t1 - t0);
    sendSum += (t2 - t1);
    count++;

    // Every 3 s: where is the time going?  capture = camera side, send = Wi-Fi/browser side
    if (millis() - lastLog >= 3000 && count > 0)
    {
      Serial.printf("[TIMING] capture %u ms | send %u ms | frame %u bytes | frames %u\n",
                    (unsigned)(capSum / count), (unsigned)(sendSum / count),
                    (unsigned)lastLen, (unsigned)count);
      capSum = sendSum = count = 0;
      lastLog = millis();
    }

    if (result != ESP_OK) break;
  }

  streamClients--;
  Serial.println("<<< Viewer DISCONNECTED from stream");
  return result;
}

// =====================================================
// START SERVERS
// =====================================================
void startServers()
{
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t index_uri   = {};  index_uri.uri   = "/";        index_uri.method   = HTTP_GET; index_uri.handler   = index_handler;
  httpd_uri_t capture_uri = {};  capture_uri.uri = "/capture"; capture_uri.method = HTTP_GET; capture_uri.handler = capture_handler;
  httpd_uri_t flash_uri   = {};  flash_uri.uri   = "/flash";   flash_uri.method   = HTTP_GET; flash_uri.handler   = flash_handler;
  httpd_uri_t status_uri  = {};  status_uri.uri  = "/status";  status_uri.method  = HTTP_GET; status_uri.handler  = status_handler;

  if (httpd_start(&camera_httpd, &config) == ESP_OK)
  {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &flash_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    Serial.println("Web server started: PORT 80");
  }

  config.server_port = 81;
  config.ctrl_port   = 32769;

  httpd_uri_t stream_uri = {};
  stream_uri.uri = "/stream"; stream_uri.method = HTTP_GET; stream_uri.handler = stream_handler;

  if (httpd_start(&stream_httpd, &config) == ESP_OK)
  {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    Serial.println("Stream server started: PORT 81");
  }
}

// =====================================================
// WIFI HELPERS
// =====================================================
const char* wifiStatusText(wl_status_t s)
{
  switch (s)
  {
    case WL_NO_SSID_AVAIL:  return "NO_SSID_AVAILABLE (network not visible / wrong name)";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED (wrong password or hotspot refused)";
    case WL_CONNECTION_LOST:return "CONNECTION_LOST";
    case WL_DISCONNECTED:   return "DISCONNECTED";
    case WL_IDLE_STATUS:    return "IDLE";
    case WL_CONNECTED:      return "CONNECTED";
    default:                return "OTHER";
  }
}

// Scan, print every network, return the real SSID that contains WIFI_NAME_PART
String findHotspot()
{
  Serial.println();
  Serial.println("Scanning Wi-Fi networks...");
  int n = WiFi.scanNetworks();
  String found = "";
  if (n <= 0) Serial.println("  (no networks found)");
  for (int i = 0; i < n; i++)
  {
    String s = WiFi.SSID(i);
    Serial.printf("  %2d) [%s]  %d dBm  channel %d\n", i + 1, s.c_str(), WiFi.RSSI(i), WiFi.channel(i));
    String lower = s;
    lower.toLowerCase();
    String part = String(WIFI_NAME_PART);
    part.toLowerCase();
    if (found == "" && lower.indexOf(part) >= 0) found = s;
  }
  WiFi.scanDelete();
  return found;
}

bool connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  for (int round = 1; round <= 5; round++)
  {
    Serial.printf("\n--- Wi-Fi attempt %d of 5 ---\n", round);
    String name = findHotspot();

    if (name == "")
    {
      Serial.println("HOTSPOT NOT FOUND. On the iPhone:");
      Serial.println("  Settings > Personal Hotspot > turn ON 'Maximize Compatibility' (2.4 GHz)");
      Serial.println("  and keep that Settings screen open while the ESP32 connects.");
      delay(3000);
      continue;
    }

    Serial.print("Connecting to: [");
    Serial.print(name);
    Serial.println("]");
    WiFi.begin(name.c_str(), WIFI_PASSWORD);

    for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++)
    {
      delay(500);
      Serial.print(".");
      digitalWrite(STATUS_LED_PIN, i % 2);        // blink red LED while connecting
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) return true;

    Serial.print("Failed: ");
    Serial.println(wifiStatusText(WiFi.status()));
    WiFi.disconnect(true);
    delay(1000);
  }
  return false;
}

void printBanner()
{
  String ip = WiFi.localIP().toString();
  Serial.println();
  Serial.println("==============================================");
  Serial.println("ROVEX CAMERA READY");
  Serial.println("==============================================");
  Serial.println("IP ADDRESS : " + ip);
  Serial.println("MAIN PAGE  : http://" + ip + "/");
  Serial.println("LIVE STREAM: http://" + ip + ":81/stream");
  Serial.println("PHOTO      : http://" + ip + "/capture");
  Serial.println("STATUS     : http://" + ip + "/status");
  Serial.println("==============================================");
}

// =====================================================
// SETUP
// =====================================================
void setup()
{
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("==============================");
  Serial.println("ROVEX ESP32-CAM (v2: timing + snapshot mode)");
  Serial.println("==============================");

  pinMode(FLASH_LED_PIN, OUTPUT);   digitalWrite(FLASH_LED_PIN, LOW);
  pinMode(STATUS_LED_PIN, OUTPUT);  digitalWrite(STATUS_LED_PIN, HIGH);   // red LED off

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk  = XCLK_GPIO_NUM;
  config.pin_pclk  = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href  = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn  = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;   // 10 MHz is gentler on weak power (use 20000000 for max speed)
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound())
  {
    Serial.println("PSRAM FOUND");
    config.frame_size   = FRAMESIZE_QVGA;   // 320x240 (light). Try FRAMESIZE_VGA later
    config.jpeg_quality = 15;               // higher number = smaller frames
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_LATEST;
  }
  else
  {
    Serial.println("PSRAM NOT FOUND (enable PSRAM in Tools menu)");
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 15;
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_DRAM;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  }

  esp_err_t error = esp_camera_init(&config);
  if (error != ESP_OK)
  {
    Serial.printf("CAMERA INIT FAILED: 0x%x\n", error);
    Serial.println("Check: camera ribbon cable seated, 5V power strong enough. Restarting in 5 s...");
    delay(5000);
    ESP.restart();
  }
  Serial.println("Camera initialized OK");

  sensor_t *sensor = esp_camera_sensor_get();
  sensor->set_vflip(sensor, 0);
  sensor->set_hmirror(sensor, 0);

  if (!connectWiFi())
  {
    Serial.println("\nWIFI FAILED after 5 attempts. Restarting in 5 s...");
    delay(5000);
    ESP.restart();
  }

  digitalWrite(STATUS_LED_PIN, LOW);     // red LED solid ON = Wi-Fi connected
  startServers();
  printBanner();
}

// =====================================================
// LOOP  -  prints the IP + stream health every 3 seconds
// =====================================================
void loop()
{
  static unsigned long lastT = millis();
  static uint32_t lastF = 0;
  static int tick = 0;

  delay(1000);

  unsigned long now = millis();
  uint32_t f = framesSent;
  currentFps = (f - lastF) * 1000.0f / (float)(now - lastT);
  lastT = now;
  lastF = f;

  if (WiFi.status() != WL_CONNECTED)
  {
    digitalWrite(STATUS_LED_PIN, HIGH);
    Serial.println("WiFi disconnected - reconnecting...");
    WiFi.reconnect();
    return;
  }
  digitalWrite(STATUS_LED_PIN, LOW);

  if (++tick >= 3)
  {
    tick = 0;
    Serial.printf("[STATUS] IP %s | %d dBm | viewers: %d | %.1f FPS | stream: ",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI(), (int)streamClients, currentFps);
    if (streamClients > 0 && currentFps > 0.5f) Serial.println("WORKING");
    else if (streamClients > 0)                 Serial.println("viewer connected but NO FRAMES");
    else                                        Serial.println("waiting for a viewer -> open http://" + WiFi.localIP().toString() + "/");
  }
}

