#include <HX711.h>
#include <Wire.h>
#include <EEPROM.h>
#define NUM_ELEMENTS(x)  (sizeof(x) / sizeof((x)[0])) // Use to calculate how many elements are in an array
#define DRAIN_SENSOR_PIN 3                            // Drain basin float sensor connection. When this goes high, Home Assistant tells a pump to pump contents of basin back into mixing res.
#define LOADCELL_DOUT_PIN 8                           // HX711
#define LOADCELL_SCK_PIN 9                            // HX711

unsigned long baudRate  = 115200;

// Mixing res scale stuff
HX711 scale;
bool scaleCalMode = false;
struct hx711CalVals 
{
  long calibrationFactor;
  long zeroFactor;
};
hx711CalVals mixingRes;

// Atlas 
int channel;                                    // For channel switching - 0-7 serial, 8-127 I2C addresses
char* cmd; 
bool i2cCallComplete;                           // This flag allows void loop() to keep running quickly without waiting for i2c data to arrive
char sensorData[30];                            // A 30 byte character array to hold incoming data from the sensors

// Serial receiving
bool newData = false;
const byte numChars = 100;
char receivedChars[numChars];

bool drainBasinStatus = LOW;

// Flood detection
char waterSensorNames[][13]                     // For describing where flood sensor was triggered
{
  {"under res 1!"},           
  {"under res 2!"},
  {"in tent!"},
  {"in overflow!"}
};
int waterSensorPins[] 
{A0, A1, A2, A3};

// Timing
unsigned long currentMillis;                    // Snapshot of current time
unsigned long floodStartMillis;                 // Timer to check for flood
unsigned long waterLvlMillis;                   // Timer to check nute res water level with weigh sensors
unsigned long drainBasinMillis;                 // Timer to check drain basin float sensor
unsigned long scaleCalMillis;                   // Timer to read the scale when calibrating it
unsigned long i2cWaitMillis;                    // Timer to wait for i2c data after call
const unsigned long waterLvlPeriod = 1000;      // Time in milliseconds between checking nute res water level
const unsigned long floodPeriod = 10000;        // Time between checking floor moisture sensors for flood
const unsigned long drainBasinPeriod = 5000;    // Time between checking float sensor in drain basin in tent
const unsigned long scaleCalPeriod = 500;       // Time between reading HX711 sensor when calibrating
unsigned long i2cWaitPeriod;                    // Time to wait for sensor data after I2C_call function

// Relays
int relayPins[2][8]
{
  {27, 29, 31, 33, 35, 37, 39, 41},              // 8 Chan Relay Board: [0]= Nute magnetic stirrers, [1]= RO feed solenoid
  {22, 23, 24, 25}                               // 4 Chan Relay Board: [0]= RO reservoir solenoid and pump, [1]= Nutrient reservoir mixing pump [2]= Nutrient reservoir plant flooding pump [3]= Drain basin pump
};

void setup()
{
  int cfAddress = 0;                             // Address in EEPROM memory of HX711 calibration factor
  int zfAddress = 4;                             // Address of HX711 zero factor
  Serial3.begin(baudRate);
  Serial.begin(baudRate);
  Wire.begin();  
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);                                 
  floodStartMillis = 0;
  pinMode(DRAIN_SENSOR_PIN, INPUT_PULLUP);

  for (int i = 0; i <= 2; i++)
  {
    for (int j = 0; j <= NUM_ELEMENTS(relayPins[i]); j++)
    {
      pinMode(relayPins[i][j], OUTPUT);
      digitalWrite(relayPins[i][j], HIGH);
    }
  }
  EEPROM.get(cfAddress, mixingRes.calibrationFactor);
  EEPROM.get(zfAddress, mixingRes.zeroFactor);
  setupScale();
  Serial.println("Setup complete, starting loop!");
}

void loop()
{
  currentMillis = millis();
  recvWithStartEndMarkers();
  processSerialData();

  if (currentMillis - drainBasinMillis >= drainBasinPeriod)
  {
    checkDrainBasin();
  }
  
  if (currentMillis - waterLvlMillis >= waterLvlPeriod)
  {
    checkWaterLvl();
  }
  
  if (currentMillis - floodStartMillis >= floodPeriod)
  {
    checkForFlood();
  }
  
  if ((currentMillis - scaleCalMillis >= scaleCalPeriod) && (scaleCalMode == true))
  {
    calibrateScale();
  }

  if ((currentMillis - i2cWaitMillis >= i2cWaitPeriod) && (i2cCallComplete == true))
  {
    parseI2Cdata();
  }  
}

void recvWithStartEndMarkers()                                       // Check for serial data from ESP32. Thanks to Robin2 for the serial input basics thread on the Arduino forums.
{
  static bool recvInProgress = false;
  static byte ndx = 0;
  char startMarker = '<';
  char endMarker = '>';
  char rc;

  while (Serial3.available() > 0 && newData == false)
  {
    rc = Serial3.read();

    if (recvInProgress == true)
    {
      if (rc != endMarker)
      {
        receivedChars[ndx] = rc;
        ndx++;
        if (ndx >= numChars)
        {
          ndx = numChars - 1;
        }
      }
      else
      {
        receivedChars[ndx] = '\0';       
        recvInProgress = false;
        ndx = 0;
        newData = true;
      }
    }

    else if (rc == startMarker)
    {
      recvInProgress = true;
    }
  }
}

