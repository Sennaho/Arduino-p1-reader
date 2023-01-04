#include <ArduinoMqttClient.h>
#if defined(ARDUINO_SAMD_MKRWIFI1010) || defined(ARDUINO_SAMD_NANO_33_IOT) || defined(ARDUINO_AVR_UNO_WIFI_REV2)
#include <WiFiNINA.h>
#elif defined(ARDUINO_SAMD_MKR1000)
#include <WiFi101.h>
#elif defined(ARDUINO_ARCH_ESP8266)
#include <ESP8266WiFi.h>
#elif defined(ARDUINO_PORTENTA_H7_M7) || defined(ARDUINO_NICLA_VISION) || defined(ARDUINO_ARCH_ESP32)
#include <WiFi.h>
#endif

#include "secrets.h"
#include <Arduino_JSON.h>
#include <Adafruit_SleepyDog.h>

#define BUFFER_SIZE 1050
char ssid[] = SECRET_SSID;  // your network SSID (name)
char pass[] = SECRET_PASS;  // your network password (use for WPA, or use as key for WEP)

WiFiClient wifiClient;

// MQTT
const char broker[] = MQTT_BROKER;
const char topic[] = MQTT_TOPIC;
const char mqtt_users[] = MQTT_USER;
const char mqtt_pass[] = MQTT_PASS;
int port = MQTT_PORT;
MqttClient mqttClient(wifiClient);
//JSON
JSONVar jsonObject;

// * Set during CRC checking
unsigned int currentCRC = 0;
// Serial
char telegram[BUFFER_SIZE];

const long interval = 60000;
unsigned long previousMillis = 0;


void setup() {
  mqttClient.setUsernamePassword(mqtt_users, mqtt_pass);

  // Serial
  SerialUSB.begin(9600);              // Initialize Serial Monitor USB
  Serial1.begin(115200, SERIAL_8N1);  // Initialize hardware serial port, pins 0/1
  Serial1.flush();
  Serial1.setTimeout(5000);

  checkConnToWiFiAndMqtt();
  Watchdog.enable(16000);
  SerialUSB.println("Startup complete");
}


void checkConnToWiFiAndMqtt() {
  if (WiFi.status() != WL_CONNECTED) {
    SerialUSB.print("Attempting to connect to WPA SSID: ");
    SerialUSB.println(ssid);
    while (WiFi.status() != WL_CONNECTED) {
      WiFi.begin(ssid, pass);
      SerialUSB.print(".");
      delay(3000);
    }
    SerialUSB.println("You're connected to the network");
    SerialUSB.println();
  }

  if (!mqttClient.connected()) {
    SerialUSB.print("Attempting to connect to the MQTT broker: ");
    SerialUSB.println(broker);
    while (!mqttClient.connect(broker, port)) {
      SerialUSB.print("MQTT connection failed! Error code = ");
      SerialUSB.println(mqttClient.connectError());
      delay(3000);
    }
  }
}

void loop() {
  // Check connection to wifi and mqtt every minute.
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= interval) {
    checkConnToWiFiAndMqtt();
  }
  read_p1_hardwareserial();
  
}

void read_p1_hardwareserial() {
  if (Serial1.available()) {
    Serial.println("Serial available");
    Watchdog.reset();
    while (Serial1.available()) {
      int len = Serial1.readBytesUntil('\n', telegram, BUFFER_SIZE);
      processLine(len);
    }
  }
}

void processLine(int len) {
  telegram[len] = '\n';
  telegram[len + 1] = 0;
  bool result = decode_telegram(len + 1);
  if (result) {
    sendMqttMsg();
  }
}

void sendMqttMsg() {
  String jsonString = JSON.stringify(jsonObject);
  mqttClient.beginMessage(topic, jsonString.length(), false, 0, false);
  mqttClient.print(jsonString);
  mqttClient.endMessage();
  Serial.println(jsonString);
}

