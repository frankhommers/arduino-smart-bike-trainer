/**
 * Arduino Smart Bike Trainer
 * 
 * This Arduino project turns a non-smart digital bicyle trainer (like Tacx Flow T2200) into a smart trainer using a BLE connection for use with Zwift, BKool,...
 * 
 * According to https://hackaday.io/project/164276-tacx-flow-ant-conversion the cable layout of the RJ10 connector on the Tacx Flow is as follows:
 *    1 Cadence signal to computer (one pulse per crank revolution), 3.3V pulses, pulled high (no pulse => 3.3V)
 *    2 GND/Common.
 *    3 PWM control signal to brake (2.6V pulses).
 *    4 AC power / synchronization signal to computer, ~23V AC signal which appears the +ve part is clipped at ~19.5V (see picture).
 *    5 +18V (~1.5V variation sawtooth profile).
 *    6 Speed signal to computer (4 pulses per brake axle revolution) 3.3V pulses, pulled high (no pulse => 3.3V)
 * 
 * BLE information that might be useful
 * 
 *    BLE Base UUID: 00000000-0000-1000-8000-00805F9B34FB
 *    16-bit UUID to 128-bit: 0000xxxx-0000-1000-8000-00805F9B34FB
 *    32-bit UUID to 128-bit: xxxxxxxx-0000-1000-8000-00805F9B34FB
 *    
 *    FTMS (Fitness Machine Service): 0x1826
 *      Characteristics:
 *        Fitness Machine Feature: 0x2ACC
 *          READ: Fitness Machine Features (32bit) and Target Setting Feature (32bit)
 *        Indoor Bike Data: 0x2AD2
 *          NOTIFY (1/sec)
 *        Training Status: 0x2AD3
 *          NOTIFY (on change) | READ
 *        Supported Resistance Level Range: 0x2AD6
 *          READ
 *        Fitness Machine Control Point: 0x2AD9
 *          WRITE
 *        Fitness Machine Status: 0x2ADA
 * 
 * @name   Arduino Smart Bike Trainer
 * @author Kris Cardinaels <kris@krisc-informatica.be>
 * 
 * @link Tacx Flow Ant+ Conversion <https://hackaday.io/project/164276-tacx-flow-ant-conversion>
 * @link Bicycle Odometer and Speedometer <https://create.arduino.cc/projecthub/alan_dewindt/bicycle-odometer-and-speedometer-with-99-lap-period-recorder-331d2b>
 */
#include <ArduinoBLE.h>

#define DEVICE_NAME_LONG "KC Tacx Flow Smart Trainer"
#define DEVICE_NAME_SHORT "KC-TFST"
/** 
 * The Fitness Machine Control Point data type structure 
 * 
 */
#define FMCP_DATA_SIZE 19 // Control point consists of 1 opcode (byte) and maximum 18 bytes as parameters

// This fmcp_data_t structure represents the control point data. The first octet represents the opcode of the request
// followed by a parameter array of maximum 18 octects
typedef struct __attribute__( ( packed ) )
{
  uint8_t OPCODE;
  uint8_t OCTETS[ FMCP_DATA_SIZE-1 ];
} fmcp_data_t;

typedef union // The union type automatically maps the bytes member array to the fmcp_data_t structure member values
{
  fmcp_data_t values;
  uint8_t bytes[ FMCP_DATA_SIZE ];
} fmcp_data_ut;

fmcp_data_ut fmcpData;
short fmcpValueLength;
volatile long lastControlPointEvent = 0;
long previousControlPointEvent = 0;

/**
 * Fitness Machine Service, uuid 0x1826 or 00001826-0000-1000-8000-00805F9B34FB
 * 
 */
BLEService fitnessMachineService("1826");

