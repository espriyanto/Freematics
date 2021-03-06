/******************************************************************************
* Reference sketch for a vehicle data feed for Freematics Hub
* Works with Freematics ONE with SIM800 GSM/GPRS module
* Developed by Stanley Huang https://www.facebook.com/stanleyhuangyc
* Distributed under BSD license
* Visit http://freematics.com/hub/api for Freematics Hub API reference
* Visit http://freematics.com/products/freematics-one for hardware information
* To obtain your Freematics Hub server key, contact support@freematics.com.au
* 
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
* THE SOFTWARE.
******************************************************************************/

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <FreematicsONE.h>
#include "config.h"
#include "datalogger.h"

// logger states
#define STATE_SD_READY 0x1
#define STATE_OBD_READY 0x2
#define STATE_GPS_READY 0x4
#define STATE_MEMS_READY 0x8
#define STATE_GSM_READY 0x10
#define STATE_CONNECTED 0x20
#define STATE_POSITIONED 0x40

uint16_t lastUTC = 0;
uint8_t lastGPSDay = 0;
uint32_t nextConnTime = 0;
uint16_t connCount = 0;
byte accCount = 0; // count of accelerometer readings
long accSum[3] = {0}; // sum of accelerometer data
int accCal[3] = {0}; // calibrated reference accelerometer data
byte deviceTemp = 0; // device temperature
int lastSpeed = 0;
uint32_t lastSpeedTime = 0;
uint32_t distance = 0;

typedef enum {
    GPRS_DISABLED = 0,
    GPRS_IDLE,
    GPRS_HTTP_RECEIVING,
    GPRS_HTTP_ERROR,
} GPRS_STATES;

typedef enum {
  HTTP_GET = 0,
  HTTP_POST,
} HTTP_METHOD;

typedef struct {
  float lat;
  float lng;
  uint8_t year; /* year past 2000, e.g. 15 for 2015 */
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
} GSM_LOCATION;