void processSerialData()
{
  if (newData == true)
  {
    char commandChar = receivedChars[0];
    switch (commandChar)
    {
      case '9':                                                     // If the message from the ESP32 starts with a "9", it's related to pH.
      {
        channel = atoi(strtok(receivedChars, ":"));                 // Parse the string at each colon
        cmd = strtok(NULL, ":");
        I2C_call();                                                 // Send to I2C
        break;
      }
      
      case '1':                                                     // If the message from the ESP32 starts with a "1", it's related to EC.
      {
        channel = atoi(strtok(receivedChars, ":"));
        cmd = strtok(NULL, ":");
        I2C_call();
        break;
      }
      
      case 'R':                                                     // If message starts with "R", it's for relays. Message format is "Relay:<BOARD#>:<RELAY#>:<STATUS>". Example: "Relay:0:4:1"
      {
        int boardNumber;
        int relayNumber;
        int relayPower;
        char* strtokIndx;  
        char buff[20];
    
        strtokIndx = strtok(receivedChars, ":");                    // Skip the first segment which is the 'R' character 
        strtokIndx = strtok(NULL, ":");                             // Get the board number
        boardNumber = atoi(strtokIndx);
        strtokIndx = strtok(NULL, ":");                             // Get the relay number
        relayNumber = atoi(strtokIndx);  
        strtokIndx = strtok(NULL, ":");                             // Get the relay power state
        relayPower = atoi(strtokIndx);
        
        triggerRelay(boardNumber, relayNumber, relayPower);
        
        sprintf(buff, "<Relay FB:%d:%d:%d>", boardNumber, relayNumber, relayPower);
        Serial3.println(buff);
        break;
      }
      
      case 'H':                                                     // If message starts with the letter "H", it's for the weigh scale.
      { 
        char commandChar = receivedChars[7];                        // Message format will be "HX711: Begin", "HX711: Exit", or a calibration value like "-20000"
        switch(commandChar)
        { 
          case 'B':                                                 // Begin cal mode
          {
            beginCalMode();
            break;
          }
          case 'E':                                                 // Exit cal mode
          {
            int cfAddress = 0;
            int zfAddress = 4;
            scaleCalMode = false;
            EEPROM.put(cfAddress, mixingRes.calibrationFactor);
            EEPROM.put(zfAddress, mixingRes.zeroFactor); 
            Serial3.print("<HX711: Calibration saved to EEPROM!>");
            Serial.println(mixingRes.calibrationFactor);
            Serial.println(mixingRes.zeroFactor);
            scale.tare();                                           // Tare the scale. Before you save and exit, the reservoir and any equipment that sit in it should be on the sensor,
            break;                                                  // however, the weights you use to calibrate it must be removed. Only the stuff that will be permanent should stay.
          }
          default:
          {
            char* strtokIndx;  
            strtokIndx = strtok(receivedChars, ":");                // Skip the first segment which is the "HX711:" part of the message
            strtokIndx = strtok(NULL, ":");                         // Get the calibration factor
            mixingRes.calibrationFactor = atol(strtokIndx);
          }
        }
        break;
      }
    }
    newData = false;
  }
}

void I2C_call()                                                     // Function to parse and call I2C commands. 
{
  memset(sensorData, 0, sizeof(sensorData));                        // Clear sensorData array;

  if (cmd[0] == 'C' || cmd[0] == 'R')
  {
    i2cWaitPeriod = 1400;                                           // If a command has been sent to calibrate or take a reading we wait 1400ms so that the circuit has time to take the reading.
  }
  else 
  {
    i2cWaitPeriod = 300;                                            // If any other command has been sent we wait only 300ms.
  }

  Wire.beginTransmission(channel);                                  // cCall the circuit by its ID number.
  Wire.write(cmd);                                                  // Transmit the command that was sent through the serial port.
  Wire.endTransmission();                                           // End the I2C data transmission.
  i2cWaitMillis = millis();
  i2cCallComplete = true;
}

