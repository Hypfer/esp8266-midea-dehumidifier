#include "types.h"
#include "wifi.h"

#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
//#include <SoftwareSerial.h>
#include <ArduinoOTA.h>
#include <FS.h>

dehumidifierState_t state;

byte serialRxBuf[255];
byte serialTxBuf[255];
uint8_t mqttRetryCounter = 0;


WiFiManager wifiManager;
WiFiClient wifiClient;
PubSubClient mqttClient;

WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, sizeof(mqtt_server));
WiFiManagerParameter custom_mqtt_user("user", "MQTT username", username, sizeof(username));
WiFiManagerParameter custom_mqtt_pass("pass", "MQTT password", password, sizeof(password));

unsigned long lastMqttConnectionAttempt = millis();
const long mqttConnectionInterval = 60000;

unsigned long statusPollPreviousMillis = millis();
const long statusPollInterval = 30000;

char identifier[24];
#define FIRMWARE_PREFIX "esp8266-midea-dehumidifier"
#define AVAILABILITY_ONLINE "online"
#define AVAILABILITY_OFFLINE "offline"
char MQTT_TOPIC_AVAILABILITY[128];
char MQTT_TOPIC_STATE[128];
char MQTT_TOPIC_COMMAND[128];

char MQTT_TOPIC_AUTOCONF_WIFI_SENSOR[128];
char MQTT_TOPIC_AUTOCONF_HUMIDITY_SENSOR[128];


char MQTT_TOPIC_AUTOCONF_FAN[128];
char MQTT_TOPIC_AUTOCONF_HUMIDIFIER[128];
char MQTT_TOPIC_AUTOCONF_IONISER[128];




bool shouldSaveConfig = false;

void saveConfigCallback () {
  shouldSaveConfig = true;
}

void setup() {
  Serial.begin(9600);

  //Initializing with defaults until we have a status update
  state.powerOn = false;
  state.fanSpeed = low;
  state.mode = setpoint;
  state.humiditySetpoint = 55;
  state.currentHumidity = 45;
  state.ion = false;
  state.errorCode = 0;

  delay(3000);
  updateAndSendNetworkStatus(false);


  snprintf(identifier, sizeof(identifier), "DEHUMIDIFIER-%X", ESP.getChipId());
  snprintf(MQTT_TOPIC_AVAILABILITY, 127, "%s/%s/status", FIRMWARE_PREFIX, identifier);
  snprintf(MQTT_TOPIC_STATE, 127, "%s/%s/state", FIRMWARE_PREFIX, identifier);
  snprintf(MQTT_TOPIC_COMMAND, 127, "%s/%s/command", FIRMWARE_PREFIX, identifier);

  snprintf(MQTT_TOPIC_AUTOCONF_HUMIDITY_SENSOR, 127, "homeassistant/sensor/%s/%s_humidity/config", FIRMWARE_PREFIX, identifier);
  snprintf(MQTT_TOPIC_AUTOCONF_WIFI_SENSOR, 127, "homeassistant/sensor/%s/%s_wifi/config", FIRMWARE_PREFIX, identifier);

  snprintf(MQTT_TOPIC_AUTOCONF_FAN, 127, "homeassistant/fan/%s/%s_fan/config", FIRMWARE_PREFIX, identifier);
  snprintf(MQTT_TOPIC_AUTOCONF_HUMIDIFIER, 127, "homeassistant/humidifier/%s/%s_dehumidifier/config", FIRMWARE_PREFIX, identifier);
  snprintf(MQTT_TOPIC_AUTOCONF_IONISER, 127, "homeassistant/switch/%s/%s_ioniser/config", FIRMWARE_PREFIX, identifier);

  WiFi.hostname(identifier);

  loadConfig();

  setupWifi();
  setupOTA();
  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setKeepAlive(10);
  mqttClient.setBufferSize(2048);
  mqttClient.setCallback(mqttCallback);

  mqttReconnect();
}

void setupOTA() {

  ArduinoOTA.onStart([]() {
    //Serial.println("Start");
  });
  ArduinoOTA.onEnd([]() {
    //Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    //Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    /*
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    */
  });

  ArduinoOTA.setHostname(identifier);

  //This is less of a security measure and more a accidential flash prevention
  ArduinoOTA.setPassword(identifier);
  ArduinoOTA.begin();
}


void loop() {
  ArduinoOTA.handle();
  handleUart();
  mqttClient.loop();

  if (statusPollInterval <= (millis() - statusPollPreviousMillis)) {
    statusPollPreviousMillis = millis();
    getStatus();
  }

  if (!mqttClient.connected() && (mqttConnectionInterval <= (millis() - lastMqttConnectionAttempt)) )  {
    lastMqttConnectionAttempt = millis();
    mqttReconnect();
  }
}

void setupWifi() {
  wifiManager.setDebugOutput(false);
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_user);
  wifiManager.addParameter(&custom_mqtt_pass);

  WiFi.hostname(identifier);
  wifiManager.autoConnect(identifier);
  mqttClient.setClient(wifiClient);

  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(username, custom_mqtt_user.getValue());
  strcpy(password, custom_mqtt_pass.getValue());

  if (shouldSaveConfig) {
    saveConfig();
  } else {
    //For some reason, the read values get overwritten in this function
    //To combat this, we just reload the config
    //This is most likely a logic error which could be fixed otherwise
    loadConfig();
  }
}

