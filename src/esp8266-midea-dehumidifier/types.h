enum fanSpeed_t {
  low = 40,
  medium = 60,
  high = 80
};

enum dehumMode_t {
  setpoint = 1,
  continuous = 2,
  smart = 3,
  clothesDrying = 4
};

struct dehumidifierState_t { 
  boolean powerOn;
  dehumMode_t mode;
  fanSpeed_t fanSpeed;
  byte humiditySetpoint;
  byte currentHumidity;
  byte errorCode;
};
