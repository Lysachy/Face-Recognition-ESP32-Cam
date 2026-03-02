#include "esp_camera.h"
#include "esp_http_server.h"
#include "fd_forward.h"
#include "fr_flash.h"
#include "fr_forward.h"
#include <WiFi.h>

// ==========================================
// PIN DEFINITION FOR AI-THINKER ESP32-CAM
// ==========================================
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

// ==========================================
// CUSTOM COMPONENTS PINS
// ==========================================
#define RELAY_PIN 12
#define LED_PIN 13
#define BUZZER_PIN 14
#define BUTTON_PIN 15

// ==========================================
// WIFI CREDENTIALS (GANTI DENGAN WIFI ANDA)
// ==========================================
const char *ssid = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";

// Face Recognition Settings
#define ENROLL_CONFIRM_TIMES 5
#define FACE_ID_SAVE_NUMBER 7

// Globals
static mtmn_config_t mtmn_config = {0};
static face_id_list id_list = {0};
bool enroll_mode = false;
String system_message = "System Booting...";

httpd_handle_t camera_httpd = NULL;

const char *INDEX_HTML = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32-CAM Smart Solenoid</title>
  <style>
    body { font-family: Arial, sans-serif; text-align: center; background-color: #f4f4f4; margin: 0; padding: 20px; }
    h2 { color: #333; }
    #stream { width: 100%; max-width: 400px; border-radius: 8px; box-shadow: 0 4px 8px rgba(0,0,0,0.2); }
    .status-box { background: white; margin: 20px auto; padding: 15px; max-width: 400px; border-radius: 8px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); }
    .status { font-size: 18px; font-weight: bold; color: #007BFF; }
    .footer { margin-top: 20px; font-size: 12px; color: #666; }
  </style>
</head>
<body>
  <h2>ESP32-CAM Face Recognition</h2>
  <img id="stream" src="/stream" crossorigin>
  <div class="status-box">
    <p>Status: <span id="status" class="status">Loading...</span></p>
  </div>
  <p class="footer">Tekan tombol TACT Switch di hardware untuk mendaftarkan wajah baru.</p>
  <script>
    // Auto-update status text every 1 second
    setInterval(() => {
      fetch('/status').then(r => r.text()).then(t => {
        document.getElementById('status').innerText = t;
        if (t.includes("Recognized") || t.includes("Open")) {
          document.getElementById('status').style.color = "green";
        } else if (t.includes("Unknown")) {
          document.getElementById('status').style.color = "red";
        } else {
          document.getElementById('status').style.color = "#007BFF";
        }
      });
    }, 1000);
  </script>
</body>
</html>
)rawliteral";

// Web server handlers for streaming
#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

static esp_err_t status_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, system_message.c_str(), system_message.length());
}

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t *_jpg_buf = NULL;
  char *part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK)
    return res;

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      _jpg_buf_len = fb->len;
      _jpg_buf = fb->buf;
    }

    if (res == ESP_OK) {
      size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY,
                                  strlen(_STREAM_BOUNDARY));
    }

    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if (_jpg_buf) {
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if (res != ESP_OK) {
      break;
    }
  }
  return res;
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t index_uri = {.uri = "/",
                           .method = HTTP_GET,
                           .handler = index_handler,
                           .user_ctx = NULL};
  httpd_uri_t status_uri = {.uri = "/status",
                            .method = HTTP_GET,
                            .handler = status_handler,
                            .user_ctx = NULL};
  httpd_uri_t stream_uri = {.uri = "/stream",
                            .method = HTTP_GET,
                            .handler = stream_handler,
                            .user_ctx = NULL};

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_uri_handler(camera_httpd, &stream_uri);
  }
}

// Actions
void beep_success() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(100);
  digitalWrite(BUZZER_PIN, LOW);
  delay(100);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(100);
  digitalWrite(BUZZER_PIN, LOW);
}

void beep_failed() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(1000);
  digitalWrite(BUZZER_PIN, LOW);
}

void open_door() {
  system_message = "Solenoid Open!";
  Serial.println(system_message);
  digitalWrite(LED_PIN, HIGH);
  digitalWrite(RELAY_PIN, HIGH);
  beep_success();

  delay(3000); // Solenoid open for 3 seconds

  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
  system_message = "Standby";
  Serial.println("Solenoid Closed.");
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);

  // Initialize Pins
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  // Configure Camera
  camera_config_t config;
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
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  // Use JPEG for faster web streaming!
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count =
        2; // Required 2 frames so stream and recognition don't block each other
  } else {
    config.frame_size = FRAMESIZE_QQVGA;
    config.jpeg_quality = 15;
    config.fb_count = 1;
  }

  // Camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("WiFi Connected. IP Address: http://");
  Serial.println(WiFi.localIP());

  // Start HTTP Server
  startCameraServer();

  // Init Face Detection & Recognition
  mtmn_config = mtmn_init_config();
  face_id_init(&id_list, FACE_ID_SAVE_NUMBER, ENROLL_CONFIRM_TIMES);
  read_face_id_from_flash_with_name(&id_list);

  system_message = "Standby - Ready";
  Serial.println("System Ready. Press button to enroll a new face.");
}

void loop() {
  // Check button for enrollment
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      enroll_mode = true;
      system_message = "Enroll Mode Started - Look at camera";
      Serial.println(system_message);
      digitalWrite(LED_PIN, HIGH);
      delay(1000);
    }
  }

  // Capture frame for Background Face Recognition
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    return;
  }

  // Convert JPEG to RGB888 for Face Detection AI
  dl_matrix3du_t *image_matrix =
      dl_matrix3du_alloc(1, fb->width, fb->height, 3);
  if (!image_matrix) {
    esp_camera_fb_return(fb);
    return;
  }

  bool converted = fmt2rgb888(fb->buf, fb->len, fb->format, image_matrix->item);
  esp_camera_fb_return(fb); // return fb quickly so Web Stream can use it

  if (converted) {
    box_array_t *net_boxes = face_detect(image_matrix, &mtmn_config);

    if (net_boxes) {
      if (enroll_mode) {
        int8_t left_result = align_face(net_boxes, image_matrix);
        // Face aligned successfully
        if (left_result == ESP_OK) {
          int8_t enroll_result = enroll_face(&id_list, image_matrix);
          if (enroll_result == FACE_ENROLL_OK) {
            system_message = "Face Enrolled Successfully!";
            Serial.println(system_message);
            enroll_mode = false;
            digitalWrite(LED_PIN, LOW);
            beep_success();
            delay(2000);
            system_message = "Standby";
          } else {
            system_message =
                "Enrolling... Need " + String(enroll_result) + " more captures";
            Serial.println(system_message);
            digitalWrite(LED_PIN, !digitalRead(LED_PIN)); // Blink LED
          }
        }
      } else {
        // Recognition mode
        int8_t left_result = align_face(net_boxes, image_matrix);
        if (left_result == ESP_OK) {
          face_id_node *f = recognize_face_with_name(&id_list, image_matrix);
          if (f) {
            system_message = String("Recognized ID: ") + f->id_name;
            open_door();
          } else {
            system_message = "Unknown Face detected";
            Serial.println(system_message);
            beep_failed();
            delay(2000); // Prevent alarm spam
            system_message = "Standby";
          }
        }
      }
      free(net_boxes->score);
      free(net_boxes->box);
      free(net_boxes->landmark);
      free(net_boxes);
    }
  }

  dl_matrix3du_free(image_matrix);

  // Give stream handler and networking a chance to breathe
  delay(150);
}