void resetWifiSettingsAndReboot() {
  wifiManager.resetSettings();
  updateAndSendNetworkStatus(false);
  delay(3000);
  ESP.restart();
}

void mqttReconnect()
{
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (mqttClient.connect(identifier, username, password, MQTT_TOPIC_AVAILABILITY, 1, true, AVAILABILITY_OFFLINE)) {
      mqttClient.publish(MQTT_TOPIC_AVAILABILITY, AVAILABILITY_ONLINE, true);
      updateAndSendNetworkStatus(true);
      getStatus();
      publishAutoConfig();

      //Make sure to subscribe after polling the status so that we never execute commands with the default data
      mqttClient.subscribe(MQTT_TOPIC_COMMAND);
      break;
    } else {
      delay(5000);
    }
  }
}

boolean isMqttConnected() {
  return mqttClient.connected();
}

void publishState() {
  DynamicJsonDocument wifiJson(192);
  DynamicJsonDocument stateJson(604);
  char payload[256];

  wifiJson["ssid"] = WiFi.SSID();
  wifiJson["ip"] = WiFi.localIP().toString();
  wifiJson["rssi"] = WiFi.RSSI();

  stateJson["state"] = state.powerOn ? "on" : "off";
  stateJson["humiditySetpoint"] = state.humiditySetpoint;
  stateJson["humidityCurrent"] = state.currentHumidity;
  stateJson["ion"] = state.ion ? "on" : "off";
  stateJson["errorCode"] = state.errorCode;

  switch (state.fanSpeed) {
    case low:
      stateJson["fanSpeed"] = "low";
      break;
    case medium:
      stateJson["fanSpeed"] = "medium";
      break;
    case high:
      stateJson["fanSpeed"] = "high";
      break;
  }

  switch (state.mode) {
    case setpoint:
      stateJson["mode"] = "setpoint";
      break;
    case continuous:
      stateJson["mode"] = "continuous";
      break;
    case smart:
      stateJson["mode"] = "smart";
      break;
    case clothesDrying:
      stateJson["mode"] = "clothesDrying";
      break;
  }

  stateJson["wifi"] = wifiJson.as<JsonObject>();

  serializeJson(stateJson, payload);
  mqttClient.publish(MQTT_TOPIC_STATE, payload, true);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, MQTT_TOPIC_COMMAND) == 0) {
    DynamicJsonDocument commandJson(256);
    char payloadText[length + 1];

    snprintf(payloadText, length + 1, "%s", payload);

    DeserializationError err = deserializeJson(commandJson, payloadText);

    if (!err) {
      handleStateUpdateRequest(
        commandJson["state"].as<String>(),
        commandJson["mode"].as<String>(),
        commandJson["fanSpeed"].as<String>(),
        commandJson["humiditySetpoint"].as<byte>(),
        commandJson["ion"].as<String>()
      );

      getStatus();
    }
  }
}

