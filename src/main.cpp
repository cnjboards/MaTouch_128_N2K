// N2K rotary display

// use to disable N2K
#define N2KENABLE
// used to populate displayn with simulated data, disable N2K when enabling simulate
//#define SIMULATE
#define SERIALDEBUG

// Includes
#include <Arduino.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include "touch.h"
#include "matouch-pins.h"
#include "matouch-n2k.h"
#include "matouch-display.h"
#include "matouch-simulate.h"
#include "otaWeb.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>

const char *host = "esp32";
const char *ssid = "n2kMaTouch128AP";
const char *password = "thisisfine";

WebServer server(80);

// Global vars
#define STARTUPDELAY 5000 // used for startup delay flag
bool startUpDelayDone=false;
bool fileUploadStarted=false;
unsigned long pm=0;
int counter = 0;
int State;
int old_State;

int move_flag = 0;
int button_flag = 0;
int flesh_flag = 1;

// allow control of the backlight
int backLightState = LOW;

// variables to handle the dial pb
int buttonState;            // the current reading from the input pin
int buttonStateLong;            // the current reading from the input pin
int lastButtonState = LOW;  // the previous reading from the input pin
int shortButtonStateLatched = LOW;  // latch if button is pressed for short interval
int longButtonStateLatched = LOW;  // latch if pressed for long interval
unsigned long lastDebounceTime = 0;  // the last time the output pin was toggled 
unsigned long shortDebounceDelay = 80;    // the short debounce time
unsigned long longDebounceDelay = 1000;    // the long debounce time
unsigned long latchClearDelay = 500;    // the time to allow latched states to be consumed befor autonomous reset

// use this for N2K
u_int32_t chipId;

// forward declarations
void pin_init();
void encoder_irq();
void checkButton(void);
void otaSetup(void);

// *****************************************************************************
void setup() {

  uint8_t chipid [ 6 ];

  // derive a unique chip id from the burned in MAC address
  esp_efuse_mac_get_default ( chipid );
  for ( int i = 0 ; i < 6 ; i++ )
  chipId += ( chipid [ i ] << ( 7 * i ));

  // Init USB serial port
  Serial.begin ( 115200 );

  // setup digital IO
  pin_init();

  // some serial outout, proof of life lol
  delay(1000);
  Serial.println("Test V1.1");
  Serial.printf ( "\nChip ID: %x\n" , chipId );
  
  // setup ota stuff
  otaSetup();

  // I2C setup
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Serial.printf ( "Wire Setup done");

  // setup N2K
  #ifdef N2KENABLE
  setupN2K();
  Serial.printf ( "N2K Setup done");
  #endif

  // setup the diaply
  doLvglInit();

  #ifdef SIMULATE
  simulateDataInit();
  Serial.printf ( "Simulate done");
  #endif

  // grab millis for startup delay
  pm = millis();

  Serial.printf ( "Setup Complete\n" , chipId );

  // wait for false initial button push and clear
  //if (shortButtonStateLatched == true);
  //  shortButtonStateLatched = false;
} // end setup

void onJavaScript(void) {
    Serial.println("onJavaScript(void)");
		server.setContentLength(jquery_min_js_v3_2_1_gz_len);
		server.sendHeader(F("Content-Encoding"), F("gzip"));
    server.send_P(200, "text/javascript", jquery_min_js_v3_2_1_gz, jquery_min_js_v3_2_1_gz_len);
}

