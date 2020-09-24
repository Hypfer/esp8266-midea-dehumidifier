byte networkStatus[20];
byte currentHeader[10];
byte getStatusCommand[21] = {
  0x41, 0x81, 0x00, 0xff, 0x03, 0xff,
  0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x03
};
byte setStatusCommand[25];

boolean parseState() {
  state.powerOn = serialRxBuf[11] & 0x01 > 0;
  state.mode = (dehumMode_t)(serialRxBuf[12] & 0x0f);
  state.fanSpeed = (fanSpeed_t)(serialRxBuf[13] & 0x7f);

  state.humiditySetpoint = serialRxBuf[17] >= 100 ? 99 : serialRxBuf[17];
  state.currentHumidity = serialRxBuf[26];
  state.errorCode = serialRxBuf[31];

  clearRxBuf();
}

void clearRxBuf() {
  //Clear everything for the next message
  memset(serialRxBuf, 0, sizeof(serialRxBuf));
}
void clearTxBuf() {
  //Clear everything for the next message
  memset(serialTxBuf, 0, sizeof(serialTxBuf));
}

void handleUart() {
  if (Serial.available()) {
    Serial.readBytesUntil('\n', serialRxBuf, 250);
    if (serialRxBuf[10] == 0xc8) {
      parseState();
      publishState();
    } else if (serialRxBuf[10] == 0x63) {
      updateAndSendNetworkStatus(isMqttConnected());
    } else if ( //Yes, this is ugly :(
      serialRxBuf[10] == 0x0 &&
      serialRxBuf[50] == 0xaa &&
      serialRxBuf[51] == 0x1e &&
      serialRxBuf[52] == 0xa1 && //Appliance Type
      serialRxBuf[58] == 0x03 &&
      serialRxBuf[59] == 0x64 &&
      serialRxBuf[61] == 0x01 &&
      serialRxBuf[65] == 0x01
    ) {
      resetWifiSettingsAndReboot();
    } else {
      //Serial.print("Received msg with invalid type: ");
      //Serial.println(serialRxBuf[10]);
    }
  }
}

void writeHeader(byte msgType, byte agreementVersion, byte packetLength) {
  currentHeader[0] = 0xAA; //Sync Header
  currentHeader[1] = 10 + packetLength + 1;
  currentHeader[2] = 0xA1; //ApplianceType
  currentHeader[3] = 0x00; //Frame sync check (not used)
  currentHeader[4] = 0x00; //Reserved
  currentHeader[5] = 0x00; //Reserved
  currentHeader[6] = 0x00; //MsgId
  currentHeader[7] = 0x00; //ProtocolVersion
  currentHeader[8] = agreementVersion;
  currentHeader[9] = msgType;
}

void handleStateUpdateRequest(String requestedState, String mode, String fanSpeed, byte humiditySetpoint) {
  dehumidifierState_t newState;

  newState.powerOn = state.powerOn;
  newState.mode = state.mode;
  newState.fanSpeed = state.fanSpeed;
  newState.humiditySetpoint = state.humiditySetpoint;

  if (requestedState == "on") {
    newState.powerOn = true;
  } else if (requestedState == "off") {
    newState.powerOn = false;
  }

  if (mode == "setpoint") {
    newState.mode = (dehumMode_t)setpoint;
  } else if (mode == "continuous") {
    newState.mode = (dehumMode_t)continuous;
  } else if (mode == "smart") {
    newState.mode = (dehumMode_t)smart;
  } else if (mode == "clothesDrying") {
    newState.mode = (dehumMode_t)clothesDrying;
  }

  if (fanSpeed == "low") {
    newState.fanSpeed = (fanSpeed_t)low;
  } else if (fanSpeed == "medium") {
    newState.fanSpeed = (fanSpeed_t)medium;
  } else if (fanSpeed == "high") {
    newState.fanSpeed = (fanSpeed_t)high;
  }

  if (humiditySetpoint && humiditySetpoint >= 35 && humiditySetpoint <= 85) {
    newState.humiditySetpoint = humiditySetpoint;
  }

  if ( //Only send if we have updates
    newState.powerOn != state.powerOn ||
    newState.mode != state.mode ||
    newState.fanSpeed != state.fanSpeed ||
    newState.humiditySetpoint != state.humiditySetpoint
  ) {
    updateSetStatus(
      newState.powerOn,
      newState.mode,
      newState.fanSpeed,
      newState.humiditySetpoint
    );
    sendSetStatus();

    //Write back so that we don't swallow commands if there are many coming right after each other
    //This may cause unexpected results if the connection to the unit doesn't work
    state.powerOn = newState.powerOn;
    state.mode = newState.mode;
    state.fanSpeed = newState.fanSpeed;
    state.humiditySetpoint = newState.humiditySetpoint;
    delay(30);
  }
}

