#include <WiFi.h>
#include <Preferences.h>
#include "driver/rtc_io.h"
#include <HX711_ADC.h>
#include "time.h"
#include "sntp.h"
#if defined(ESP8266)|| defined(ESP32) || defined(AVR)
#endif
#define uS_TO_S 1000000
#define TIME_TO_SLEEP 15

RTC_DATA_ATTR int bootCount = 0;

//Network credentials
const char* ssid = "Connalls_iPhone";
const char* password = "123456789";

//pins:
const int HX711_dout = 4; //HX711 dout pin
const int HX711_sck = 5; //HX711 sck pin

const int DIR = 32;
const int STEP = 33;
const int  steps_per_rev = 200;

//HX711 constructor:
HX711_ADC LoadCell(HX711_dout, HX711_sck);

const int calVal_eepromAddress = 0;
unsigned long t = 0;

// Set web server port number to 80
WiFiServer server(80);

// Variable to store the HTTP request
String header;

Preferences preferences;

// Decode HTTP GET value
String valueString = String(5);

//Init variables
int hour1 = 0;
int hour2 = 0;
int portion;

int scheduleStart = 0;
int scheduleEnd = 0;
bool scheduleFlag;

int timeAwake;
int currentHour;

// Current time
unsigned long currentTime = millis();
// Previous time
unsigned long previousTime = 0; 
// Define timeout time in milliseconds (example: 2000ms = 2s)
const long timeoutTime = 20000;

//NTP server setup
const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "time.nist.gov";
const long  gmtOffset_sec = 3600;
const int   daylightOffset_sec = 3600;

const char* time_zone = "EST5EDT,M3.2.0,M11.1.0";  // TimeZone rule for Toronto including daylight adjustment rules (optional)

//NTP functions
void setTimezone(String timezone)
{
  //Serial.printf("Setting Timezone to %s\n",timezone.c_str());
  setenv("TZ",timezone.c_str(),1);  //  Now adjust the TZ.  Clock settings are adjusted to show the new local time
  tzset();
}

void printLocalTime()
{
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("No time available (yet)");
    return;
  }
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
}

int getLocalHour(){
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    return -1;
  }
  return timeinfo.tm_hour;
}

// Callback function (get's called when time adjusts via NTP)
void timeavailable(struct timeval *t)
{
  Serial.println("Got time adjustment from NTP!");
  printLocalTime();
}

void print_wakeup_reason(){
  esp_sleep_wakeup_cause_t wakeup_reason;

  wakeup_reason = esp_sleep_get_wakeup_cause();

  switch(wakeup_reason)
  {
    case ESP_SLEEP_WAKEUP_EXT0 : Serial.println("Wakeup caused by external signal using RTC_IO"); break;
    case ESP_SLEEP_WAKEUP_EXT1 : Serial.println("Wakeup caused by external signal using RTC_CNTL"); break;
    case ESP_SLEEP_WAKEUP_TIMER : Serial.println("Wakeup caused by timer"); break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD : Serial.println("Wakeup caused by touchpad"); break;
    case ESP_SLEEP_WAKEUP_ULP : Serial.println("Wakeup caused by ULP program"); break;
    default : Serial.printf("Wakeup was not caused by deep sleep: %d\n",wakeup_reason); break;
  }
}

void loadCellInit(){
  LoadCell.begin();
  //LoadCell.setReverseOutput(); //uncomment to turn a negative output value to positive
  float calibrationValue; // calibration value (see example file "Calibration.ino")
  calibrationValue = -471.44; // uncomment this if you want to set the calibration value in the sketch

  unsigned long stabilizingtime = 2000; // preciscion right after power-up can be improved by adding a few seconds of stabilizing time
  boolean _tare = true; //set this to false if you don't want tare to be performed in the next step
  LoadCell.start(stabilizingtime, _tare);
  if (LoadCell.getTareTimeoutFlag()) {
    Serial.println("Timeout, check MCU>HX711 wiring and pin designations");
    while (1);
  }
  else {
    LoadCell.setCalFactor(calibrationValue); // set calibration value (float)
    Serial.println("Load cell startup complete");
  }
}

void feed(int portion){
  int mass = 0;
  digitalWrite(DIR, 1);
  while (-mass < portion){
    static boolean newDataReady = 0;

    digitalWrite(STEP, HIGH);
    delayMicroseconds(1000);
    digitalWrite(STEP, LOW);
    delayMicroseconds(1000);
    // check for new data/start next conversion:
    if (LoadCell.update()){
      newDataReady = true;
    }

    // get smoothed value from the dataset:
    if (newDataReady) {
      mass = LoadCell.getData();
      Serial.print("Load_cell output val: ");
      Serial.println(-mass);
      newDataReady = 0;
    }

  }
}

