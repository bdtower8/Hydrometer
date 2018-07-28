#include <ESP8266WiFi.h>
#include <WifiClient.h>
#include <ArduinoJson.h>

// wifi manager includes
#include <DNSServer.h>            //Local DNS Server used for redirecting all requests to the configuration portal
#include <ESP8266WebServer.h>     //Local WebServer used to serve the configuration portal
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager WiFi Configuration Magic

// accelerometer includes
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADXL345_U.h>

// temperature includes
#include <OneWire.h>
#include <DallasTemperature.h>

// use an adafruit accel
Adafruit_ADXL345_Unified accel = Adafruit_ADXL345_Unified(12345);

// calibrate the temperature sensor versus a more accurate thermometer
const float fTemperatureOffset = -2.0;

// DS18B20 pin
#define ONE_WIRE_BUS 2
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature DS18B20(&oneWire);

// wifi settings
WiFiManager wifiManager;

// OTA update
ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;

const char* update_path = "/firmware";
const char* update_username = "admin";
const char* update_password = "update_password";

// name the sensor something unique
const char *sensorId = "unique_id";

// parse server information
const char* appId = "AppId";
const char* restKey = "AppPassword";
const char *serverAddress = "AWS_PARSE_URL";
const char *tableLoc = "/parse/functions/";
const char *tableName = "addDataPoint";

// powers the DS18B20/ADXL345
const int iPowerPin = 13;

// query data every 30 minutes
const long deepSleepTime = 30 * 60 * 1000000;

// dynamic sleep
long lTimeBeforeSleep;

void handleRoot() {
  Serial.println("Alive");
  httpServer.send(200, "text/plain", "Alive!");
}

void handleNotFound() {
  httpServer.send (404, "text/plain", "Not found!");
}

// empty loop, deep-sleeping
void loop() {
  if(millis() > lTimeBeforeSleep) {    
    // go to deep sleep to save battery until the next reading
    Serial.println("going to sleep");
    ESP.deepSleep(deepSleepTime);
  }
  
  httpServer.handleClient();
}

void setup() {
  // set the serial baud rate
  Serial.begin(115200);
  
  // turn on power to the sensors
  pinMode(iPowerPin, OUTPUT);
  digitalWrite(iPowerPin, HIGH);

  // Connect to WiFi network
  wifiManager.autoConnect(sensorId);

  float fAngle = 180.0f;
  float fTemperature = 20.0f;
  
  // start the accel  
  accel.begin();
  
  // get the angle from the accelerometer
  if(!readAngle(fAngle)) {
    Serial.println("Failed to read angle");
  }

  // get the temperature from the ds18b20
  readTemperatureSensor(fTemperature);

  // turn off the power to the sensors
  digitalWrite(iPowerPin, LOW);

  // post the data to parse
  postDataToParse(fTemperature, fAngle);

  // if we are upside-down, delay deep-sleep by 3 minutes and allow user to setup the wifi
  if(fAngle > 135) {
    Serial.println("Staying alive longer");   
    lTimeBeforeSleep = 20 * 60 * 1000;
  } else {
    lTimeBeforeSleep = 0;
  }

  httpServer.on("/", handleRoot);
  httpServer.onNotFound(handleNotFound);
  
  // updater stuff
  MDNS.begin(sensorId);
  httpUpdater.setup(&httpServer, update_path, update_username, update_password);
  httpServer.begin();
  MDNS.addService("http", "tcp", 80);
  Serial.printf("HTTPUpdateServer ready! Open http://%s.local%s in your browser and login with username '%s' and password '%s'\n", sensorId, update_path, update_username, update_password);  
}

bool readAngle(float &fAngle)
{  
  // setup the accelerometer  
  accel.setRange(ADXL345_RANGE_2_G);
  
  // read values
  sensors_event_t accelEvent;
  accel.getEvent(&accelEvent);
  delay(500);
  accel.getEvent(&accelEvent);

  // calculate the angle from perpendicular
  float m_fXCal = 0.5f;
  float m_fYCal = 0.0f;
  float m_fZCal = 0.0f;

  // A.B = |A||B|cosT
  float fLHS = ((m_fXCal * accelEvent.acceleration.x) + (m_fYCal * accelEvent.acceleration.y) + (m_fZCal * accelEvent.acceleration.z));
  float fRHSCoeff = sqrt((m_fXCal * m_fXCal) + (m_fYCal * m_fYCal) + (m_fZCal * m_fZCal)) * sqrt((accelEvent.acceleration.x * accelEvent.acceleration.x) + (accelEvent.acceleration.y * accelEvent.acceleration.y) + (accelEvent.acceleration.z * accelEvent.acceleration.z));
  float fRatio = fLHS / fRHSCoeff;

  // limits detection
  if (isnan(fRatio))
    fRatio = 1.0f;
  if (fRatio > 1.0f)
    fRatio = 1.0f;
  if (fRatio < -1.0f)
    fRatio = -1.0f;

  // calculate the angle
  fAngle = acos(fRatio) * (180.0f / 3.14159f);
  
  Serial.print("Angle: "); 
  Serial.println(fAngle);
  
  return true;
}

void readTemperatureSensor(float &fTemperature) {  
  // read the temperature
  DS18B20.requestTemperatures();
  delay(500);
  DS18B20.requestTemperatures();
  fTemperature = DS18B20.getTempCByIndex(0) + fTemperatureOffset;  

  Serial.print("Temp C: ");
  Serial.println(fTemperature);  
}

void postDataToParse(const float &fTemperature, const float &fAngle) {
  // create the json objects for transmission
  StaticJsonBuffer<500> jsonBufferParent;
  JsonObject& jsonParent = jsonBufferParent.createObject();
  jsonParent["tempC"] = fTemperature;
  jsonParent["angle"] = fAngle;
  jsonParent["sensorId"] = sensorId;

  // prepare the json object for transmission
  String jsonOut;
  jsonParent.printTo(jsonOut);

  Serial.println("Starting Post");
  
  // create a secure https client
  WiFiClient client;
  if(client.connect(serverAddress, 80)) {
    client.print("POST ");    
    client.print(tableLoc);
    client.print(tableName);
    client.println(" HTTP/1.1");
    client.print("Host: ");
    client.println(serverAddress);
    client.println("Content-Type: application/json");
    
    client.print("X-Parse-Application-Id: ");
    client.println(appId);
    client.print("X-Parse-REST-API-Key: ");
    client.println(restKey);
    
    // add the payload
    client.print("Content-Length: ");
    client.println(jsonParent.measureLength());
    client.println();
    client.println(jsonOut);
    client.println();  

    // read the response from the network
    String reply = client.readString();

    Serial.println();  
    Serial.println("HTTP Reply:");
    Serial.println(reply);
    
  } else {
    Serial.print("Failed to connect to server: ");
    Serial.println(serverAddress);
  }
  
  Serial.println("Post Completed");
}