// Service characteristics exposed by our trainer
BLECharacteristic fitnessMachineFeatureCharacteristic("2ACC", BLERead, 8);                                  // Fitness Machine Feature, mandatory, read
BLECharacteristic indoorBikeDataCharacteristic("2AD2", BLENotify, 6);                                       // Indoor Bike Data, optional, notify
BLECharacteristic trainingStatusCharacteristic("2AD3", BLENotify | BLERead, 20);                            // Training Status, optional, read & notify
BLECharacteristic supportedResistanceLevelRangeCharacteristic("2AD6", BLERead, 4);                          // Supported Resistance Level, read, optional
BLECharacteristic fitnessMachineControlPointCharacteristic("2AD9", BLEWrite | BLEIndicate, FMCP_DATA_SIZE); // Fitness Machine Control Point, optional, write & indicate
BLECharacteristic fitnessMachineStatusCharacteristic("2ADA", BLENotify, 2);                                 // Fitness Machine Status, mandatory, notify

// Buffers used to write to the characteristics and initial values
unsigned char ftmfBuffer[8] = { 0b10000111, 0b01000100, 0, 0, 0, 0, 0, 0};                                  // Features: 0 (Avg speed), 1 (Cadence), 2 (Total distance), 7 (Resistance level), 10 (Heart rate measurement), 14 (Power measurement)
unsigned char ibdBuffer[6] = {0, 0, 0, 0, 0, 0};                                                            // 
unsigned char srlrBuffer[4] = {0, 200, 0, 1};
unsigned char ftmsBuffer[2] = {0, 0};
unsigned char tsBuffer[2] = {0x0, 0x0};                                                                     // Training status: flags: 0 (no string present); Status: 0x00 = Other
unsigned char ftmcpBuffer[20];

/**
 * Training session
 * 
 */
enum status_t {STOPPED, RUNNING, PAUSED};
unsigned short training_status = STOPPED;
unsigned long training_started;
unsigned long training_elapsed;

/**
 * Indoor Bike Data characteristic variables
 * 
 */
const uint8_t flagMoreData = 1;
const uint8_t flagAverageSpeed = 2;
const uint8_t flagInstantaneousCadence = 4;
const uint8_t flagAverageCadence = 8;
const uint8_t flagTotalDistance = 16;
const uint8_t flagResistanceLevel = 32;
const uint8_t flagIntantaneousPower = 64;
const uint8_t flagAveragePower = 128;
const uint8_t flagExpendedEnergy = 256;
const uint8_t flagHeartRate = 512;
const uint8_t flagMetabolicEquivalent = 1024;
const uint8_t flagElapsedTime = 2048;
const uint8_t flagRemainingTime = 4096;

int instantaneous_speed = 0;
int average_speed = 0;
int instantaeous_cadence = 0;
int average_cadence = 0;
int total_distance = 0;
int resistance_level = 0;
 
/**
 * Fitness Machine Control Point opcodes 
 * 
 * LSO: uint8 Op Code
 * MSO: 0..18 octets Parameters
 */
const uint8_t fmcpRequestControl = 0x00;
const uint8_t fmcpReset = 0x01;
const uint8_t fmcpSetTargetSpeed = 0x02;
const uint8_t fmcpSetTargetInclination = 0x03;
const uint8_t fmcpSetTargetResistanceLevel = 0x04;
const uint8_t fmcpSetTargetPower = 0x05;
const uint8_t fmcpSetTargetHeartRate = 0x06;
const uint8_t fmcpStartOrResume = 0x07;
const uint8_t fmcpStopOrPause = 0x08;
const uint8_t fmcpSetTargetedExpendedEngery = 0x09;
const uint8_t fmcpSetTargetedNumberOfSteps = 0x0A;
const uint8_t fmcpSetTargetedNumberOfStrided = 0x0B;
const uint8_t fmcpSetTargetedDistance = 0x0C;
const uint8_t fmcpSetTargetedTrainingTime = 0x0D;
const uint8_t fmcpSetTargetedTimeInTwoHeartRateZones = 0x0E;
const uint8_t fmcpSetTargetedTimeInThreeHeartRateZones = 0x0F;
const uint8_t fmcpSetTargetedTimeInFiveHeartRateZones = 0x10;
const uint8_t fmcpSetIndoorBikeSimulationParameters = 0x11;
const uint8_t fmcpSetWheelCircumference = 0x12;
const uint8_t fmcpSetSpinDownControl = 0x13;
const uint8_t fmcpSetTargetedCadence = 0x14;
const uint8_t fmcpResponseCode = 0x80;

