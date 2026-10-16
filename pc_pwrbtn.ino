/*
 * pc_pwrbtn.ino — ESP32-S3 电脑远程开关机/复位/指示灯硬件回读完整版
 * ----------------------------------------------------------------------------
 * 1. 核心控制（连续四个脚）：
 *    - 电源键 (GPIO 4)：并联机箱 POWER SW，开漏输出（短按 0.5s 开机/关机；长按 4.5s 强制断电，断电自松）
 *    - 复位键 (GPIO 5)：并联机箱 RESET SW，开漏输出（脉冲 0.5s 硬重启）
 * 2. 状态真机感知（前面板灯低端控制）：
 *    - 电源指示灯 (GPIO 6)：单接 PLED- 负极，低电平有效（开机拉低至 0V，关机断开为 3.3V，pledInv=1）
 *    - 硬盘指示灯 (GPIO 7)：单接 HDLED- 负极，低电平有效（读写拉低至 0V 触发闪烁，关机 0V 自动互锁）
 * 3. 蓝牙全量无线配置：
 *    - 广播名：pc_pwrbtn (GATT Service UUID: 7a1e0001-9c3b-4f2a-8d5e-1b2c3d4e5f60)
 *    - 全参数均可通过蓝牙配置，彻底解决改引脚、换网络、调反相无需重烧代码的问题
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include "driver/gpio.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#if __has_include("secrets.h")
  #include "secrets.h"
#endif

// 兼容 DEF_ 宏
#ifdef DEF_WIFI_SSID
  #define WIFI_SSID       DEF_WIFI_SSID
#endif
#ifdef DEF_EAP_USER
  #define EAP_USER        DEF_EAP_USER
#endif
#ifdef DEF_EAP_PASS
  #define EAP_PASS        DEF_EAP_PASS
#endif
#ifdef DEF_MQTT_HOST
  #define MQTT_HOST       DEF_MQTT_HOST
#endif
#ifdef DEF_MQTT_PORT
  #define MQTT_PORT       DEF_MQTT_PORT
#endif
#ifdef DEF_TOPIC
  #define MQTT_TOPIC_BASE DEF_TOPIC
#endif
#ifdef DEF_BLE_PIN
  #define BLE_AUTH_PIN    DEF_BLE_PIN
#endif

#ifndef WIFI_SSID
#define WIFI_SSID       "ShanghaiTech"
#endif
#ifndef EAP_USER
#define EAP_USER        "YOUR_STUDENT_ID"
#endif
#ifndef EAP_PASS
#define EAP_PASS        "YOUR_CAMPUS_PASSWORD"
#endif
#ifndef MQTT_HOST
#define MQTT_HOST       "broker.emqx.io"
#endif
#ifndef MQTT_PORT
#define MQTT_PORT       8883
#endif
#ifndef MQTT_TOPIC_BASE
#define MQTT_TOPIC_BASE "pcbtn/YOUR_UNIQUE_SECRET_TOPIC"
#endif
#ifndef BLE_AUTH_PIN
#define BLE_AUTH_PIN    "123456"
#endif

#define MS_PRESS        500
#define MS_FORCE        4500
#define MS_RESET        500

#define BLE_SVC_UUID    "7a1e0001-9c3b-4f2a-8d5e-1b2c3d4e5f60"
#define BLE_CH_CFG      "7a1e0002-9c3b-4f2a-8d5e-1b2c3d4e5f60" // 读写配置 JSON
#define BLE_CH_STAT     "7a1e0003-9c3b-4f2a-8d5e-1b2c3d4e5f60" // 状态实时通知
#define BLE_CH_CMD      "7a1e0004-9c3b-4f2a-8d5e-1b2c3d4e5f60" // 快捷指令

struct AppConfig {
  char ssid[33];
  char user[65];
  char pass[65];
  char mqttHost[64];
  uint16_t mqttPort;
  char topic[48];
  char blePin[16];
  uint8_t pinPwr;
  uint8_t pinRst;
  uint8_t pinPled;
  uint8_t pinHdled;
  uint8_t pledInv;
};

static AppConfig cfg;
static Preferences prefs;

static WiFiClientSecure netClient;
static PubSubClient     mqtt(netClient);
static char             clientId[32] = {0};

static BLEServer*        bleServer = nullptr;
static BLECharacteristic* chBleStat = nullptr;
static BLECharacteristic* chBleCfg  = nullptr;
static bool              bleConnected = false;
static String            bleRxBuffer = "";

static uint32_t lastHeartbeatMs = 0;
static uint32_t lastWifiRetryMs = 0;
static uint32_t rebootScheduleMs = 0;

static int      lastPwrState = -1;
static uint32_t hddActCount = 0;
static uint32_t lastHddActMs = 0;
static uint32_t lastHddReportMs = 0;

// ---------- 配置存储与加载 ----------
static void loadConfig() {
  strlcpy(cfg.ssid, WIFI_SSID, sizeof(cfg.ssid));
  strlcpy(cfg.user, EAP_USER, sizeof(cfg.user));
  strlcpy(cfg.pass, EAP_PASS, sizeof(cfg.pass));
  strlcpy(cfg.mqttHost, MQTT_HOST, sizeof(cfg.mqttHost));
  cfg.mqttPort = MQTT_PORT;
  strlcpy(cfg.topic, MQTT_TOPIC_BASE, sizeof(cfg.topic));
  strlcpy(cfg.blePin, BLE_AUTH_PIN, sizeof(cfg.blePin));
  cfg.pinPwr   = 4;
  cfg.pinRst   = 5;
  cfg.pinPled  = 6;
  cfg.pinHdled = 7;
  cfg.pledInv  = 1;

  prefs.begin("pwrbtn", true);
  if (prefs.isKey("cfg_saved")) {
    prefs.getString("ssid", cfg.ssid, sizeof(cfg.ssid));
    prefs.getString("user", cfg.user, sizeof(cfg.user));
    prefs.getString("pass", cfg.pass, sizeof(cfg.pass));
    prefs.getString("host", cfg.mqttHost, sizeof(cfg.mqttHost));
    cfg.mqttPort = prefs.getUShort("port", cfg.mqttPort);
    prefs.getString("topic", cfg.topic, sizeof(cfg.topic));
    prefs.getString("pin", cfg.blePin, sizeof(cfg.blePin));
    cfg.pinPwr   = prefs.getUChar("pwr", cfg.pinPwr);
    cfg.pinRst   = prefs.getUChar("rst", cfg.pinRst);
    cfg.pinPled  = prefs.getUChar("pled", cfg.pinPled);
    cfg.pinHdled = prefs.getUChar("hdled", cfg.pinHdled);
    cfg.pledInv  = prefs.getUChar("inv", cfg.pledInv);
  }
  prefs.end();
}

static void saveConfig() {
  prefs.begin("pwrbtn", false);
  prefs.putBool("cfg_saved", true);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("user", cfg.user);
  prefs.putString("pass", cfg.pass);
  prefs.putString("host", cfg.mqttHost);
  prefs.putUShort("port", cfg.mqttPort);
  prefs.putString("topic", cfg.topic);
  prefs.putString("pin", cfg.blePin);
  prefs.putUChar("pwr", cfg.pinPwr);
  prefs.putUChar("rst", cfg.pinRst);
  prefs.putUChar("pled", cfg.pinPled);
  prefs.putUChar("hdled", cfg.pinHdled);
  prefs.putUChar("inv", cfg.pledInv);
  prefs.end();
}

static void resetConfigDefault() {
  prefs.begin("pwrbtn", false);
  prefs.clear();
  prefs.end();
}

// ---------- 硬件引脚控制 ----------
static void setupOpenDrain(uint8_t pin) {
  if (pin == 0) return;
  gpio_reset_pin((gpio_num_t)pin);
  gpio_set_level((gpio_num_t)pin, 1);
  gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT_OUTPUT_OD);
}

static void triggerPulse(uint8_t pin, uint32_t ms, const char* name) {
  if (pin == 0) return;
  Serial.printf("[ACTION] 触发: %s (%ums)\n", name, ms);
  gpio_set_level((gpio_num_t)pin, 0);
  delay(ms);
  gpio_set_level((gpio_num_t)pin, 1);
  delay(50);
}

// ---------- 状态读取 ----------
static int readPcPower() {
  if (cfg.pinPled == 0) return -1; // 0 表示未接
  int raw = digitalRead(cfg.pinPled) ? 1 : 0;
  return cfg.pledInv ? (1 - raw) : raw;
}

static void buildStateJson(char* out, size_t maxLen, const char* lastAct) {
  int pcOn = readPcPower();
  int diskLit = (cfg.pinHdled != 0 && pcOn == 1 && digitalRead(cfg.pinHdled) == 0) ? 1 : 0;
  uint32_t diskAgo = lastHddActMs ? ((millis() - lastHddActMs) / 1000) : 99999;
  snprintf(out, maxLen,
    "{\"online\":1,\"pc\":%d,\"disk_lit\":%d,\"disk_act\":%lu,\"disk_ago\":%lu,\"rssi\":%d,\"ip\":\"%s\",\"uptime\":%lu,\"last\":\"%s\"}",
    pcOn, diskLit, (unsigned long)hddActCount, (unsigned long)diskAgo,
    WiFi.status() == WL_CONNECTED ? (int)WiFi.RSSI() : 0,
    WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "--",
    (unsigned long)(millis() / 1000), lastAct ? lastAct : "idle");
}

static void publishState(const char* actName) {
  char buf[256];
  buildStateJson(buf, sizeof(buf), actName);

  if (mqtt.connected()) {
    char topicState[80];
    snprintf(topicState, sizeof(topicState), "%s/state", cfg.topic);
    mqtt.publish(topicState, buf, true);
  }

  if (chBleStat && bleConnected) {
    chBleStat->setValue((uint8_t*)buf, strlen(buf));
    chBleStat->notify();
  }

  lastHeartbeatMs = millis();
}

static void executeCommand(const char* cmd) {
  String c = String(cmd);
  c.trim();
  c.toLowerCase();

  if (c == "force" || c == "f") {
    triggerPulse(cfg.pinPwr, MS_FORCE, "长按强制关机");
    publishState("force_off");
  } else if (c == "reset" || c == "r") {
    triggerPulse(cfg.pinRst, MS_RESET, "复位硬重启");
    publishState("reset");
  } else if (c == "reboot") {
    Serial.println("[SYSTEM] 收到重启指令，3秒后重启...");
    rebootScheduleMs = millis() + 3000;
  } else if (c == "status") {
    publishState("status_query");
  } else {
    triggerPulse(cfg.pinPwr, MS_PRESS, "短按电源键");
    publishState("power_press");
  }
}

// ---------- MQTT 回调与维护 ----------
static void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  char cmd[32] = {0};
  unsigned int copyLen = (length < sizeof(cmd) - 1) ? length : (sizeof(cmd) - 1);
  memcpy(cmd, payload, copyLen);
  Serial.printf("[MQTT] 收到: %s\n", cmd);
  executeCommand(cmd);
}

static void connectWiFi() {
  Serial.printf("[WiFi] 正在连接网络 %s (用户: %s)...\n", cfg.ssid, cfg.user);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  if (strlen(cfg.user) > 0) {
    WiFi.begin(cfg.ssid, WPA2_AUTH_PEAP, "", cfg.user, cfg.pass);
  } else {
    WiFi.begin(cfg.ssid, cfg.pass);
  }
}

static void ensureConnections() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiRetryMs > 15000) {
      lastWifiRetryMs = millis();
      connectWiFi();
    }
    return;
  }

  if (!mqtt.connected()) {
    static uint32_t lastRetry = 0;
    if (millis() - lastRetry < 5000) return;
    lastRetry = millis();

    Serial.printf("[MQTT] 连接 %s:%u ... ", cfg.mqttHost, cfg.mqttPort);
    char topicState[80], topicSet[80];
    snprintf(topicState, sizeof(topicState), "%s/state", cfg.topic);
    snprintf(topicSet, sizeof(topicSet), "%s/set", cfg.topic);

    if (mqtt.connect(clientId, topicState, 1, true, "{\"online\":0}")) {
      Serial.println("成功！");
      mqtt.subscribe(topicSet);
      publishState("boot");
    } else {
      Serial.printf("失败 rc=%d\n", mqtt.state());
    }
  }
}

// ---------- 蓝牙服务与全量 JSON 参数解析 ----------
static bool extractJsonValue(const String& json, const char* key, char* out, size_t maxLen) {
  String pattern = String("\"") + key + "\"";
  int idx = json.indexOf(pattern);
  if (idx < 0) return false;
  idx = json.indexOf(':', idx + pattern.length());
  if (idx < 0) return false;
  idx++;
  while (idx < (int)json.length() && (json[idx] == ' ' || json[idx] == '\t')) idx++;
  if (idx >= (int)json.length()) return false;

  if (json[idx] == '"') {
    int endIdx = json.indexOf('"', idx + 1);
    if (endIdx < 0) return false;
    strlcpy(out, json.substring(idx + 1, endIdx).c_str(), maxLen);
    return true;
  } else {
    int endIdx = idx;
    while (endIdx < (int)json.length() && (isDigit(json[endIdx]) || json[endIdx] == '.' || json[endIdx] == '-')) endIdx++;
    strlcpy(out, json.substring(idx, endIdx).c_str(), maxLen);
    return true;
  }
}

static void updateBleConfigCharacteristic() {
  if (!chBleCfg) return;
  char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"ssid\":\"%s\",\"user\":\"%s\",\"host\":\"%s\",\"port\":%u,\"topic\":\"%s\",\"pwr\":%u,\"rst\":%u,\"pled\":%u,\"hdled\":%u,\"inv\":%u}",
    cfg.ssid, cfg.user, cfg.mqttHost, cfg.mqttPort, cfg.topic,
    cfg.pinPwr, cfg.pinRst, cfg.pinPled, cfg.pinHdled, cfg.pledInv);
  chBleCfg->setValue((uint8_t*)buf, strlen(buf));
}

class BleServerCallbacksImpl : public BLEServerCallbacks {
  void onConnect(BLEServer*) {
    bleConnected = true;
    Serial.println("[BLE] 手机已连接");
    updateBleConfigCharacteristic();
    publishState("ble_connect");
  }
  void onDisconnect(BLEServer*) {
    bleConnected = false;
    Serial.println("[BLE] 手机已断开");
    BLEDevice::startAdvertising();
  }
};

class BleCmdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) {
    String val = c->getValue().c_str();
    Serial.printf("[BLE CMD] %s\n", val.c_str());
    executeCommand(val.c_str());
  }
};

class BleCfgCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) {
    bleRxBuffer += String(c->getValue().c_str());
    if (bleRxBuffer.indexOf('}') >= 0) {
      String json = bleRxBuffer;
      bleRxBuffer = "";
      Serial.printf("[BLE CFG RECV] %s\n", json.c_str());

      char pinBuf[32] = {0};
      if (!extractJsonValue(json, "pin", pinBuf, sizeof(pinBuf)) || strcmp(pinBuf, cfg.blePin) != 0) {
        Serial.println("[BLE] 配置被拒：安全口令 PIN 错误！");
        return;
      }

      char tmp[65] = {0};
      if (extractJsonValue(json, "ssid", tmp, sizeof(tmp))) strlcpy(cfg.ssid, tmp, sizeof(cfg.ssid));
      if (extractJsonValue(json, "user", tmp, sizeof(tmp))) strlcpy(cfg.user, tmp, sizeof(cfg.user));
      if (extractJsonValue(json, "pass", tmp, sizeof(tmp))) strlcpy(cfg.pass, tmp, sizeof(cfg.pass));
      if (extractJsonValue(json, "host", tmp, sizeof(tmp))) strlcpy(cfg.mqttHost, tmp, sizeof(cfg.mqttHost));
      if (extractJsonValue(json, "topic", tmp, sizeof(tmp))) strlcpy(cfg.topic, tmp, sizeof(cfg.topic));
      if (extractJsonValue(json, "new_pin", tmp, sizeof(tmp))) strlcpy(cfg.blePin, tmp, sizeof(cfg.blePin));
      if (extractJsonValue(json, "port", tmp, sizeof(tmp))) cfg.mqttPort = (uint16_t)atoi(tmp);

      // 引脚全量修改支持
      if (extractJsonValue(json, "pwr", tmp, sizeof(tmp))) cfg.pinPwr = (uint8_t)atoi(tmp);
      if (extractJsonValue(json, "rst", tmp, sizeof(tmp))) cfg.pinRst = (uint8_t)atoi(tmp);
      if (extractJsonValue(json, "pled", tmp, sizeof(tmp))) cfg.pinPled = (uint8_t)atoi(tmp);
      if (extractJsonValue(json, "hdled", tmp, sizeof(tmp))) cfg.pinHdled = (uint8_t)atoi(tmp);
      if (extractJsonValue(json, "inv", tmp, sizeof(tmp))) cfg.pledInv = (uint8_t)atoi(tmp);

      // 恢复出厂默认支持
      if (extractJsonValue(json, "reset_default", tmp, sizeof(tmp)) && atoi(tmp) == 1) {
        Serial.println("[BLE] 收到恢复出厂指令，清空 NVS...");
        resetConfigDefault();
      } else {
        saveConfig();
        Serial.println("[BLE] 配置已保存到 NVS！");
      }

      updateBleConfigCharacteristic();
      rebootScheduleMs = millis() + 2500;
      Serial.println("[BLE] 2.5 秒后自动重启生效！");
    }
  }
};

static void setupBLE() {
  BLEDevice::init("pc_pwrbtn");
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new BleServerCallbacksImpl());

  BLEService* svc = bleServer->createService(BLE_SVC_UUID);

  chBleCfg = svc->createCharacteristic(
    BLE_CH_CFG,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
  );
  chBleCfg->setCallbacks(new BleCfgCallbacks());

  chBleStat = svc->createCharacteristic(
    BLE_CH_STAT,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  chBleStat->addDescriptor(new BLE2902());

  BLECharacteristic* chCmd = svc->createCharacteristic(
    BLE_CH_CMD,
    BLECharacteristic::PROPERTY_WRITE
  );
  chCmd->setCallbacks(new BleCmdCallbacks());

  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SVC_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.printf("[BLE] 蓝牙就绪: pc_pwrbtn (PIN: %s)\n", cfg.blePin);
}

// ---------- 硬件指示灯轮询 ----------
static void pollLeds() {
  // 1. 电源指示灯 (PLED+ 高有效)
  int currentPwr = readPcPower();
  if (currentPwr != lastPwrState) {
    lastPwrState = currentPwr;
    Serial.printf("[LED] 电脑电源状态变为: %s\n", currentPwr == 1 ? "🟢 开机" : (currentPwr == 0 ? "⚪ 关机" : "未接"));
    publishState(currentPwr == 1 ? "pc_on" : "pc_off");
  }

  // 2. 硬盘指示灯 (实时电平镜像：HDLED- 0V 为亮，3.3V 为灭)
  if (cfg.pinHdled != 0) {
    static int lastDiskLit = -1;
    static uint32_t lastLitReportMs = 0;
    int pcOn = readPcPower();
    int currentLit = (pcOn == 1 && digitalRead(cfg.pinHdled) == 0) ? 1 : 0;
    if (currentLit != lastDiskLit) {
      if (millis() - lastLitReportMs > 150) {
        lastLitReportMs = millis();
        lastDiskLit = currentLit;
        if (currentLit == 1) {
          hddActCount++;
          lastHddActMs = millis();
        }
        publishState(currentLit == 1 ? "disk_lit" : "disk_off");
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  loadConfig();

  // 硬件开漏初始化
  setupOpenDrain(cfg.pinPwr);
  setupOpenDrain(cfg.pinRst);

  // 指示灯引脚配置：PLED 启用内部弱下拉（防止未插线时悬空浮动误判为开机）
  // PLED- 负极模式：启用内部弱上拉，开机接地为 0V，关机浮高为 3.3V
  if (cfg.pinPled != 0) pinMode(cfg.pinPled, INPUT_PULLUP);
  // 存储活动灯启用内部上拉（平时高电平，读盘接地触发）
  if (cfg.pinHdled != 0) pinMode(cfg.pinHdled, INPUT_PULLUP);

  uint64_t mac = ESP.getEfuseMac();
  snprintf(clientId, sizeof(clientId), "esp32s3_%04X", (uint16_t)(mac >> 32));

  netClient.setInsecure();
  mqtt.setServer(cfg.mqttHost, cfg.mqttPort);
  mqtt.setCallback(onMqttMessage);
  mqtt.setKeepAlive(30);

  setupBLE();
  connectWiFi();

  Serial.println("\n=============================================");
  Serial.println(" ESP32-S3 全功能远程控制器 (MQTT + BLE + LED)");
  Serial.println("=============================================");
}

void loop() {
  if (rebootScheduleMs && millis() > rebootScheduleMs) {
    ESP.restart();
  }

  pollLeds();
  ensureConnections();

  if (mqtt.connected()) {
    mqtt.loop();
    if (millis() - lastHeartbeatMs > 30000) {
      publishState("heartbeat");
    }
  }

  if (Serial.available()) {
    char c = tolower(Serial.read());
    if (c == 'p') executeCommand("press");
    if (c == 'f') executeCommand("force");
    if (c == 'r') executeCommand("reset");
    if (c == 's') publishState("serial_status");
  }

  delay(5);
}
