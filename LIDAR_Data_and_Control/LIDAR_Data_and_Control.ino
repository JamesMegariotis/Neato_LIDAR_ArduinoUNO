/*
 :)
Piccolo Laser Distance Scanner
Copyright (c) 2009-2011 Neato Robotics, Inc.
All Rights Reserved

Loader	V2.5.14010
CPU	F2802x/c001
Serial	WTD35312AA-0156924
LastCal	[5371726C]
Runtime	V2..59

Orange TX line from lidar connected to digital pin 0
*/

#include <PID_v1.h>

const int N_ANGLES = 360;                                       // # of angles (0..359)
const int SHOW_ALL_ANGLES = N_ANGLES;                           // value means 'display all angle data, 0..359'

const int ledPin = 13;
const int enable_pin1 = 6;
const int enable_pin2 = 7;
const int motor_pwm_pin = 5;    // pin connected to mosfet for motor speed control

byte id;
char version[6];
double rpm_setpoint=300;  // desired RPM (uses double to be compatible with PID library)
double rpm_min = 280;
double rpm_max = 320;
double pwm_max = 255;       // max analog value.  probably never needs to change from 1023
double pwm_min = 25;       // min analog pulse value to spin the motor
int sample_time = 20;      // how often to calculate the PID values

// PID tuning values
double Kp = 0.5;
double Ki = 0.25;
double Kd = 0.0;

boolean motor_enable = true;  // to spin the laser or not.  No data when not spinning
boolean raw_data = true;  // to retransmit the serial data to the USB port
boolean show_dist = false;  //  controlled by ShowDist and HideDist commands
boolean show_rpm = false;  // controlled by ShowRPM and HideRPM commands
boolean show_interval = false;  // true = show time interval, once per revolution, at angle=0
boolean show_errors = false;  // Show CRC, signal strength and invalid data errors
boolean aryAngles[N_ANGLES]; // array of angles to display

double pwm_val = 128;  // start with ~50% power
double pwm_last;
double motor_rpm;
unsigned long now;
unsigned long motor_check_timer = millis();
unsigned long motor_check_interval = 200;
unsigned int rpm_err_thresh = 10;  // 2 seconds (10 * 200ms) to shutdown motor with improper RPM and high voltage
unsigned int rpm_err = 0;
unsigned long curMillis;
unsigned long lastMillis = millis();

const unsigned char COMMAND = 0xFA;        // Start of new packet
const int INDEX_LO = 0xA0;                 // lowest index value
const int INDEX_HI = 0xF9;                 // highest index value

const int N_DATA_QUADS = 4;                // there are 4 groups of data elements
const int N_ELEMENTS_PER_QUAD = 4;         // viz., 0=distance LSB; 1=distance MSB; 2=sig LSB; 3=sig MSB

// Offsets to bytes within 'Packet'
const int OFFSET_TO_START = 0;
const int OFFSET_TO_INDEX = OFFSET_TO_START + 1;
const int OFFSET_TO_SPEED_LSB = OFFSET_TO_INDEX + 1;
const int OFFSET_TO_SPEED_MSB = OFFSET_TO_SPEED_LSB + 1;
const int OFFSET_TO_4_DATA_READINGS = OFFSET_TO_SPEED_MSB + 1;
const int OFFSET_TO_CRC_L = OFFSET_TO_4_DATA_READINGS + (N_DATA_QUADS * N_ELEMENTS_PER_QUAD);
const int OFFSET_TO_CRC_M = OFFSET_TO_CRC_L + 1;
const int PACKET_LENGTH = OFFSET_TO_CRC_M + 1;  // length of a complete packet
// Offsets to the (4) elements of each of the (4) data quads
const int OFFSET_DATA_DISTANCE_LSB = 0;
const int OFFSET_DATA_DISTANCE_MSB = OFFSET_DATA_DISTANCE_LSB + 1;
const int OFFSET_DATA_SIGNAL_LSB = OFFSET_DATA_DISTANCE_MSB + 1;
const int OFFSET_DATA_SIGNAL_MSB = OFFSET_DATA_SIGNAL_LSB + 1;

int Packet[PACKET_LENGTH];                 // an input packet
int ixPacket = 0;                          // index into 'Packet' array
const int VALID_PACKET = 0;
const int INVALID_PACKET = VALID_PACKET + 1;
const byte INVALID_DATA_FLAG = (1 << 7);   // Mask for byte 1 of each data quad "Invalid data"

