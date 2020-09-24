#include "types.h"
#include "wifi.h"

#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <PubSubClient.h>

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

char MQTT_TOPIC_AUTOCONF_HUMIDITY_SENSOR[128];
char MQTT_TOPIC_AUTOCONF_FAN[128];


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
  state.errorCode = 0;

  delay(3000);
  updateAndSendNetworkStatus(false);


  snprintf(identifier, sizeof(identifier), "DEHUMIDIFIER-%X", ESP.getChipId());
  snprintf(MQTT_TOPIC_AVAILABILITY, 127, "%s/%s/status", FIRMWARE_PREFIX, identifier);
  snprintf(MQTT_TOPIC_STATE, 127, "%s/%s/state", FIRMWARE_PREFIX, identifier);
  snprintf(MQTT_TOPIC_COMMAND, 127, "%s/%s/command", FIRMWARE_PREFIX, identifier);

  snprintf(MQTT_TOPIC_AUTOCONF_HUMIDITY_SENSOR, 127, "homeassistant/sensor/%s/%s_humidity/config", FIRMWARE_PREFIX, identifier);
  snprintf(MQTT_TOPIC_AUTOCONF_FAN, 127, "homeassistant/fan/%s/%s_fan/config", FIRMWARE_PREFIX, identifier);



  WiFi.hostname(identifier);

  loadConfig();
  setupWifi();
  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setKeepAlive(10);
  mqttClient.setBufferSize(2048);
  mqttClient.setCallback(mqttCallback);

  mqttReconnect();
}


void loop() {
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
  DynamicJsonDocument stateJson(256);
  char payload[256];
  stateJson["state"] = state.powerOn ? "on" : "off";
  stateJson["humiditySetpoint"] = state.humiditySetpoint;
  stateJson["humidityCurrent"] = state.currentHumidity;
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
        commandJson["humiditySetpoint"].as<byte>()
      );

    }
  }
}

void publishAutoConfig() {
  char mqttPayload[2048];
  DynamicJsonDocument device(256);
  StaticJsonDocument<64> identifiersDoc;
  JsonArray identifiers = identifiersDoc.to<JsonArray>();

  identifiers.add(identifier);

  device["identifiers"] = identifiers;
  device["manufacturer"] = "Midea Group Co., Ltd.";
  device["model"] = "Generic Dehumidifier";
  device["name"] = identifier;
  device["sw_version"] = "0.0.1";


  DynamicJsonDocument humiditySensorAutoconfPayload(512);

  humiditySensorAutoconfPayload["device"] = device.as<JsonObject>();
  humiditySensorAutoconfPayload["availability_topic"] = MQTT_TOPIC_AVAILABILITY;
  humiditySensorAutoconfPayload["state_topic"] = MQTT_TOPIC_STATE;
  humiditySensorAutoconfPayload["name"] = identifier + String(" Humidity");
  humiditySensorAutoconfPayload["device_class"] = "humidity";
  humiditySensorAutoconfPayload["unit_of_measurement"] = "%";
  humiditySensorAutoconfPayload["value_template"] = "{{value_json.humidityCurrent}}";
  humiditySensorAutoconfPayload["unique_id"] = identifier + String("_humidity");

  serializeJson(humiditySensorAutoconfPayload, mqttPayload);
  mqttClient.publish(MQTT_TOPIC_AUTOCONF_HUMIDITY_SENSOR, mqttPayload, true);



  DynamicJsonDocument fanAutoconfPayload(1024);
  StaticJsonDocument<64> speedsDoc;
  JsonArray speeds = speedsDoc.to<JsonArray>();

  speeds.add("low");
  speeds.add("medium");
  speeds.add("high");

  fanAutoconfPayload["device"] = device.as<JsonObject>();
  fanAutoconfPayload["availability_topic"] = MQTT_TOPIC_AVAILABILITY;
  fanAutoconfPayload["state_topic"] = MQTT_TOPIC_STATE;
  fanAutoconfPayload["name"] = identifier + String(" Fan");
  fanAutoconfPayload["unique_id"] = identifier + String("_fan");
  fanAutoconfPayload["state_value_template"] = "{\"state\": \"{{value_json.state}}\"}";
  fanAutoconfPayload["payload_on"] = "{\"state\": \"on\"}";
  fanAutoconfPayload["payload_off"] = "{\"state\": \"off\"}";
  fanAutoconfPayload["command_topic"] = MQTT_TOPIC_COMMAND;
  fanAutoconfPayload["speeds"] = speeds;
  fanAutoconfPayload["speed_state_topic"] = MQTT_TOPIC_STATE;
  fanAutoconfPayload["speed_value_template"] = "{\"fanSpeed\": \"{{value_json.fanSpeed}}\"}";
  fanAutoconfPayload["speed_command_topic"] = MQTT_TOPIC_COMMAND;
  fanAutoconfPayload["payload_low_speed"] = "{\"fanSpeed\": \"low\"}";
  fanAutoconfPayload["payload_medium_speed"] = "{\"fanSpeed\": \"medium\"}";
  fanAutoconfPayload["payload_high_speed"] = "{\"fanSpeed\": \"high\"}";
  fanAutoconfPayload["qos"] = 1;


  serializeJson(fanAutoconfPayload, mqttPayload);
  mqttClient.publish(MQTT_TOPIC_AUTOCONF_FAN, mqttPayload, true);


}
