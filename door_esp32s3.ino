/*
  Cửa thông minh nhận diện khuôn mặt - ESP32-S3
  App điện thoại nhận diện mặt rồi gửi lệnh qua MQTT:
    <PREFIX>/cmd     ← "OPEN|<SECRET>"  (đúng người)  /  "DENY|<SECRET>" (người lạ)
    <PREFIX>/status  → "opened" / "closed" / "denied" / "online"
    <PREFIX>/pir     → "motion" khi cảm biến phát hiện chuyển động
  Thư viện cần cài: PubSubClient (Nick O'Leary), ESP32Servo
*/
#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>

// ====== CẤU HÌNH ======
const char* WIFI_SSID   = "TEN_WIFI";
const char* WIFI_PASS   = "MAT_KHAU_WIFI";
const char* MQTT_HOST   = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;
const String PREFIX     = "smartdoor_demo_123";   // phải trùng "Tiền tố topic" trong app
const String SECRET     = "doi-ma-nay";           // phải trùng "Mã bí mật" trong app
const unsigned long HOLD_OPEN_MS = 5000;          // giữ cửa mở tối thiểu sau lệnh OPEN

// ====== PHẦN CỨNG (giữ nguyên như dự án cũ) ======
Servo doorServo;
const int servoPin = 18;
const int pirPin   = 4;
const int ledPin   = 8;
const int STOP_US = 1500, OPEN_US = 1800, CLOSE_US = 1200;
int RUN_TIME = 900;
bool doorIsOpen = false;

WiFiClient espClient;
PubSubClient mqtt(espClient);
String tCmd, tStatus, tPir;

volatile int pendingCmd = 0;          // 0: không có, 1: OPEN, 2: DENY
unsigned long openedAt = 0, lastPir = 0, lastReconnect = 0;
int lastPirState = LOW;

void say(const char* s) { if (mqtt.connected()) mqtt.publish(tStatus.c_str(), s); }

void onMessage(char* topic, byte* payload, unsigned int len) {
  String m; for (unsigned int i = 0; i < len; i++) m += (char)payload[i];
  Serial.println("MQTT nhận: " + m.substring(0, m.indexOf('|')));
  if (!m.endsWith("|" + SECRET)) { Serial.println("Sai mã bí mật, bỏ qua"); return; }
  if (m.startsWith("OPEN")) pendingCmd = 1;
  else if (m.startsWith("DENY")) pendingCmd = 2;
}

void openDoor() {
  digitalWrite(ledPin, HIGH);
  doorServo.attach(servoPin, 500, 2400);
  doorServo.writeMicroseconds(OPEN_US);
  delay(RUN_TIME);
  doorServo.writeMicroseconds(STOP_US);
  delay(200);
  doorServo.detach();
  doorIsOpen = true;
  say("opened");
}

void closeDoor() {
  digitalWrite(ledPin, LOW);
  doorServo.attach(servoPin, 500, 2400);
  doorServo.writeMicroseconds(CLOSE_US);
  delay(RUN_TIME);
  doorServo.writeMicroseconds(STOP_US);
  delay(200);
  doorServo.detach();
  doorIsOpen = false;
  say("closed");
}

void warnBlink() {                    // người lạ: cửa giữ nguyên, LED nháy cảnh báo
  for (int i = 0; i < 6; i++) { digitalWrite(ledPin, HIGH); delay(100); digitalWrite(ledPin, LOW); delay(100); }
  digitalWrite(ledPin, doorIsOpen ? HIGH : LOW);
  say("denied");
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Kết nối WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.println("\nWiFi OK: " + WiFi.localIP().toString());
}

void reconnectMqtt() {
  if (mqtt.connected() || millis() - lastReconnect < 3000) return;
  lastReconnect = millis();
  String id = "esp32s3_" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqtt.connect(id.c_str())) {
    mqtt.subscribe(tCmd.c_str());
    say("online");
    Serial.println("MQTT OK");
  }
}

void setup() {
  Serial.begin(115200);
  ESP32PWM::allocateTimer(0); ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2); ESP32PWM::allocateTimer(3);
  pinMode(pirPin, INPUT);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);
  doorServo.setPeriodHertz(50);

  tCmd = PREFIX + "/cmd"; tStatus = PREFIX + "/status"; tPir = PREFIX + "/pir";
  connectWifi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  Serial.println("Cửa thông minh sẵn sàng, chờ lệnh từ app.");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWifi();
  reconnectMqtt();
  mqtt.loop();

  // Xử lý lệnh từ app (làm ở loop để không chặn callback MQTT)
  if (pendingCmd == 1) {
    pendingCmd = 0;
    if (!doorIsOpen) openDoor();
    openedAt = millis();              // nhận OPEN lần nữa thì gia hạn thời gian mở
  } else if (pendingCmd == 2) {
    pendingCmd = 0;
    warnBlink();
  }

  // PIR chỉ báo chuyển động lên app, KHÔNG tự mở cửa nữa
  int pir = digitalRead(pirPin);
  if (pir == HIGH && lastPirState == LOW && millis() - lastPir > 2000) {
    lastPir = millis();
    if (mqtt.connected()) mqtt.publish(tPir.c_str(), "motion");
  }
  lastPirState = pir;

  // Tự đóng cửa: đã hết thời gian giữ mở và không còn ai đứng trước cảm biến
  if (doorIsOpen && millis() - openedAt > HOLD_OPEN_MS && pir == LOW) {
    delay(3000);                      // chờ người đi hẳn như code cũ
    if (digitalRead(pirPin) == LOW) closeDoor();
    else openedAt = millis();
  }
  delay(50);
}