/**
 * The client device
 */
BLEDevice central;

/**
 * Variables for the handling of writing statuses over BLE
 */
#define RED 22     
#define GREEN 23
#define BLUE 24     
int ble_connected = LOW;
const short NOTIFICATION_INTERVAL = 1000;
long previous_notification = 0;

/**
 * Speed and Cadence sensors
 */
#define CADENCE 10
#define SPEED 11
float speed_raw;
volatile long speed_counter;
long speed_counter_previous;
unsigned long speed_elapsed_time;
unsigned long speed_last_micros;

float cadence_raw;
volatile long cadence_counter;
long cadence_counter_previous;
unsigned long cadence_elapsed_time;
unsigned long cadence_last_millis;

/**
 * Data for the resistance calculation of the trainer
 */
float wind_speed = 0;       // meters per second, resolution 0.001
float grade = 0;            // percentage, resolution 0.01
float crr = 0;              // Coefficient of rolling resistance, resolution 0.0001
float cw = 0;               // Wind resistance Kg/m, resolution 0.01;

float weight = 95;
float trainer_resistance = 0; // To be mapped to the correct value for the trainer (-4..9)

void writeStatus(int red, int green, int blue) {
  analogWrite(RED, red);
  analogWrite(GREEN, green);
  analogWrite(BLUE, blue);
}

void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(0)); // For testing purposes of speed and cadence

  pinMode(RED, OUTPUT);
  pinMode(GREEN, OUTPUT);
  pinMode(BLUE, OUTPUT);
  writeStatus(1024, 1024, 0);

  if (!BLE.begin()) { // Error starting the bluetooth module
    while (1) {
      writeStatus(0, 1024, 1024);
      delay(250);
      writeStatus(1024, 1024, 1024);
      delay(250);
    }
  }

  BLE.setDeviceName(DEVICE_NAME_LONG);
  BLE.setLocalName(DEVICE_NAME_SHORT);
  BLE.setAdvertisedService(fitnessMachineService);

  // add the characteristic to the service
  fitnessMachineService.addCharacteristic(fitnessMachineFeatureCharacteristic);
  fitnessMachineService.addCharacteristic(indoorBikeDataCharacteristic);
  fitnessMachineService.addCharacteristic(trainingStatusCharacteristic);
  fitnessMachineService.addCharacteristic(supportedResistanceLevelRangeCharacteristic);
  fitnessMachineService.addCharacteristic(fitnessMachineControlPointCharacteristic);
  fitnessMachineService.addCharacteristic(fitnessMachineStatusCharacteristic);

  // Add our AST service to the device
  BLE.addService(fitnessMachineService);

  // Write values to the characteristics that can be read
  fitnessMachineFeatureCharacteristic.writeValue(ftmfBuffer, 8);
  indoorBikeDataCharacteristic.writeValue(ibdBuffer, 6);
  supportedResistanceLevelRangeCharacteristic.writeValue(srlrBuffer, 4);
  fitnessMachineStatusCharacteristic.writeValue(ftmsBuffer, 2);
  trainingStatusCharacteristic.writeValue(tsBuffer, 2);

  // Write requests to the control point characteristic are handled by an event handler
  fitnessMachineControlPointCharacteristic.setEventHandler(BLEWritten, fitnessMachineControlPointCharacteristicWritten);

  // start advertising
  BLE.advertise();
  BLE.setEventHandler(BLEConnected, blePeripheralConnectHandler);
  BLE.setEventHandler(BLEDisconnected, blePeripheralDisconnectHandler);

  // Speed and Cadence handling
  pinMode(SPEED, INPUT_PULLUP);
  pinMode(CADENCE, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SPEED), speedPulseInterrupt, FALLING);
  attachInterrupt(digitalPinToInterrupt(CADENCE), cadencePulseInterrupt, FALLING);


  // Initialize training to stopped
  training_status = STOPPED;
  training_started = 0;
  training_elapsed = 0;
}