void sendSetStatus() {
  sendMessage(0x02, 0x03, 25, setStatusCommand);
}

void updateSetStatus(boolean powerOn, dehumMode_t dehumMode, fanSpeed_t fanSpeed, byte humiditySetpoint) {
  memset(setStatusCommand, 0, sizeof(setStatusCommand));

  setStatusCommand[0] = 0x48; //Magic
  setStatusCommand[1] = powerOn ? 0x01 : 0x00;
  setStatusCommand[2] = (byte)(dehumMode & 0x0f);
  setStatusCommand[3] = (byte)fanSpeed;
  setStatusCommand[7] = humiditySetpoint;
}

void updateAndSendNetworkStatus(boolean isConnected) {
  updateNetworkStatus(isConnected);
  sendMessage(0x0D, 0x03, 20, networkStatus);
}

void getStatus() {
  sendMessage(0x03, 0x03, 21, getStatusCommand);
}

void updateNetworkStatus(boolean isConnected) {
  memset(networkStatus, 0, sizeof(networkStatus));

  // Byte 0: Module type:
  // - 0x00 RF module
  // - 0x01 WiFi module
  networkStatus[0] = 0x01;

  // Byte 1: WiFi module working mode:
  // - 0x01 Client mode
  // - 0x02 Configuration mode
  // - 0x03 AP mode
  networkStatus[1] = 0x01;

  // Byte 2: WiFi signal strength
  // - 0x00 No signal
  // - 0x01 Weak
  // - 0x02 Low
  // - 0x03 Medium
  // - 0x04 Strong
  // - 0xFF WiFi is not supported
  networkStatus[2] = 0x04;

  // Byte 3-6: IPv4 address of client in reverse order
  networkStatus[3] = 1;
  networkStatus[4] = 0;
  networkStatus[5] = 0;
  networkStatus[6] = 127;

  // Byte 7: RF Signal strength
  // - 0x00, no signal
  // - 0x01, weak
  // - 0x02, low
  // - 0x03, medium
  // - 0x04, strong
  // - 0xFF, RF is not supported
  networkStatus[7] = 0xff;

  // Byte 8: Router status
  // - 0x00, wireless router is connected
  // - 0x01, wireless router not connected
  // - 0x02, connecting to a wireless router
  // - 0x03, password verification error
  // - 0x04, no wireless router found
  // - 0x05, IP cannot be obtained
  // - 0x06, wireless unstable
  // - 0xFF, WI-FI failure
  networkStatus[8] = isConnected ? 0x00 : 0x01;

  // Byte 9: Cloud service connection status:
  // - 0x00, connected to the cloud service center
  // - 0x01, not connected to the cloud service center
  // - 0x02, unstable internet connection
  // - 0x03, domain name resolution error
  // - 0x04, cloud service connection refused
  // - 0x05, cloud service maintenance
  // - 0xFF, cloud service failure
  networkStatus[9] = isConnected ? 0x00 : 0x01;

  // Byte 10: Direct LAN connection status
  // - 0x00: No connection/connection has been disconnected
  // - 0x01: connected/connected with mobile terminal
  networkStatus[10] = 0x00;

  // Byte 11 Number of TCP connections between the module and the mobile terminal
  networkStatus[11] = 0x00;

  // Byte 12 - 19 Reserved
}

void sendMessage(byte msgType, byte agreementVersion, byte payloadLength, byte *payload) {
  clearTxBuf();

  writeHeader(msgType, agreementVersion, payloadLength);
  memcpy(serialTxBuf, currentHeader, 10);
  memcpy(serialTxBuf + 10, payload, payloadLength);
  serialTxBuf[10 + payloadLength] = crc8(serialTxBuf + 10, payloadLength);
  serialTxBuf[10 + payloadLength + 1] = checksum(serialTxBuf, 10 + payloadLength + 1);

  Serial.write(serialTxBuf, 10 + payloadLength + 2);
}