/* REF: https://github.com/Xevel/NXV11/wiki
  The bit 7 of byte 1 seems to indicate that the distance could not be calculated.
  It's interesting to see that when this bit is set, the second byte is always 80, and the values of the first byte seem to be
  only 02, 03, 21, 25, 35 or 50... When it's 21, then the whole block is 21 80 XX XX, but for all the other values it's the
  data block is YY 80 00 00 maybe it's a code to say what type of error ? (35 is preponderant, 21 seems to be when the beam is
  interrupted by the supports of the cover) .
*/
const byte STRENGTH_WARNING_FLAG = (1 << 6);  // Mask for byte 1 of each data quat "Strength Warning"
/*
  The bit 6 of byte 1 is a warning when the reported strength is greatly inferior to what is expected at this distance.
  This may happen when the material has a low reflectance (black material...), or when the dot does not have the expected
  size or shape (porous material, transparent fabric, grid, edge of an object...), or maybe when there are parasitic
  reflections (glass... ).
*/
const byte BAD_DATA_MASK = (INVALID_DATA_FLAG | STRENGTH_WARNING_FLAG);

const byte eState_Find_COMMAND = 0;                        // 1st state: find 0xFA (COMMAND) in input stream
const byte eState_Build_Packet = eState_Find_COMMAND + 1;  // 2nd state: build the packet
int eState = eState_Find_COMMAND;
PID rpmPID(&motor_rpm, &pwm_val, &rpm_setpoint, Kp, Ki, Kd, DIRECT);

uint8_t inByte = 0;  // incoming serial byte
uint8_t motor_rph_high_byte = 0;
uint8_t motor_rph_low_byte = 0;
uint16_t aryDist[N_DATA_QUADS] = {0, 0, 0, 0};   // thre are (4) distances, one for each data quad
// so the maximum distance is 16383 mm (0x3FFF)
uint16_t aryQuality[N_DATA_QUADS] = {0, 0, 0, 0}; // same with 'quality'
uint16_t motor_rph = 0;
uint16_t startingAngle = 0;                      // the first scan angle (of group of 4, based on 'index'), in degrees (0..359)

boolean ledState = LOW;

void setup () {

  Serial.begin (115200, SERIAL_8N1);

  pinMode(motor_pwm_pin, OUTPUT);
  pinMode(enable_pin1, OUTPUT);
  pinMode(enable_pin2, OUTPUT);
  pinMode(ledPin, OUTPUT);
  
  rpmPID.SetOutputLimits(pwm_min, pwm_max);
  rpmPID.SetSampleTime(sample_time);
  rpmPID.SetTunings(Kp, Ki, Kd);
  rpmPID.SetMode(AUTOMATIC);

  eState = eState_Find_COMMAND;
  for (ixPacket = 0; ixPacket < PACKET_LENGTH; ixPacket++)  // Initialize
    Packet[ixPacket] = 0;
  ixPacket = 0;
  
  for (int ix = 0; ix < N_ANGLES; ix++)
    aryAngles[ix] = true;
  
} // End setup