class COBDGSM : public COBDSPI {
public:
    COBDGSM():connErrors(0) { buffer[0] = 0; }
    void toggleGSM()
    {
        setTarget(TARGET_OBD);
        sendCommand("ATGSMPWR\r", buffer, sizeof(buffer));
    }
    bool initGSM()
    {
      // discard any stale data
      xbPurge();
      for (byte n = 0; n < 10; n++) {
        if (sendGSMCommand("ATE0\r") != 0) {
          return true;
        }
        // try turning on GSM
        toggleGSM();
        delay(2000);
      }
      return false;
    }
    bool setupGPRS(const char* apn)
    {
      uint32_t t = millis();
      bool success = false;
      do {
        success = sendGSMCommand("AT+CREG?\r", 5000, "+CREG: 0,") != 0;
        Serial.print('.'); 
      } while (!success && millis() - t < MAX_CONN_TIME);
      if (!success) return false;
      //sendGSMCommand("AT+CGATT?\r");
      sendGSMCommand("AT+SAPBR=3,1,\"Contype\",\"GPRS\"\r");
      sprintf_P(buffer, PSTR("AT+SAPBR=3,1,\"APN\",\"%s\"\r"), apn);
      sendGSMCommand(buffer, MAX_CONN_TIME);
      t = millis();
      do {
        sendGSMCommand("AT+SAPBR=1,1\r", 5000);
        sendGSMCommand("AT+SAPBR=2,1\r", 5000);
        success = !strstr_P(buffer, PSTR("0.0.0.0")) && !strstr_P(buffer, PSTR("ERROR"));
      } while (!success && millis() - t < MAX_CONN_TIME);
      return success;
    }
    int getSignal()
    {
        if (sendGSMCommand("AT+CSQ\r", 500)) {
            char *p = strchr(buffer, ':');
            if (p) {
              p += 2;
              int db = atoi(p) * 10;
              p = strchr(p, '.');
              if (p) db += *(p + 1) - '0';
              return db;
            }
        }
        return -1;
    }
    bool getOperatorName()
    {
        // display operator name
        if (sendGSMCommand("AT+COPS?\r") == 1) {
            char *p = strstr(buffer, ",\"");
            if (p) {
                p += 2;
                char *s = strchr(p, '\"');
                if (s) *s = 0;
                strcpy(buffer, p);
                return true;
            }
        }
        return false;
    }
    void httpUninit()
    {
      sendGSMCommand("AT+HTTPTERM\r");
    }
    bool httpInit()
    {
      if (!sendGSMCommand("AT+HTTPINIT\r", 3000) || !sendGSMCommand("AT+HTTPPARA=\"CID\",1\r", 3000)) {
        return false;
      }
      return true;
    }
    void httpConnect(HTTP_METHOD method)
    {
        // 0 for GET, 1 for POST
        char cmd[17];
        sprintf_P(cmd, PSTR("AT+HTTPACTION=%c\r"), '0' + method);
        setTarget(TARGET_BEE);
        write(cmd);
        bytesRecv = 0;
        checkTimer = millis();
    }
    byte httpIsConnected()
    {
        // may check for "ACTION: 0" for GET and "ACTION: 1" for POST
        byte ret = checkbuffer("ACTION:", MAX_CONN_TIME);
        if (ret == 1) {
          // success
          connErrors = 0;
        } else if (ret == 2) {
          // timeout
          connErrors++;
        }
        return ret;
    }
    bool httpRead()
    {
        if (sendGSMCommand("AT+HTTPREAD\r", MAX_CONN_TIME) && strstr(buffer, "+HTTPREAD:")) {
          return true;
        } else {
          Serial.println("READ ERROR");
          return false;
        }
    }
    bool getLocation(GSM_LOCATION* loc)
    {
      if (sendGSMCommand("AT+CIPGSMLOC=1,1\r", 3000)) do {
        char *p;
        if (!(p = strchr(buffer, ':'))) break;
        if (!(p = strchr(p, ','))) break;
        loc->lng = atof(++p);
        if (!(p = strchr(p, ','))) break;
        loc->lat = atof(++p);
        if (!(p = strchr(p, ','))) break;
        loc->year = atoi(++p) - 2000;
        if (!(p = strchr(p, '/'))) break;
        loc->month = atoi(++p);
        if (!(p = strchr(p, '/'))) break;
        loc->day = atoi(++p);
        if (!(p = strchr(p, ','))) break;
        loc->hour = atoi(++p);
        if (!(p = strchr(p, ':'))) break;
        loc->minute = atoi(++p);
        if (!(p = strchr(p, ':'))) break;
        loc->second = atoi(++p);
        return true;
      } while(0);
      return false;
    }
    byte checkbuffer(const char* expected, unsigned int timeout = 2000)
    {
      byte ret = xbReceive(buffer, sizeof(buffer), 100, expected);
      if (ret == 0) {
        // timeout
        return (millis() - checkTimer < timeout) ? 0 : 2;
      } else {
        return ret;
      }
    }
    bool sendGSMCommand(const char* cmd, unsigned int timeout = 2000, const char* expected = "OK")
    {
      xbWrite(cmd);
      delay(10);
      return xbReceive(buffer, sizeof(buffer), timeout, expected) != 0;
    }
    bool setPostPayload(const char* payload, int bytes)
    {
        // set HTTP POST payload data
        char cmd[24];
        sprintf_P(cmd, PSTR("AT+HTTPDATA=%d,1000\r"), bytes);
        if (!sendGSMCommand(cmd, 1000, "DOWNLOAD")) {
          return false;
        }
        // send cached data
        return sendGSMCommand(payload, 1000);
    }
    char buffer[128];
    byte bytesRecv;
    uint32_t checkTimer;
    byte connErrors;
};

