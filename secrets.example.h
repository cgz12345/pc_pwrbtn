#pragma once
// ============================================================================
//  复制本文件为 secrets.h，填上你自己的值。secrets.h 不会被提交到 git。
//  填完烧录，设备开机即可联网 / 连 MQTT —— 不需要网页、不需要配网。
// ============================================================================

// ---- 校园网 / WiFi ----
#define DEF_WIFI_SSID   "你的WiFi名称"
#define DEF_WIFI_AUTH   0                     // 0=WPA2-企业(PEAP) 1=WPA2-个人 2=开放
#define DEF_EAP_IDENT   ""                    // 匿名身份，多数留空
#define DEF_EAP_USER    "账号"
#define DEF_EAP_PASS    "密码"

// ---- MQTT ----
#define DEF_MQTT_HOST   "broker.emqx.io"       // 自建 broker 改成自己的域名/IP
#define DEF_MQTT_PORT   8883
#define DEF_MQTT_TLS    1                      // 1=TLS(8883) 0=明文(1883)
#define DEF_MQTT_USER   ""
#define DEF_MQTT_PASS   ""
#define DEF_TOPIC       "pcbtn/改成别人猜不到的后缀"   // 主题前缀（等于密码）

// ---- 引脚（0 = 不接）----
#define DEF_PIN_PWR     5                      // 电源键（开漏 → PWR_SW）
#define DEF_PIN_RST     0                      // 复位键（开漏 → RESET_SW）
#define DEF_PIN_VSEN    0                      // 开机侦测（电源灯/SATA 5V 经分压）
#define DEF_PIN_ACT     0                      // 硬盘活动侦测（硬盘灯经分压）
#define DEF_VSEN_INV    0                      // 0=高电平表示开机

// ---- 蓝牙改配置口令 ----
#define DEF_BLE_PIN     "123456"