void loop () {

  byte aryInvalidDataFlag[N_DATA_QUADS] = {0, 0, 0, 0};   // non-zero = INVALID_DATA_FLAG or STRENGTH_WARNING_FLAG is set
  
  if (Serial.available ()) {
    inByte = Serial.read ();
    if (raw_data)
      Serial.write(inByte);
    if (eState == eState_Find_COMMAND) {                  // flush input until we get COMMAND byte
      if (inByte == COMMAND) {
        eState++;                                         // switch to 'build a packet' state
        Packet[ixPacket++] = inByte;                      // store 1st byte of data into 'Packet'
      }
    }
    else {                                            // eState == eState_Build_Packet
      Packet[ixPacket++] = inByte;                    // keep storing input into 'Packet'
      if (ixPacket == PACKET_LENGTH) {                // we've got all the input bytes, so we're done building this packet
        if (eValidatePacket() == VALID_PACKET) {      // Check packet CRC
          startingAngle = processIndex();             // get the starting angle of this group (of 4), e.g., 0, 4, 8, 12, ...
          processSpeed();                             // process the speed
          // process each of the (4) sets of data in the packet
          for (int ix = 0; ix < N_DATA_QUADS; ix++)   // process the distance
            aryInvalidDataFlag[ix] = processDistance(ix);
          for (int ix = 0; ix < N_DATA_QUADS; ix++) { // process the signal strength (quality)
            aryQuality[ix] = 0;
            if (aryInvalidDataFlag[ix] == 0)
              processSignalStrength(ix);
          }
          if (show_dist) {                           // the 'ShowDistance' command is active
            for (int ix = 0; ix < N_DATA_QUADS; ix++) {
              if (aryAngles[startingAngle + ix]) {             // if we're supposed to display that angle
                if (aryInvalidDataFlag[ix] & BAD_DATA_MASK) {  // if LIDAR reported a data error...
                  if (show_errors) {                           // if we're supposed to show data errors...
                    Serial.print("A,");
                    Serial.print(startingAngle + ix);
                    Serial.print(",");
                    if (aryInvalidDataFlag[ix] & INVALID_DATA_FLAG)
                      Serial.println("I");
                    if (aryInvalidDataFlag[ix] & STRENGTH_WARNING_FLAG)
                      Serial.println("S");
                  }
                }
                else {                                         // show clean data
                  Serial.print("A,");
                  Serial.print(startingAngle + ix);
                  Serial.print(",");
                  Serial.print(int(aryDist[ix]));
                  Serial.print(",");
                  Serial.println(aryQuality[ix]);
                }
              }  // if (aryAngles[startingAngle + ix])
            }  // for (int ix = 0; ix < N_DATA_QUADS; ix++)
          }  // if (show_dist)
        }  // if (eValidatePacket() == 0
        else if (show_errors) {                                // we have encountered a CRC error
          Serial.println("C,CRC");
        }
        // initialize a bunch of stuff before we switch back to State 1
        for (int ix = 0; ix < N_DATA_QUADS; ix++) {
          aryDist[ix] = 0;
          aryQuality[ix] = 0;
          aryInvalidDataFlag[ix] = 0;
        }
        for (ixPacket = 0; ixPacket < PACKET_LENGTH; ixPacket++)  // clear out this packet
          Packet[ixPacket] = 0;
        ixPacket = 0;
        eState = eState_Find_COMMAND;                // This packet is done -- look for next COMMAND byte
      }  // if (ixPacket == PACKET_LENGTH)
    }  // if (eState == eState_Find_COMMAND)
  }  // if (Serial.available() > 0)
  if (motor_enable) {
    digitalWrite(enable_pin1, LOW);
    digitalWrite(enable_pin2, HIGH);
    rpmPID.Compute();
    if (pwm_val != pwm_last) {
      analogWrite(motor_pwm_pin, pwm_val);  // replacement for analogWrite()
      pwm_last = pwm_val;
    }  //if (pwm_val != pwm_last)
    motorCheck();
  }  // if (motor_enable)

} // End loop

/*
   processIndex - Process the packet element 'index'
   index is the index byte in the 90 packets, going from A0 (packet 0, readings 0 to 3) to F9
      (packet 89, readings 356 to 359).
   Enter with: N/A
   Uses:       Packet
               ledState gets toggled if angle = 0
               ledPin = which pin the LED is connected to
               ledState = LED on or off
               xv_config.show_dist = true if we're supposed to show distance
               curMillis = milliseconds, now
               lastMillis = milliseconds, last time through this subroutine
               xv_config.show_interval = true ==> display time interval once per revolution, at angle 0
   Calls:      digitalWrite() - used to toggle LED pin
               Serial.print
   Returns:    The first angle (of 4) in the current 'index' group
*/
uint16_t processIndex() {
  uint16_t angle = 0;
  uint16_t data_4deg_index = Packet[OFFSET_TO_INDEX] - INDEX_LO;
  angle = data_4deg_index * N_DATA_QUADS;     // 1st angle in the set of 4
  if (angle == 0) {
    if (ledState) {
      ledState = LOW;
    }
    else {
      ledState = HIGH;
    }
    digitalWrite(ledPin, ledState);

    if (show_rpm) {
      Serial.print("R,");
      Serial.print((int)motor_rpm);
      Serial.print(",");
      Serial.println((int)pwm_val);
    }

    curMillis = millis();
    if (show_interval) {
      Serial.print("T,");                                // Time Interval in ms since last complete revolution
      Serial.println(curMillis - lastMillis);
    }
    lastMillis = curMillis;

  } // if (angle == 0)
  return angle;
}

/*
   processSpeed- Process the packet element 'speed'
   speed is two-bytes of information, little-endian. It represents the speed, in 64th of RPM (aka value
      in RPM represented in fixed point, with 6 bits used for the decimal part).
   Enter with: N/A
   Uses:       Packet
               angle = if 0 then enable display of RPM and PWM
               xv_config.show_rpm = true if we're supposed to display RPM and PWM
   Calls:      Serial.print
*/
void processSpeed() {
  motor_rph_low_byte = Packet[OFFSET_TO_SPEED_LSB];
  motor_rph_high_byte = Packet[OFFSET_TO_SPEED_MSB];
  motor_rph = (motor_rph_high_byte << 8) | motor_rph_low_byte;
  motor_rpm = float( (motor_rph_high_byte << 8) | motor_rph_low_byte ) / 64.0;
}