void parseI2Cdata()
{                                                       
  byte code = 254;                                                  // Used to hold the I2C response code.
  byte inChar = 0;                                                  // Used as a 1 byte buffer to store in bound bytes from the I2C Circuit.
  byte sensorBytesReceived = 0;                                     // We need to know how many characters bytes have been received
                                                        
  while (code == 254)                                               // In case the command takes longer to process, we keep looping here until we get a success or an error
  {
    Wire.requestFrom(channel, 48, 1);                               // Call the circuit and request 48 bytes (this is more then we need).
    code = Wire.read();
    while (Wire.available())                                        // Are there bytes to receive?
    {
      inChar = Wire.read();                                         // Receive a byte.

      if (inChar == 0)                                              // If we see that we have been sent a null command.
      {
        Wire.endTransmission();                                     // End the I2C data transmission.
        break;                                                      
      }
      else
      {
        sensorData[sensorBytesReceived] = inChar;                   // Load this byte into our array.
        sensorBytesReceived++;
      }
    }

    switch (code)
    {
      case 1:
        {
          if ((channel == 99) && (cmd[0] != 'T'))                   // Print the sensor data for pH to console and to ESP32 if it's a normal reading
          {
            char buff[20];
            sprintf(buff, "<PH:%s>", sensorData);
            Serial3.println(buff);
          }
          else if ((channel == 100) && (cmd != 'T'))                // Print the sensor data for EC to console and to ESP32 if it's a normal reading
          {
            char buff[20];
            sprintf(buff, "<EC:%s>", sensorData);
            Serial3.println(buff);
          }
          break;
        }

//    case 2:
//    {
//        Serial.println("command failed");
//        break;
//    }
//
//    case 254:
//    {
//        Serial.println("command pending");      // Means the command has not yet been finished calculating.
//        delay(200);                             // We wait for 200ms and give the circuit some time to complete the command
//        break;
//    }
//
//    case 255:
//    {
//      Serial.println("No Data");               // means there is no further data to send.
//      break;
//    }
    }
  }
  i2cCallComplete = false;
}

void checkForFlood()                                                  // I'm doing this with analog output of sensors, but this could switch to digital since I just want high or low really.
{ 
  int reading;
  for (int i = 0; i < NUM_ELEMENTS(waterSensorPins); i++)
  {
    analogRead(waterSensorPins[i]);
    delay(50);
    reading = analogRead(waterSensorPins[i]);
    if (reading <= 1000)                                        // If the analog reading is less than 1000, trigger flooding message with name of flooded area.
    {
      Serial3.print("<Flooding ");
      Serial3.print(waterSensorNames[i]);
      Serial3.println('>'); 
    }
  }
  floodStartMillis = millis();
}

void checkDrainBasin()
{
  drainBasinStatus = digitalRead(DRAIN_SENSOR_PIN);
  if (drainBasinStatus == HIGH)
  {
    Serial3.println("<Basin full!>");
  }
  else
  {
    Serial3.println("<Basin OK>");
  }
  drainBasinMillis = millis();
}

void checkWaterLvl()
{
  float liters = (scale.get_units());
  if (liters < 0)
  {
    Serial3.print("<WL:0.00>");
  }
  else
  {
    Serial3.print("<WL:");
    Serial3.print(liters);
    Serial3.println('>');
    
  }
  
  waterLvlMillis = millis();
}

void triggerRelay(int boardNumber, int relayNumber, int relayTrigger)
{
    char buff[50];
    sprintf(buff, "Triggering board#:%d, relay#:%d, state: %d",boardNumber,relayNumber,relayTrigger);
    Serial.println(buff);
    if (relayTrigger == 1)
    {
      digitalWrite(relayPins[boardNumber][relayNumber], LOW); // Turn relay ON
    }
    else if (relayTrigger == 0)
    {
      digitalWrite(relayPins[boardNumber][relayNumber], HIGH); // Turn relay OFF
    }
}

void setupScale()
{  
  char buff[80];
  sprintf(buff, "<HX711: Loaded calibration factor of %ld and zero factor of %ld>", mixingRes.calibrationFactor, mixingRes.zeroFactor);
  Serial3.println(buff);
  Serial.println(buff);
  scale.set_scale(mixingRes.calibrationFactor);               // Assign calibration factor from calibration process below
  delay(500);
  scale.set_offset(mixingRes.zeroFactor);                     // Zero out the scale using zero_factor from calibration process below
}

void beginCalMode()
{
  char buff[80];
  scaleCalMillis = currentMillis;
  Serial3.println("<HX711: Calibration starting.>");
  delay(1500);
  for(int i = 10; i >= 0; i--)
  {
    sprintf(buff, "<HX711: Remove ALL weight from sensors. You have %d seconds...>", i);
    Serial3.print(buff);
    delay(1000);
  }
  Serial3.println("<HX711: Taring scale...>");
  delay(2000);
  scale.set_scale();
  scale.tare(); //Reset the scale to 0
  mixingRes.calibrationFactor = -20000;
  for(int i = 10; i >= 0; i--)
  {
    sprintf(buff, "<HX711: Place stuff to be zeroed out on scale now! You have %d seconds...>", i);
    Serial3.print(buff);
    delay(1000);
  }
  Serial3.println("<HX711: Zero factor applied. Add known weight now and adjust calibration factor.>");
  mixingRes.zeroFactor = scale.read_average(); //Get a baseline reading
  Serial.print("zero factor: ");
  Serial.println(mixingRes.zeroFactor);
  scale.set_offset(mixingRes.zeroFactor);
  delay(1500);
  scaleCalMode = true;
}

void calibrateScale()
{
  scale.set_scale(mixingRes.calibrationFactor);               //Adjust the calibration factor
  Serial3.print("<HX711: ");
  Serial3.print(scale.get_units(), 2);
  Serial3.print("kg, ");                                      //Change this to lbs if you want and re-adjust the calibration factor for imperial values.
  Serial3.print("Calibration factor: ");
  Serial3.print(mixingRes.calibrationFactor);
  Serial3.println('>');
}