void setup() {
  Serial.begin(115200);

  pinMode(STEP, OUTPUT);
  pinMode(DIR, OUTPUT);
  pinMode(2, OUTPUT);
  digitalWrite(2, LOW);

  bootCount++;

  print_wakeup_reason();

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0){
    timeAwake = 15000;
  }
  else{
    timeAwake = 0;
  }

  esp_sleep_enable_timer_wakeup(uS_TO_S * TIME_TO_SLEEP);
  rtc_gpio_pullup_dis(GPIO_NUM_12);
  rtc_gpio_pulldown_en(GPIO_NUM_12);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_12,1);

  // Connect to Wi-Fi network with SSID and password
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  // Print local IP address and start web server
  Serial.println("");
  Serial.println("WiFi connected.");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
  server.begin();
  digitalWrite(2, HIGH);

  // set notification call-back function for NTP
  sntp_set_time_sync_notification_cb( timeavailable );
  sntp_servermode_dhcp(1);    // (optional)
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);

  setTimezone("EST5EDT,M3.2.0,M11.1.0");

  printLocalTime();
  currentHour = getLocalHour();
  Serial.println("Current hour is: ");
  Serial.println(currentHour);
  preferences.begin("schedule_states", false);
  if (preferences.getInt("scheduleStart", 0) > 0){
    scheduleStart = preferences.getInt("scheduleStart", 0);
    Serial.println("Found scheduleStart in memory:");
    Serial.println(scheduleStart);
  }
  if (preferences.getInt("scheduleEnd", 0) > 0){
    scheduleEnd = preferences.getInt("scheduleEnd", 0);
    Serial.println("Found scheduleEnd in memory:");
    Serial.println(scheduleEnd);
  }
  if (preferences.getInt("portion", 0) > 0){
    portion = preferences.getInt("portion", 0);
    Serial.println("Found portion in memory:");
    Serial.println(portion);
  }
  scheduleFlag = preferences.getBool("scheduleFlag", 0);
  Serial.println("Found scheduleFlag in memory");
  Serial.println(scheduleFlag);

  preferences.end();

  loadCellInit();
}