void loop() {
  BLE.poll();

  central = BLE.central();
  if (central && central.connected()) {
    int current_millis = millis();
    if (current_millis > previous_notification + NOTIFICATION_INTERVAL) { // A new notification should be done after the given period (1 second)
  
      writeIndoorBikeDataCharacteristic();
      // writeTrainingStatus(); // Training Status shall be notified when there is a transition in the training program
      // writeFitnessMachineStatus(); // Fitness Machine Status shall be notified when a chane happened, not at regular interval
      previous_notification = millis();
    }
  }

  if (speed_counter != speed_counter_previous) {
    speed_elapsed_time = (micros() - speed_last_micros);
    speed_raw = (2.355/(speed_elapsed_time/1000))*10;
    speed_counter_previous = speed_counter;
    speed_last_micros = micros();
  }

  if (cadence_counter != cadence_counter_previous) {
    cadence_elapsed_time = (millis() - cadence_last_millis);
    cadence_raw = 60000/cadence_elapsed_time;
    cadence_counter_previous = cadence_counter;
    cadence_last_millis = millis();
  }

  if (previousControlPointEvent != lastControlPointEvent) { // A newer control point has been written, so handle it
    handleControlPoint();
    previousControlPointEvent = lastControlPointEvent;
  }
}

void writeIndoorBikeDataCharacteristic() {
  ibdBuffer[0] = 0x00 | flagInstantaneousCadence; // More Data = 0 (instantaneous speed present), bit 2: instantaneous cadence present
  ibdBuffer[1] = 0;
  speed_raw = random(25, 50) / 2.56; // Testing the speed with a random value
  cadence_raw = random(75, 150); // Testing the cadence with a random value

  int s = (int)(speed_raw * 100);
  ibdBuffer[2] = s & 0xFF; // Instantaneous Speed, uint16
  ibdBuffer[3] = (s >> 8) & 0xFF;
  ibdBuffer[4] = (int)cadence_raw & 0xFF; // Instantaneous Cadence, uint16
  ibdBuffer[5] = ((int)cadence_raw >> 8) & 0xFF;
  indoorBikeDataCharacteristic.writeValue(ibdBuffer, 6);
  Serial.println("Indoor Bike Data written");
}

void writeTrainingStatus() {
  switch (training_status) {
    case STOPPED:
      tsBuffer[0] = 0x02;
      tsBuffer[1] = 0x01;
      trainingStatusCharacteristic.writeValue(tsBuffer, 2);
      break;
    case PAUSED:
      tsBuffer[0] = 0x02;
      tsBuffer[1] = 0x02;
      trainingStatusCharacteristic.writeValue(tsBuffer, 2);
      break;
    case RUNNING:
      tsBuffer[0] = 0x04;
      trainingStatusCharacteristic.writeValue(tsBuffer, 1);
      break;
  }
  
  Serial.println("Training Status written");
}

void writeFitnessMachineStatus() {
}