/* setup function */
void otaSetup(void)
{
    // create a unique ssid based on constant + chipid
    char locBuf[128], locChipId[16];
    strcpy(locBuf, ssid);
    sprintf(locChipId,"-%x", chipId);
    strcat(locBuf, locChipId);
    
    IPAddress local_ip(192, 168, 1, 1);
    IPAddress local_mask(255,255,255,0);
    IPAddress gateway(192, 168, 1, 1);

    WiFi.softAP(locBuf,password);
    WiFi.softAPConfig(local_ip,gateway,local_mask);

    #ifdef SERIALDEBUG
      Serial.println("");
      Serial.print("AP set to ");
      Serial.println(locBuf);
      Serial.print("IP address: ");
      Serial.println(WiFi.softAPIP());
    #endif

    /*use mdns for host name resolution*/
    if (!MDNS.begin(host))
    { // http://esp32.local
        Serial.println("Error setting up MDNS responder!");
        while (1)
        {
            delay(1000);
        }
    }
    Serial.println("mDNS responder started");

    /*return javascript jquery */
    server.on("/jquery.min.js", HTTP_GET, onJavaScript);

    /*return index page which is stored in serverIndex */
    server.on("/", HTTP_GET, []()
              {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", loginIndex); });
    server.on("/serverIndex", HTTP_GET, []()
              {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", serverIndex); });
    /*handling uploading firmware file */
    server.on(
        "/update", HTTP_POST, []()
        {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    ESP.restart(); },
        []()
        {
            HTTPUpload &upload = server.upload();
            if (upload.status == UPLOAD_FILE_START)
            {
                Serial.printf("Update: %s\n", upload.filename.c_str());
                fileUploadStarted = true;
                if (!Update.begin(UPDATE_SIZE_UNKNOWN))
                { // start with max available size
                    Update.printError(Serial);
                }
            }
            else if (upload.status == UPLOAD_FILE_WRITE)
            {
                /* flashing firmware to ESP*/
                if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
                {
                    Update.printError(Serial);
                    fileUploadStarted = false;
                }
            }
            else if (upload.status == UPLOAD_FILE_END)
            {
                if (Update.end(true))
                { // true to set the size to the current progress
                    Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
                    fileUploadStarted = false;
                }
                else
                {
                    Update.printError(Serial);
                    fileUploadStarted = false;
                }
            }
        });
    server.begin();
}

// *****************************************************************************
// main processing loop
// *****************************************************************************
void loop() {

  // flag startup delay
  if (millis()-pm > STARTUPDELAY){
    startUpDelayDone = true;
  } // end if

  // check for Input (PB)
  checkButton();

  // web ota stuff
  server.handleClient();

  // process N2K
  #ifdef N2KENABLE
  doN2Kprocessing();
  #endif

  // only update display if not uploading, display glitches during the upload
  if (fileUploadStarted != true) {
    // Update display
    processDisplay();
  }
} // end loop

// setup all the IO
void pin_init()
{
  #if 0
  // dial PB
  pinMode(BUTTON_PIN,INPUT_PULLUP);

  // backlight display, leave on for now
  pinMode(TFT_BL,OUTPUT_OPEN_DRAIN);
  digitalWrite(TFT_BL, backLightState);

  // rotary encoder IO
  pinMode(ENCODER_CLK, INPUT_PULLUP);
  pinMode(ENCODER_DT, INPUT_PULLUP);
  old_State = digitalRead(ENCODER_CLK);
  attachInterrupt(ENCODER_CLK, encoder_irq, CHANGE);
  
  // setup tft backlight output
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  #endif
    pinMode(TFT_BLK, OUTPUT);
    digitalWrite(TFT_BLK, HIGH);

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(ENCODER_CLK, INPUT_PULLUP);
    pinMode(ENCODER_DT, INPUT_PULLUP);
    old_State = digitalRead(ENCODER_CLK);

    attachInterrupt(ENCODER_CLK, encoder_irq, CHANGE);

  // for debounce init
  lastButtonState = digitalRead(BUTTON_PIN);
  buttonState = lastButtonState;
  buttonStateLong = lastButtonState;


    //pinMode(ADC_INPUT_1, INPUT);

} // end pin_init

// this debounces the dial PB and will latch both a short and long press
void checkButton(void){
  // read the state of the switch into a local variable:
  int reading = digitalRead(BUTTON_PIN);

  // check to see if you just pressed the button
  // (i.e. the input went from LOW to HIGH), and you've waited long enough
  // since the last press to ignore any noise:

  // If the switch changed, due to noise or pressing:
  if (reading != lastButtonState) {
    // reset the debouncing timer
    lastDebounceTime = millis();
  } // end if

  if ((millis() - lastDebounceTime) > shortDebounceDelay) {
    // whatever the reading is at, it's been there for longer than the debounce
    // delay, so take it as the actual current state:

    // if the button state has changed:
    if (reading != buttonState) {
      buttonState = reading;

      // if we timeout on debounce then latch short button pressed
      if (buttonState == HIGH) {
        shortButtonStateLatched = HIGH;
        Serial.println("Dial button pressed, short debounce");
      } // end if
    } // end if
  } // end if

  if ((millis() - lastDebounceTime) > longDebounceDelay) {
    // whatever the reading is at, it's been there for longer than the debounce
    // delay, so take it as the actual current state:

    // if the button state has changed:
    if (reading != buttonStateLong) {
      buttonStateLong = reading;

      // if we timeout on debounce then latch short button pressed
      if (buttonState == HIGH) {
        longButtonStateLatched = HIGH;
        Serial.println("Dial button pressed, long debounce");
      } // end if
    } // end if
  } // end if

  // save the reading. Next time through the loop, it'll be the lastButtonState:
  lastButtonState = reading;

} // end checkButton

// irq handler for rotary encoder
void encoder_irq()
{
  State = digitalRead(ENCODER_CLK);
  if (State != old_State)
  {
    if (digitalRead(ENCODER_DT) == State)
    {
      counter++;
    }
    else
    {
      counter--;
    }
  }
  old_State = State; // the first position was changed
  move_flag = 1;
} // end encoder_irq