void loop(){
  preferences.begin("schedule_states", false); 
  int wakeTime = millis();
  while (millis() - wakeTime < timeAwake){

    WiFiClient client = server.available();   // Listen for incoming clients

    if (client) {                             // If a new client connects,
      currentTime = millis();
      previousTime = currentTime;
      //Serial.println("New Client.");          // print a message out in the serial port
      String currentLine = "";                // make a String to hold incoming data from the client
      while (client.connected() && currentTime - previousTime <= timeoutTime) { // loop while the client's connected
        currentTime = millis();
        if (client.available()) {             // if there's bytes to read from the client,
          char c = client.read();             // read a byte, then
          //Serial.write(c);                    // print it out the serial monitor
          header += c;
          if (c == '\n') {                    // if the byte is a newline character
            // if the current line is blank, you got two newline characters in a row.
            // that's the end of the client HTTP request, so send a response:
            if (currentLine.length() == 0) {
              // HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
              // and a content-type so the client knows what's coming, then a blank line:
              client.println("HTTP/1.1 200 OK");
              client.println("Content-type:text/html");
              client.println("Connection: close");
              client.println();

              // Display the HTML web page
              client.println("<!DOCTYPE html><html>");
              client.println("<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
              client.println("<link rel=\"icon\" href=\"data:,\">");
              client.println("<style>body { text-align: center; font-family: \"Trebuchet MS\", Arial; margin-left:auto; margin-right:auto;}");
              client.println(".slider { width: 300px; }</style>");
              client.println("<script src=\"https://ajax.googleapis.com/ajax/libs/jquery/3.3.1/jquery.min.js\"></script>");
                      
              // Web Page
              client.println("</head><body><h1>Feeding Schedule Sliders</h1>");

              // First Slider
              client.println("<p>Breakfast Time: <span id=\"hour1\"></span>:00</p>");
              client.println("<input type=\"range\" min=\"0\" max=\"23\" class=\"slider\" id=\"scheduleSlider1\" onchange=\"schedule(1, this.value)\" value=\"" + valueString + "\"/>");

              // Second Slider
              client.println("<p>Dinner Time: <span id=\"hour2\"></span>:00</p>");
              client.println("<input type=\"range\" min=\"0\" max=\"23\" class=\"slider\" id=\"scheduleSlider2\" onchange=\"schedule(2, this.value)\" value=\"" + valueString + "\"/>");

              // Portion Slider
              client.println("<p>Portion Size: <span id=\"portion\"></span>g</p>");
              client.println("<input type=\"range\" min=\"0\" max=\"200\" class=\"slider\" id=\"scheduleSlider3\" onchange=\"schedule(3, this.value)\" value=\"" + valueString + "\"/>");

              client.println("<script>");
              client.println("var slider1 = document.getElementById(\"scheduleSlider1\");");
              client.println("var hour1 = document.getElementById(\"hour1\");");
              client.println("hour1.innerHTML = slider1.value;");
              client.println("slider1.oninput = function() {");
              client.println("  hour1.innerHTML = this.value;");
              client.println("};");

              client.println("var slider2 = document.getElementById(\"scheduleSlider2\");");
              client.println("var hour2 = document.getElementById(\"hour2\");");
              client.println("hour2.innerHTML = slider2.value;");
              client.println("slider2.oninput = function() {");
              client.println("  hour2.innerHTML = this.value;");
              client.println("};");

              client.println("var slider3 = document.getElementById(\"scheduleSlider3\");");
              client.println("var portion = document.getElementById(\"portion\");");
              client.println("portion.innerHTML = scheduleSlider3.value;");
              client.println("scheduleSlider3.oninput = function() {");
              client.println("  portion.innerHTML = this.value;");
              client.println("};");

              client.println("$.ajaxSetup({timeout: 1000});");
              client.println("function schedule(sliderNumber, hour) {");
              client.println("  $.get(\"/?slider=\" + sliderNumber + \"&value=\" + hour, { Connection: \"close\" });");
              client.println("}");
              client.println("</script>");

              client.println("</body></html>");
              
              //GET /?value=180& HTTP/1.1
              if (header.indexOf("GET /?slider=") >= 0) {

                int sliderNumStart = header.indexOf('=') + 1;
                int sliderNumEnd = header.indexOf('&');
                int sliderNumber = header.substring(sliderNumStart, sliderNumEnd).toInt();

                int hourStart = header.indexOf("value=") + 6;
                int hourEnd = header.indexOf(' ', hourStart);
                int hourValue = header.substring(hourStart, hourEnd).toInt();

                if (sliderNumber == 1) {
                  Serial.print("Slider 1 Value: ");
                  Serial.println(hourValue);
                  scheduleStart = hourValue;
                  preferences.putInt("scheduleStart", hourValue);
                  // Use 'hourValue' for Slider 1
                } else if (sliderNumber == 2) {
                  Serial.print("Slider 2 Value: ");
                  Serial.println(hourValue);
                  scheduleEnd = hourValue;
                  preferences.putInt("scheduleEnd", hourValue);
                  // Use 'hourValue' for Slider 2
                } else if (sliderNumber ==3){
                  Serial.print("Portion Size: ");
                  portion = hourValue;
                  Serial.println(portion);
                  preferences.putInt("portion", portion);
                }
              }
              // The HTTP response ends with another blank line
              client.println();
              // Break out of the while loop
              break;
            } else { // if you got a newline, then clear currentLine
              currentLine = "";
            }
          } else if (c != '\r') {  // if you got anything else but a carriage return character,
            currentLine += c;      // add it to the end of the currentLine
          }
        }
      }
      // Clear the header variable
      header = "";
      // Close the connection
      client.stop();
      //Serial.println("Client disconnected.");
      //Serial.println("");
    }
  }
  if (currentHour == scheduleStart && !scheduleFlag){
    Serial.println("Current hour is:");
    Serial.println(currentHour);
    Serial.println("Serving breakfast...");
    feed(portion);
    scheduleFlag = true;
    preferences.putBool("scheduleFlag", true);
  }
  else if (currentHour == scheduleEnd && scheduleFlag){
    Serial.println("Current hour is:");
    Serial.println(currentHour);
    Serial.println("Serving dinner...");
    feed(portion);
    scheduleFlag = false;
    preferences.putBool("scheduleFlag", false);
  }
  preferences.end();
  Serial.println("Going to sleep for 60s.");
  Serial.flush();
  esp_deep_sleep_start();
}