void handleControlPoint() {
    Serial.println("Control point received");
    Serial.print("OpCode: ");
    Serial.println(fmcpData.values.OPCODE, HEX);
    Serial.print("Values: ");
    for (int i=0; i<fmcpValueLength-1; i++) Serial.println(fmcpData.values.OCTETS[i], HEX);
    Serial.println();
    switch(fmcpData.values.OPCODE) {
      case fmcpRequestControl: {
        // Always allow control
        ftmcpBuffer[0] = fmcpResponseCode;
        ftmcpBuffer[1] = fmcpData.values.OPCODE;
        ftmcpBuffer[2] =  0x01;
        fitnessMachineControlPointCharacteristic.writeValue(ftmcpBuffer, 3);
        break;
      }
      case fmcpStartOrResume: {
        training_status = RUNNING;
        break;
      }
      case fmcpStopOrPause: {
        training_status = STOPPED;
        break;
      }
      case fmcpSetIndoorBikeSimulationParameters: {
        wind_speed = fmcpData.values.OCTETS[0] + (fmcpData.values.OCTETS[1] * 256);
        grade = fmcpData.values.OCTETS[2] + (fmcpData.values.OCTETS[3] * 256);
        crr = fmcpData.values.OCTETS[4];
        cw = fmcpData.values.OCTETS[5];
        Serial.print("Wind speed (1000): "); Serial.println((int)(wind_speed));
        Serial.print("Grade (100): "); Serial.println((int)grade);
        Serial.print("Crr (10000): "); Serial.println((int)crr);
        Serial.print("Cw (100): "); Serial.println((int)cw);
        ftmcpBuffer[0] = fmcpResponseCode;
        ftmcpBuffer[1] = fmcpData.values.OPCODE;
        ftmcpBuffer[2] =  0x01;
        fitnessMachineControlPointCharacteristic.writeValue(ftmcpBuffer, 3);
        break;
      }
      case fmcpReset:
      case fmcpSetTargetResistanceLevel:
      case fmcpSetTargetSpeed:
      case fmcpSetTargetInclination:
      case fmcpSetTargetPower:
      case fmcpSetTargetHeartRate:
      case fmcpSetTargetedExpendedEngery:
      case fmcpSetTargetedNumberOfSteps:
      case fmcpSetTargetedNumberOfStrided:
      case fmcpSetTargetedDistance:
      case fmcpSetTargetedTrainingTime:
      case fmcpSetTargetedTimeInTwoHeartRateZones:
      case fmcpSetTargetedTimeInThreeHeartRateZones:
      case fmcpSetTargetedTimeInFiveHeartRateZones:
      case fmcpSetWheelCircumference:
      case fmcpSetSpinDownControl:
      case fmcpSetTargetedCadence: {
        ftmcpBuffer[0] = fmcpResponseCode;
        ftmcpBuffer[1] = fmcpData.values.OPCODE;
        ftmcpBuffer[2] =  0x02; // Op Coce not supported for now
        fitnessMachineControlPointCharacteristic.writeValue(ftmcpBuffer, 3);
        break;
      }
    }
}

void blePeripheralConnectHandler(BLEDevice central) {
  ble_connected = HIGH;
  writeStatus(1024, 0, 1024);
}

void blePeripheralDisconnectHandler(BLEDevice central) {
  ble_connected = LOW;
  writeStatus(1024, 1024, 0);
}

void fitnessMachineControlPointCharacteristicWritten(BLEDevice central, BLECharacteristic characteristic) {
  fmcpValueLength = fitnessMachineControlPointCharacteristic.valueLength();
  memset(fmcpData.bytes, 0, sizeof(fmcpData.bytes));
  fitnessMachineControlPointCharacteristic.readValue(fmcpData.bytes, fmcpValueLength);
  lastControlPointEvent = millis(); 
}

void speedPulseInterrupt() {
  speed_counter++;
}

void cadencePulseInterrupt() {
  cadence_counter++;
}

/**
 * Training session handling
 */
void training_start() {
  training_elapsed = 0;
  training_started = millis();
  training_status = RUNNING;
}

void training_pause() {
  if (training_status == RUNNING) {
    training_elapsed = training_elapsed + millis() - training_started;
    training_status = PAUSED;
  }
}

void training_resume() {
  if (training_status == PAUSED) {
    training_started = millis();
    training_status = RUNNING;
  }
}

void training_stop() {
  if (training_status == RUNNING) {
    training_elapsed = millis() - training_started + training_elapsed;
  }
  training_status = STOPPED;
}

long training_read() {
  if (training_status == RUNNING) {
    return millis() - training_started + training_elapsed;
  }
  return training_elapsed;
}