// parsing of telegram according to Swedish ESMR 5.0 implementation //UKR 1220
bool decode_telegram(int len) {

  // Check crc
  int startChar = FindCharInArrayRev(telegram, '/', len);
  int endChar = FindCharInArrayRev(telegram, '!', len);
  bool validCRCFound = false;

  for (int cnt = 0; cnt < len; cnt++) {
    SerialUSB.print(telegram[cnt]);
  }

  if (startChar >= 0) {
    // * Start found. Reset CRC calculation
    currentCRC = CRC16(0x0000, (unsigned char *)telegram + startChar, len - startChar);
  } else if (endChar >= 0) {
    // * Add to crc calc
    currentCRC = CRC16(currentCRC, (unsigned char *)telegram + endChar, 1);

    char messageCRC[5];
    strncpy(messageCRC, telegram + endChar + 1, 4);

    messageCRC[4] = 0;  // * Thanks to HarmOtten (issue 5)
    validCRCFound = (strtol(messageCRC, NULL, 16) == currentCRC);

    if (validCRCFound)
      Serial.println(F("CRC Valid!"));
    else
      Serial.println(F("CRC Invalid!"));

    currentCRC = 0;
  } else {
    currentCRC = CRC16(currentCRC, (unsigned char *)telegram, len);
  }

  // Parse values

  // 1-0:1.8.0(000992.992*kWh)
  // 1-0:1.8.0 = Cumulative hourly active import energy (A+) (Q1+Q4)
  if (strncmp(telegram, "1-0:1.8.0", 9) == 0) {
    jsonObject["CONSUMPTION"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:2.8.0(000560.157*kWh)
  // 1-0:2.8.0 = Cumulative hourly active export energy (A-) (Q2+Q3)
  else if (strncmp(telegram, "1-0:2.8.0", 9) == 0) {
    jsonObject["RETURNDELIVERY"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:3.8.0(000560.157*kWh)
  // 1-0:3.8.0 = Cumulative hourly reactive import energy (R+) (Q1+Q2)
  else if (strncmp(telegram, "1-0:3.8.0", 9) == 0) {
    jsonObject["CONSUMPTION_REACT"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:4.8.0(000560.157*kWh)
  // 1-0:4.8.0 = Cumulative hourly reactive export energy (R-) (Q3+Q4)
  else if (strncmp(telegram, "1-0:4.8.0", 9) == 0) {
    jsonObject["RETURNDELIVERY_REACT"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:1.7.0(00.424*kW)
  // 1-0:1.7.x = Momentary Active power+ (Q1+Q4)
  else if (strncmp(telegram, "1-0:1.7.0", 9) == 0) {
    jsonObject["ACTUAL_CONSUMPTION"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:2.7.0(00.000*kW)
  // 1-0:2.7.x = Momentary Active power- (Q2+Q3)
  else if (strncmp(telegram, "1-0:2.7.0", 9) == 0) {
    jsonObject["ACTUAL_RETURNDELIVERY"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:3.7.0(00.424*kW)
  // 1-0:3.7.x = Momentary Reactive power + ( Q1+Q2)
  else if (strncmp(telegram, "1-0:3.7.0", 9) == 0) {
    jsonObject["ACTUAL_CONSUMPTION_REACT"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:4.7.0(00.000*kW)
  // 1-0:4.7.x = Momentary Reactive power - ( Q3+Q4)
  else if (strncmp(telegram, "1-0:4.7.0", 9) == 0) {
    jsonObject["ACTUAL_RETURNDELIVERY_REACT"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:21.7.0(00.378*kW)
  // 1-0:21.7.0 = Momentary Active power+ (L1)
  else if (strncmp(telegram, "1-0:21.7.0", 10) == 0) {
    jsonObject["L1_INSTANT_POWER_USAGE"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:22.7.0(00.378*kW)
  // 1-0:22.7.0 = Momentary Active power- (L1)
  else if (strncmp(telegram, "1-0:22.7.0", 10) == 0) {
    jsonObject["L1_INSTANT_POWER_DELIVERY"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:41.7.0(00.378*kW)
  // 1-0:41.7.0 = Momentary Active power+ (L2)
  else if (strncmp(telegram, "1-0:41.7.0", 10) == 0) {
    jsonObject["L2_INSTANT_POWER_USAGE"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:42.7.0(00.378*kW)
  // 1-0:42.7.0 = Momentary Active power- (L2)
  else if (strncmp(telegram, "1-0:42.7.0", 10) == 0) {
    jsonObject["L2_INSTANT_POWER_DELIVERY"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:61.7.0(00.378*kW)
  // 1-0:61.7.0 = Momentary Active power+ (L3)
  else if (strncmp(telegram, "1-0:61.7.0", 10) == 0) {
    jsonObject["L3_INSTANT_POWER_USAGE"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:62.7.0(00.378*kW)
  // 1-0:62.7.0 = Momentary Active power- (L3)
  else if (strncmp(telegram, "1-0:62.7.0", 10) == 0) {
    jsonObject["L3_INSTANT_POWER_DELIVERY"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:23.7.0(00.378*kW)
  // 1-0:23.7.0 = Momentary Reactive power+ (L1)
  else if (strncmp(telegram, "1-0:23.7.0", 10) == 0) {
    jsonObject["L1_REACT_POWER_USAGE"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:24.7.0(00.378*kW)
  // 1-0:24.7.0 = Momentary Reactive power- (L1)
  else if (strncmp(telegram, "1-0:24.7.0", 10) == 0) {
    jsonObject["L1_REACT_POWER_DELIVERY"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:43.7.0(00.378*kW)
  // 1-0:43.7.0 = Momentary Reactive power+ (L2)
  else if (strncmp(telegram, "1-0:43.7.0", 10) == 0) {
    jsonObject["L2_REACT_POWER_USAGE"] = getValue(telegram, len, '(', '*');
    ;
  }

  // 1-0:44.7.0(00.378*kW)
  // 1-0:44.7.0 = Momentary Reactive power+ (L2)
  else if (strncmp(telegram, "1-0:44.7.0", 10) == 0) {
    jsonObject["L2_REACT_POWER_DELIVERY"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:63.7.0(00.378*kW)
  // 1-0:63.7.0 = Momentary Reactive power+ (L3)
  else if (strncmp(telegram, "1-0:63.7.0", 10) == 0) {
    jsonObject["L3_REACT_POWER_USAGE"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:64.7.0(00.378*kW)
  // 1-0:64.7.0 = Momentary Reactive power- (L3)
  else if (strncmp(telegram, "1-0:64.7.0", 10) == 0) {
    jsonObject["L3_REACT_POWER_DELIVERY"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:31.7.0(002*A)
  // 1-0:31.7.0 = Momentary RMS Current phase L1
  else if (strncmp(telegram, "1-0:31.7.0", 10) == 0) {
    jsonObject["L1_INSTANT_POWER_CURRENT"] = getValue(telegram, len, '(', '*');
  }
  // 1-0:51.7.0(002*A)
  // 1-0:51.7.0 = Momentary RMS Current phase L2
  else if (strncmp(telegram, "1-0:51.7.0", 10) == 0) {
    jsonObject["L2_INSTANT_POWER_CURRENT"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:71.7.0(002*A)
  // 1-0:71.7.0 = Momentary RMS Current phase L3
  else if (strncmp(telegram, "1-0:71.7.0", 10) == 0) {
    jsonObject["L3_INSTANT_POWER_CURRENT"] = getValue(telegram, len, '(', '*');
  }

  // 1-0:32.7.0(232.0*V)
  // 1-0:32.7.0 = Momentary RMS Phase voltage L1
  else if (strncmp(telegram, "1-0:32.7.0", 10) == 0) {
    jsonObject["L1_VOLTAGE"] = getValue(telegram, len, '(', '*');
  }
  // 1-0:52.7.0(232.0*V)
  // 1-0:52.7.0 = Momentary RMS Phase voltage L2
  else if (strncmp(telegram, "1-0:52.7.0", 10) == 0) {
    jsonObject["L2_VOLTAGE"] = getValue(telegram, len, '(', '*');
  }
  // 1-0:72.7.0(232.0*V)
  // 1-0:72.7.0 = Momentary RMS Phase voltage L3
  else if (strncmp(telegram, "1-0:72.7.0", 10) == 0) {
    jsonObject["L3_VOLTAGE"] = getValue(telegram, len, '(', '*');
  }

  return validCRCFound;
}

double getValue(char *buffer, int maxlen, char startchar, char endchar) {
  int s = FindCharInArrayRev(buffer, startchar, maxlen - 2);
  int l = FindCharInArrayRev(buffer, endchar, maxlen - 2) - s - 1;

  char res[16];
  memset(res, 0, sizeof(res));

  if (strncpy(res, buffer + s + 1, l)) {
    if (endchar == '*') {
      if (isNumber(res, l))
        // * Lazy convert float to long
        return atof(res);
    } else if (endchar == ')') {
      if (isNumber(res, l))
        return atof(res);
    }
  }
  return 0;
}

bool isNumber(char *res, int len) {
  for (int i = 0; i < len; i++) {
    if (((res[i] < '0') || (res[i] > '9')) && (res[i] != '.' && res[i] != 0))
      return false;
  }
  return true;
}

unsigned int CRC16(unsigned int crc, unsigned char *buf, int len) {
  for (int pos = 0; pos < len; pos++) {
    crc ^= (unsigned int)buf[pos];  // * XOR byte into least sig. byte of crc
                                    // * Loop over each bit
    for (int i = 8; i != 0; i--) {
      // * If the LSB is set
      if ((crc & 0x0001) != 0) {
        // * Shift right and XOR 0xA001
        crc >>= 1;
        crc ^= 0xA001;
      }
      // * Else LSB is not set
      else
        // * Just shift right
        crc >>= 1;
    }
  }
  return crc;
}

int FindCharInArrayRev(char array[], char c, int len) {
  for (int i = len - 1; i >= 0; i--) {
    if (array[i] == c)
      return i;
  }
  return -1;
}