class CTeleLogger : public COBDGSM, public CDataLogger
#if USE_MPU6050
,public CMPU6050
#elif USE_MPU9250
,public CMPU9250
#endif
{
public:
    CTeleLogger():gprsState(GPRS_DISABLED),state(0),feedid(0) {}
    void setup()
    {
#if USE_MPU6050 || USE_MPU9250
        if (!(state & STATE_MEMS_READY)) {
          Serial.print("#MEMS...");
          if (memsInit()) {
            state |= STATE_MEMS_READY;
            Serial.println("OK");
          } else {
            Serial.println("NO");
          }
        }
#endif

        for (;;) {
          // initialize OBD communication
          Serial.print("#OBD...");
          if (init()) {
            Serial.println("OK");
          } else {
            Serial.println("NO");
            standby();
            continue;
          }
          state |= STATE_OBD_READY;

#if USE_GPS
          // start serial communication with GPS receive
          Serial.print("#GPS...");
          if (initGPS(GPS_SERIAL_BAUDRATE)) {
            state |= STATE_GPS_READY;
            Serial.println("OK");
          } else {
            Serial.println("NO");
          }
#endif

          // initialize SIM800L xBee module (if present)
          Serial.print("#GSM...");
          // setup xBee module serial baudrate
          xbBegin(XBEE_BAUDRATE);
          // turn on power for GSM module which needs some time to boot
          toggleGSM();
          if (initGSM()) {
            state |= STATE_GSM_READY;
            Serial.println("OK");
          } else {
            standby();
            continue;
          }

          Serial.print("#GPRS(APN:");
          Serial.print(APN);
          Serial.print(")...");
          if (setupGPRS(APN)) {
            Serial.println("OK");
          } else {
            Serial.println("NO (check SIM card & APN setting)");
            Serial.print("#SIGNAL:");
            Serial.println(getSignal());
            standby();
            continue;
          }
          
          // init HTTP
          Serial.print("#HTTP...");
          for (byte n = 0; n < 5; n++) {
            if (httpInit()) {
              state |= STATE_CONNECTED;
              gprsState = GPRS_IDLE;
              break;
            }
            Serial.print('.');
            httpUninit();
            delay(3000);
          }
          if (state & STATE_CONNECTED) {
            Serial.println("OK");
          } else {
            Serial.println(buffer);
            standby();
            continue;
          }

          // retrieve VIN and GSM signal index
          int signal = getSignal();
          Serial.print("#SIGNAL:");
          Serial.println(signal);

          char vin[20];
          // retrieve VIN
          if (getVIN(buffer, sizeof(buffer))) {
            strncpy(vin, buffer, sizeof(vin) - 1);
            Serial.print("#VIN:");
            Serial.println(vin);
          } else {
            strcpy(vin, "NO_VIN");            
          }

          // sign in server
          if (!regDataFeed(0, vin, signal)) {
            standby(); 
            continue;
          }

          break;
        }

        calibrateMEMS();
    }
    bool regDataFeed(byte action, const char* vin = 0, int csq = 0)
    {
      // action == 0 for registering a data feed, action == 1 for de-registering a data feed
      gprsState = GPRS_IDLE;
      for (byte n = 0; ;n++) {
        if (action == 0) {
          // error handling
          if (readSpeed() == -1 || n >= MAX_CONN_ERRORS) {
            return false;
          }
        } else {
          if (n >= MAX_CONN_ERRORS) {
            return false;
          } 
        }
        char *p = buffer;
        p += sprintf_P(buffer, PSTR("AT+HTTPPARA=\"URL\",\"%s/reg?"), SERVER_URL);
        if (action == 0) {
          sprintf_P(p, PSTR("CSQ=%d&vin=%s\"\r"), csq, vin);
        } else {
          sprintf_P(p, PSTR("id=%u&off=1\"\r"), feedid);
        }
        Serial.print("#SERVER");
        if (!sendGSMCommand(buffer)) {
          Serial.println(buffer);
          delay(3000);
          continue;
        }
        httpConnect(HTTP_GET);
        do {
          delay(500);
          Serial.print('.');
        } while (httpIsConnected() == 0);
        if (gprsState == GPRS_HTTP_ERROR) {
          continue;
        }
        if (action != 0) {
          Serial.println(); 
          return true;
        }
        if (httpRead()) {
          char *p = strstr(buffer, "\"id\":");
          if (p) {
            int m = atoi(p + 5);
            if (m > 0) {
              feedid = m;
              Serial.println(m);
              state |= STATE_CONNECTED;
              break;
            }
          }            
        }
        Serial.println(buffer);
      }
      return true;
    }
    void loop()
    {
        uint32_t start = millis();

        if (!(state & STATE_OBD_READY)) {
          setup();
          return;
        }
        processOBD();

#if USE_MPU6050 || USE_MPU9250
        if (state & STATE_MEMS_READY) {
            processMEMS();  
        }
#endif

        if (state & STATE_GPS_READY) {
#if USE_GPS
          processGPS();
#endif
#if USE_GSM_LOCATION
        } else {
          processGSMLocation();
#endif
        }

        // read and log car battery voltage , data in 0.01v
        int v = getVoltage() * 100;
        dataTime = millis();
        logData(PID_BATTERY_VOLTAGE, v);

        do {
          if (millis() > nextConnTime) {
#if USE_GSM_LOCATION
            if (!(state & STATE_GPS_READY)) {
              if (gprsState == GPRS_IDLE && !(state & STATE_POSITIONED)) {
                // get GSM location if GPS not present
                if (getLocation(&loc)) {
                  //Serial.print(buffer);
                }
              }
            }
#endif
            // process HTTP state machine
            processGPRS();

            // continously read speed for calculating trip distance
            if (state & STATE_OBD_READY) {
               readSpeed();
            }
            // error and exception handling
            if (gprsState == GPRS_IDLE) {
              if (errors > 10) {
                reconnect();
                return;
              } else if (deviceTemp >= COOLING_DOWN_TEMP && deviceTemp < 100) {
                // device too hot, cool down
                Serial.print("Cooling (");
                Serial.print(deviceTemp);
                Serial.println("C)");
                delay(5000);
                break;
              }
            }        
          }
        } while (millis() - start < MIN_LOOP_TIME);

        // error handling
        if (connErrors >= MAX_CONN_ERRORS) {
          // reset GPRS 
          Serial.print("Reset GPRS...");
          initGSM();
          setupGPRS(APN);
          if (httpInit()) {
            Serial.println("OK"); 
            gprsState = GPRS_IDLE;
          } else {
            Serial.println(buffer); 
          }
          connErrors = 0;
        }
    }
private:
    void processGPRS()
    {
        switch (gprsState) {
        case GPRS_IDLE:
            if (state & STATE_CONNECTED) {
#if !ENABLE_DATA_CACHE
                char cache[16];
                int v = getVoltage() * 100;
                byte cacheBytes = sprintf(cache, "#%lu,%u,%d ", millis(), PID_BATTERY_VOLTAGE, v);
                Serial.println(v);
#endif
                // generate URL
                sprintf_P(buffer, PSTR("AT+HTTPPARA=\"URL\",\"%s/post?id=%u\"\r"), SERVER_URL, feedid);
                if (!sendGSMCommand(buffer)) {
                  break;
                }
                // replacing last space with
                cache[cacheBytes - 1] = '\r';
                if (setPostPayload(cache, cacheBytes - 1)) {
                  // success
                  // output payload data to serial
                  //Serial.println(cache);
                  Serial.print("#");
                  Serial.print(++connCount);
                  Serial.print(' ');
                  Serial.print(cacheBytes - 1);
                  Serial.print("B ");
#if ENABLE_DATA_CACHE
                  purgeCache();
#endif
                  state &= ~STATE_POSITIONED; // to be positioned again
                  delay(10);
                  httpConnect(HTTP_POST);
                  gprsState = GPRS_HTTP_RECEIVING;
                  nextConnTime = millis() + 200;
                } else {
                  Serial.println(buffer);
                  gprsState = GPRS_HTTP_ERROR;
                }
            }
            break;        
        case GPRS_HTTP_RECEIVING:
            switch (httpIsConnected()) {
            case 0:
              // not yet connected
              nextConnTime = millis() + 200;
              break;
            case 1:
              // connected
              if (httpRead()) {
                // success
                Serial.println("OK");
                //Serial.println(buffer);
                gprsState = GPRS_IDLE;
                break;
              }
            case 2:
              // error occurred
              gprsState = GPRS_HTTP_ERROR;
              break;
            }
            break;
        case GPRS_HTTP_ERROR:
            Serial.println("HTTP error");
            //Serial.println(buffer);
            connCount = 0;
            xbPurge();
            httpUninit();
            delay(200);
            if (httpInit()) {
              gprsState = GPRS_IDLE;
              nextConnTime = millis() + 500;
            } else {
              Serial.println("Failed to reconnect");
              connErrors = MAX_CONN_ERRORS;
              nextConnTime = millis() + 3000;
            }
            break;
        }
    }
    void processOBD()
    {
        int speed = readSpeed();
        if (speed == -1) {
          return;
        }
        logData(0x100 | PID_SPEED, speed);
        logData(PID_TRIP_DISTANCE, distance);
        // poll more PIDs
        byte pids[]= {0, PID_RPM, PID_ENGINE_LOAD, PID_THROTTLE};
        const byte pidTier2[] = {PID_INTAKE_TEMP, PID_COOLANT_TEMP};
        static byte index2 = 0;
        int values[sizeof(pids)] = {0};
        // read multiple OBD-II PIDs, tier2 PIDs are less frequently read
        pids[0] = pidTier2[index2 = (index2 + 1) % sizeof(pidTier2)];
        byte results = readPID(pids, sizeof(pids), values);
        if (results == sizeof(pids)) {
          for (byte n = 0; n < sizeof(pids); n++) {
            logData(0x100 | pids[n], values[n]);
          }
        }
    }
    int readSpeed()
    {
        int value;
        if (readPID(PID_SPEED, value)) {
           dataTime = millis();
           distance += (value + lastSpeed) * (dataTime - lastSpeedTime) / 3600 / 2;
           lastSpeedTime = dataTime;
           lastSpeed = value;
           return value;
        } else {
          return -1; 
        }
    }
#if USE_MPU6050 || USE_MPU9250
    void processMEMS()
    {
         // log the loaded MEMS data
        if (accCount) {
          logData(PID_ACC, accSum[0] / accCount / ACC_DATA_RATIO, accSum[1] / accCount / ACC_DATA_RATIO, accSum[2] / accCount / ACC_DATA_RATIO);
        }
    }
#endif
    void processGPS()
    {
        GPS_DATA gd = {0};
        // read parsed GPS data
        if (getGPSData(&gd)) {
            if (lastUTC != (uint16_t)gd.time) {
              dataTime = millis();
              byte day = gd.date / 10000;
              logData(PID_GPS_TIME, gd.time);
              if (lastGPSDay != day) {
                logData(PID_GPS_DATE, gd.date);
                lastGPSDay = day;
              }
              logCoordinate(PID_GPS_LATITUDE, gd.lat);
              logCoordinate(PID_GPS_LONGITUDE, gd.lng);
              logData(PID_GPS_ALTITUDE, gd.alt);
              logData(PID_GPS_SPEED, gd.speed);
              logData(PID_GPS_SAT_COUNT, gd.sat);
              lastUTC = (uint16_t)gd.time;
              state |= STATE_POSITIONED;
            }
            //Serial.print("#UTC:"); 
            //Serial.println(gd.time);
        } else {
          Serial.println("No GPS data");
          xbPurge();
        }
    }
    void processGSMLocation()
    {
        uint32_t t = (uint32_t)loc.hour * 1000000 + (uint32_t)loc.minute * 10000 + (uint32_t)loc.second * 100;
        if (lastUTC != (uint16_t)t) {
          dataTime = millis();
          logData(PID_GPS_TIME, t);
          if (lastGPSDay != loc.day) {
            logData(PID_GPS_DATE, (uint32_t)loc.day * 10000 + (uint32_t)loc.month * 100 + loc.year);
            lastGPSDay = loc.day;
          }
          logCoordinate(PID_GPS_LATITUDE, loc.lat * 1000000);
          logCoordinate(PID_GPS_LONGITUDE, loc.lng * 1000000);
          lastUTC = (uint16_t)t;
          state |= STATE_POSITIONED;
        }
    }
    void reconnect()
    {
        // try to re-connect to OBD
        if (init()) return;
        standby();
    }
    void standby()
    {
#if USE_GPS
        if (state & STATE_GPS_READY) {
          initGPS(0); // turn off GPS power
          Serial.println("GPS shutdown");
        }
#endif
        if (state & STATE_GSM_READY) {
          if (state & STATE_CONNECTED) {
            regDataFeed(1); // de-register
          }
          toggleGSM(); // turn off GSM power
          Serial.println("GSM shutdown");
        }
        state &= ~(STATE_OBD_READY | STATE_GPS_READY | STATE_GSM_READY | STATE_CONNECTED);
        Serial.print("Standby");
        // put OBD chips into low power mode
        enterLowPowerMode();
        // sleep for serveral seconds
        for (byte n = 0; n < RECALIBRATION_TIME / 210; n++) {
          Serial.print('.');
          readMEMS();
          delay(200);
        }
        // re-calibrate MEMS sensor with past data
        calibrateMEMS();
        for (;;) {
          accSum[0] = 0;
          accSum[1] = 0;
          accSum[2] = 0;
          for (accCount = 0; accCount < 50; ) {
            readMEMS();
            delay(50);
          }
          // calculate relative movement
          unsigned long motion = 0;
          for (byte i = 0; i < 3; i++) {
            long n = accSum[i] / accCount - accCal[i];
            motion += n * n;
          }
          // check movement
          if (motion > WAKEUP_MOTION_THRESHOLD) {
            Serial.println(motion);
            leaveLowPowerMode();
            break;
          }
          Serial.print('.');
        }
        Serial.println("Wakeup");
    }
    void reset()
    {
        // reset device
        void(* resetFunc) (void) = 0; //declare reset function at address 0
        resetFunc();
    }
    void calibrateMEMS()
    {
        // get accelerometer calibration reference data
        accCal[0] = accSum[0] / accCount;
        accCal[1] = accSum[1] / accCount;
        accCal[2] = accSum[2] / accCount;
    }
    void readMEMS()
    {
        // load accelerometer and temperature data
        int acc[3] = {0};
        int temp; // device temperature (in 0.1 celcius degree)
        memsRead(acc, 0, 0, &temp);
        if (accCount >= 250) {
          accSum[0] >>= 1;
          accSum[1] >>= 1;
          accSum[2] >>= 1;
          accCount >>= 1;
        }
        accSum[0] += acc[0];
        accSum[1] += acc[1];
        accSum[2] += acc[2];
        accCount++;
        deviceTemp = temp / 10;
    }
    void dataIdleLoop()
    {
      // do something while waiting for data on SPI
      if (state & STATE_MEMS_READY) {
        readMEMS();
      }
      delay(20);
    }
    byte state;
    byte gprsState;
    uint16_t feedid;
    GSM_LOCATION loc;
};

CTeleLogger logger;

void setup()
{
    // initialize hardware serial (for USB and BLE)
    Serial.begin(115200);
 #if USE_MPU6050 || USE_MPU9250
    // start I2C communication 
    Wire.begin();
#endif
    delay(500);
    // this will also init SPI communication
    logger.begin();
}

void loop()
{
    logger.loop();
}