/*
   Data 0 to Data 3 are the 4 readings. Each one is 4 bytes long, and organized as follows :
     byte 0 : <distance 7:0>
     byte 1 : <"invalid data" flag> <"strength warning" flag> <distance 13:8>
     byte 2 : <signal strength 7:0>
     byte 3 : <signal strength 15:8>
*/
/*
   processDistance- Process the packet element 'distance'
   Enter with: iQuad = which one of the (4) readings to process, value = 0..3
   Uses:       Packet
               dist[] = sets distance to object in binary: ISbb bbbb bbbb bbbb
                                       so maximum distance is 0x3FFF (16383 decimal) millimeters (mm)
   Calls:      N/A
   Exits with: 0 = okay
   Error:      1 << 7 = INVALID_DATA_FLAG is set
               1 << 6 = STRENGTH_WARNING_FLAG is set
*/
byte processDistance(int iQuad) {
  uint8_t dataL, dataM;
  aryDist[iQuad] = 0;                     // initialize
  int iOffset = OFFSET_TO_4_DATA_READINGS + (iQuad * N_DATA_QUADS) + OFFSET_DATA_DISTANCE_LSB;
  // byte 0 : <distance 7:0> (LSB)
  // byte 1 : <"invalid data" flag> <"strength warning" flag> <distance 13:8> (MSB)
  dataM = Packet[iOffset + 1];           // get MSB of distance data + flags
  if (dataM & BAD_DATA_MASK)             // if either INVALID_DATA_FLAG or STRENGTH_WARNING_FLAG is set...
    return dataM & BAD_DATA_MASK;        // ...then return non-zero
  dataL = Packet[iOffset];               // LSB of distance data
  aryDist[iQuad] = dataL | ((dataM & 0x3F) << 8);
  return 0;                              // okay
}

/*
   processSignalStrength- Process the packet element 'signal strength'
   Enter with: iQuad = which one of the (4) readings to process, value = 0..3
   Uses:       Packet
               quality[] = signal quality
   Calls:      N/A
*/
void processSignalStrength(int iQuad) {
  uint8_t dataL, dataM;
  aryQuality[iQuad] = 0;                        // initialize
  int iOffset = OFFSET_TO_4_DATA_READINGS + (iQuad * N_DATA_QUADS) + OFFSET_DATA_SIGNAL_LSB;
  dataL = Packet[iOffset];                  // signal strength LSB
  dataM = Packet[iOffset + 1];
  aryQuality[iQuad] = dataL | (dataM << 8);
}

/*
   eValidatePacket - Validate 'Packet'
   Enter with: 'Packet' is ready to check
   Uses:       CalcCRC
   Exits with: 0 = Packet is okay
   Error:      non-zero = Packet is no good
*/
byte eValidatePacket() {
  unsigned long chk32;
  unsigned long checksum;
  const int bytesToCheck = PACKET_LENGTH - 2;
  const int CalcCRC_Len = bytesToCheck / 2;
  unsigned int CalcCRC[CalcCRC_Len];

  byte b1a, b1b, b2a, b2b;
  int ix;

  for (int ix = 0; ix < CalcCRC_Len; ix++)       // initialize 'CalcCRC' array
    CalcCRC[ix] = 0;

  // Perform checksum validity test
  for (ix = 0; ix < bytesToCheck; ix += 2)      // build 'CalcCRC' array
    CalcCRC[ix / 2] = Packet[ix] + ((Packet[ix + 1]) << 8);

  chk32 = 0;
  for (ix = 0; ix < CalcCRC_Len; ix++)
    chk32 = (chk32 << 1) + CalcCRC[ix];
  checksum = (chk32 & 0x7FFF) + (chk32 >> 15);
  checksum &= 0x7FFF;
  b1a = checksum & 0xFF;
  b1b = Packet[OFFSET_TO_CRC_L];
  b2a = checksum >> 8;
  b2b = Packet[OFFSET_TO_CRC_M];
  if ((b1a == b1b) && (b2a == b2b))
    return VALID_PACKET;                       // okay
  else
    return INVALID_PACKET;                     // non-zero = bad CRC
}

void motorCheck() {  // Make sure the motor RPMs are good else shut it down
  now = millis();
  if (now - motor_check_timer > motor_check_interval) {
    if ((motor_rpm < rpm_min or motor_rpm > rpm_max) and pwm_val > 1000) {
      rpm_err++;
    }
    else {
      rpm_err = 0;
    }
    if (rpm_err > rpm_err_thresh) {
      motor_enable = false;
      ledState = LOW;
      digitalWrite(ledPin, ledState);
    }
    motor_check_timer = millis();
  }
}