void publishAutoConfig() {
  char mqttPayload[2048];
  DynamicJsonDocument device(256);
  DynamicJsonDocument autoconfPayload(1024);
  StaticJsonDocument<64> identifiersDoc;
  JsonArray identifiers = identifiersDoc.to<JsonArray>();

  identifiers.add(identifier);

  device["identifiers"] = identifiers;
  device["manufacturer"] = "Midea Group Co., Ltd.";
  device["model"] = "Generic Dehumidifier";
  device["name"] = identifier;
  device["sw_version"] = "2021.08.0";


  autoconfPayload["device"] = device.as<JsonObject>();
  autoconfPayload["availability_topic"] = MQTT_TOPIC_AVAILABILITY;
  autoconfPayload["state_topic"] = MQTT_TOPIC_STATE;
  autoconfPayload["name"] = identifier + String(" WiFi");
  autoconfPayload["value_template"] = "{{value_json.wifi.rssi}}";
  autoconfPayload["unique_id"] = identifier + String("_wifi");
  autoconfPayload["unit_of_measurement"] = "dBm";
  autoconfPayload["json_attributes_topic"] = MQTT_TOPIC_STATE;
  autoconfPayload["json_attributes_template"] = "{\"ssid\": \"{{value_json.wifi.ssid}}\", \"ip\": \"{{value_json.wifi.ip}}\"}";
  autoconfPayload["icon"] = "mdi:wifi";

  serializeJson(autoconfPayload, mqttPayload);
  mqttClient.publish(MQTT_TOPIC_AUTOCONF_WIFI_SENSOR, mqttPayload, true);

  autoconfPayload.clear();


  autoconfPayload["device"] = device.as<JsonObject>();
  autoconfPayload["availability_topic"] = MQTT_TOPIC_AVAILABILITY;
  autoconfPayload["state_topic"] = MQTT_TOPIC_STATE;
  autoconfPayload["name"] = identifier + String(" Humidity");
  autoconfPayload["device_class"] = "humidity";
  autoconfPayload["unit_of_measurement"] = "%";
  autoconfPayload["value_template"] = "{{value_json.humidityCurrent}}";
  autoconfPayload["unique_id"] = identifier + String("_humidity");

  serializeJson(autoconfPayload, mqttPayload);
  mqttClient.publish(MQTT_TOPIC_AUTOCONF_HUMIDITY_SENSOR, mqttPayload, true);

  autoconfPayload.clear();


  StaticJsonDocument<64> speedsDoc;
  JsonArray speeds = speedsDoc.to<JsonArray>();

  speeds.add("low");
  speeds.add("medium");
  speeds.add("high");

  autoconfPayload["device"] = device.as<JsonObject>();
  autoconfPayload["availability_topic"] = MQTT_TOPIC_AVAILABILITY;
  autoconfPayload["name"] = identifier + String(" Fan");
  autoconfPayload["unique_id"] = identifier + String("_fan");

  autoconfPayload["state_topic"] = MQTT_TOPIC_STATE;
  autoconfPayload["command_topic"] = MQTT_TOPIC_COMMAND;

  autoconfPayload["preset_modes"] = speeds;
  autoconfPayload["preset_mode_state_topic"] = MQTT_TOPIC_STATE;
  autoconfPayload["preset_mode_command_topic"] = MQTT_TOPIC_COMMAND;
  autoconfPayload["preset_mode_value_template"] = "{{value_json.fanSpeed}}";
  autoconfPayload["preset_mode_command_template"] = "{\"fanSpeed\": \"{{value}}\"}";

  serializeJson(autoconfPayload, mqttPayload);
  mqttClient.publish(MQTT_TOPIC_AUTOCONF_FAN, mqttPayload, true);

  autoconfPayload.clear();


  autoconfPayload["device"] = device.as<JsonObject>();
  autoconfPayload["availability_topic"] = MQTT_TOPIC_AVAILABILITY;
  autoconfPayload["name"] = identifier + String(" Dehumidifier");
  autoconfPayload["unique_id"] = identifier + String("_dehumidifier");
  autoconfPayload["device_class"] = "dehumidifier";

  autoconfPayload["max_humidity"] = 85;
  autoconfPayload["min_humidity"] = 35;

  autoconfPayload["state_topic"] = MQTT_TOPIC_STATE;
  autoconfPayload["state_value_template"] = "{\"state\": \"{{value_json.state}}\"}";
  autoconfPayload["payload_on"] = "{\"state\": \"on\"}";
  autoconfPayload["payload_off"] = "{\"state\": \"off\"}";
  autoconfPayload["command_topic"] = MQTT_TOPIC_COMMAND;
  autoconfPayload["state_value_template"] = "{\"state\": \"{{value_json.state}}\"}";
  autoconfPayload["payload_on"] = "{\"state\": \"on\"}";
  autoconfPayload["payload_off"] = "{\"state\": \"off\"}";

  StaticJsonDocument<64> modesDoc;
  JsonArray modes = modesDoc.to<JsonArray>();

  autoconfPayload["target_humidity_state_topic"] = MQTT_TOPIC_STATE;
  autoconfPayload["target_humidity_command_topic"] = MQTT_TOPIC_COMMAND;
  autoconfPayload["target_humidity_state_template"] = "{{value_json.humiditySetpoint | int}}";
  autoconfPayload["target_humidity_command_template"] = "{\"humiditySetpoint\": {{value | int}}}";

  modes.add("setpoint");
  modes.add("continuous");
  modes.add("smart");
  modes.add("clothesDrying");

  autoconfPayload["modes"] = modes;
  autoconfPayload["mode_state_topic"] = MQTT_TOPIC_STATE;
  autoconfPayload["mode_command_topic"] = MQTT_TOPIC_COMMAND;
  autoconfPayload["mode_state_template"] = "{{value_json.mode}}";
  autoconfPayload["mode_command_template"] = "{\"mode\": \"{{value}}\"}";

  serializeJson(autoconfPayload, mqttPayload);
  mqttClient.publish(MQTT_TOPIC_AUTOCONF_HUMIDIFIER, mqttPayload, true);

  autoconfPayload.clear();

  //Ioniser
  autoconfPayload["device"] = device.as<JsonObject>();
  autoconfPayload["availability_topic"] = MQTT_TOPIC_AVAILABILITY;
  autoconfPayload["name"] = identifier + String(" Ioniser");
  autoconfPayload["unique_id"] = identifier + String("_ioniser");

  autoconfPayload["state_topic"] = MQTT_TOPIC_STATE;
  autoconfPayload["command_topic"] = MQTT_TOPIC_COMMAND;

  autoconfPayload["value_template"] = "{{value_json.ion}}";
  autoconfPayload["payload_on"] = "{\"ion\": \"on\"}";
  autoconfPayload["payload_off"] = "{\"ion\": \"off\"}";
  autoconfPayload["state_on"] = "on";
  autoconfPayload["state_off"] = "off";

  serializeJson(autoconfPayload, mqttPayload);
  mqttClient.publish(MQTT_TOPIC_AUTOCONF_IONISER, mqttPayload, true);

  autoconfPayload.clear();
}
