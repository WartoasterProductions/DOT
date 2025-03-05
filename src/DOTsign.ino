#include <NTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_IS31FL3731.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include "font.h"
#include <math.h>
#include <WiFiUdp.h>
#include <OpenWeather.h>
#include <ArduinoOTA.h>

// ----------------------- Global Configuration -----------------------
#define MAX_PAGES 100            
int NUM_PAGES = 4;
#define TCA9548A_ADDRESS 0x70
#define AP_SSID "DOTsign"
#define DNS_PORT 53

const uint8_t DISPLAY_WIDTH = 40;
const uint8_t DISPLAY_HEIGHT = 21;

uint8_t frameBuffer[DISPLAY_WIDTH][DISPLAY_HEIGHT];  // 40x21 display buffer
uint8_t bigBuffer[DISPLAY_WIDTH][27];                // "Real" display size buffer

int currentMode = 0;      // 0: Pages, 1: Menu, 2: Clock, 3: Weather, 4: Snake, 5: DVD, 6: Edit, 7: WiFi
int ON_VALUE = 64, DELAY_TIME = 3000;
unsigned long previousPageTime = 0, previousDrawTime = 0, lastButtonPressTime = 0;
bool changeNow = false, forceUpdate = false;
int currentPage = 0;

// Weather variables
OW_Weather ow;
String api_key, latitude, longitude, units = "imperial", language = "en";
unsigned long lastWeatherUpdate = 0;
const unsigned long WEATHER_UPDATE_INTERVAL = 5 * 60 * 1000; // 5 minutes

// WiFi and connection
bool connectInProgress = false;
unsigned long connectStartTime = 0;
String pendingSSID, pendingPass;

// Button pins and debounce settings
#define BTN_LEFT    14
#define BTN_RIGHT   2  //retail
#define BTN_SELECT  12 //retail
// #define BTN_RIGHT   12 //prototype
// #define BTN_SELECT  2  //prototype
#define BTN_UP      13
#define BTN_DOWN    0
const unsigned long debounceDelay = 80;
bool buttonDown = false;

// Remote control flags
volatile bool remote_UP = false, remote_DOWN = false, remote_LEFT = false, remote_RIGHT = false, remote_SELECT = false;

// Menu
#define MENU_ITEMS 9
const char *menuOptions[MENU_ITEMS] = { "EDIT", "DISPLAY", "CLOCK", "WEATHER", "WIFI", "GAME", "SCRNSAVR", "STREAM", "RESET" };
int menuSelected = 0;
bool menuOpen = false, ignoreMenu = false;

// Snake game
#define SNAKE_MAX_LENGTH (DISPLAY_WIDTH * DISPLAY_HEIGHT)
int snakeLength = 5, snakeDirection = 1;  // 0: Up, 1: Right, 2: Down, 3: Left
int snakeX[SNAKE_MAX_LENGTH], snakeY[SNAKE_MAX_LENGTH];
int foodX, foodY;
bool foodEaten = false, snake = false;

// Loading animation for weather
String loadingBar[4] = { "|", "/", "-", "\\" };
unsigned long lastLoadingUpdate = 0;
int loadingIndex = 0;

// Networking, OTA, and time
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
DNSServer dnsServer;
AsyncWebServer server(80);
IPAddress apIP(8, 8, 8, 8);
IPAddress netMsk(255, 255, 255, 0);
WiFiUDP udpReceiver;
const uint16_t UDP_FRAME_PORT = 42069;



// Matrix driver and addresses
Adafruit_IS31FL3731 matrices[8];
uint8_t matrixAddresses[] = { 0x74, 0x75, 0x76, 0x77 };

// File storage for pages (3 lines per page)
String pageData[MAX_PAGES][3];

// Global time offset (for NTP)
int timeOffset = 0;

// ----------------------- Utility Functions -----------------------

void clearBuffer() {
  memset(frameBuffer, 0, sizeof(frameBuffer));
  memset(bigBuffer, 0, sizeof(bigBuffer));
}

void setPixel(int x, int y, uint8_t brightness) {
  if (x >= 0 && x < DISPLAY_WIDTH && y >= 0 && y < DISPLAY_HEIGHT)
    frameBuffer[x][y] = brightness;
}

char wrapChar(int ch) {
  if (ch < 32) return 126;
  if (ch > 126) return 32;
  return ch;
}

void drawCharToBuffer(int x, int y, char c) {
  if (c < 0x20 || c > 0x7F) return;
  int idx = c - 0x20;
  for (uint8_t i = 0; i < 5; i++) {
    uint8_t col = font[idx][i];
    for (uint8_t j = 0; j < 7; j++) {
      if (col & (1 << j))
        setPixel(x + i, y + j, ON_VALUE);
    }
  }
}

void drawText(int x, int y, const char *text) {
  for (int i = 0; text[i] != '\0'; i++) {
    drawCharToBuffer(x + i * 5, y, text[i]);
  }
}

void drawText(int x, int y, const String &text) {
  drawText(x, y, text.c_str());
}

void drawCharToBigBuffer(int x, int y, char c) {
  if (c < 0x20 || c > 0x7F) return;
  int idx = c - 0x20;
  for (uint8_t i = 0; i < 5; i++) {
    uint8_t col = font[idx][i];
    for (uint8_t j = 0; j < 7; j++) {
      if (col & (1 << j))
        realPixel(x + i, y + j, ON_VALUE);
    }
  }
}

void drawTextBigBuffer(int x, int y, const char* text) {
  for (int i = 0; text[i] != '\0'; i++) {
    drawCharToBigBuffer(x + i * 5, y, text[i]);
  }
}


// ----------------------- Multiplexer & Matrix Setup -----------------------
void tcaSelect(uint8_t channel) {
  if (channel > 7) return;
  Wire.beginTransmission(TCA9548A_ADDRESS);
  Wire.write(1 << channel);
  Wire.endTransmission();
}

void setupMatrices() {
  uint8_t matrixIndex = 0;
  for (uint8_t ch = 0; ch < 2; ch++) {
    tcaSelect(ch);
    for (uint8_t i = 0; i < 4; i++) {
      if (matrices[matrixIndex].begin(matrixAddresses[i])) {
        matrices[matrixIndex].clear();
      }
      matrixIndex++;
    }
  }
}

// ----------------------- File I/O Functions -----------------------
void loadFromLittleFS() {
  for (int i = 0; i < NUM_PAGES; i++) {
    String fname = "/page" + String(i) + ".txt";
    File file = LittleFS.open(fname, "r");
    if (file && file.size() > 6) {
      for (int j = 0; j < 3; j++) {
        pageData[i][j] = file.readStringUntil('\n');
        pageData[i][j].trim();
      }
      file.close();
    } else {
      for (int j = 0; j < 3; j++)
        pageData[i][j] = "";
    }
  }
}

void saveToLittleFS() {
  for (int i = 0; i < NUM_PAGES; i++) {
    String fname = "/page" + String(i) + ".txt";
    File file = LittleFS.open(fname, "w");
    if (file) {
      for (int j = 0; j < 3; j++) {
        file.println(pageData[i][j]);
      }
      file.close();
    }
  }
}

void saveDisplaySettings() {
  File file = LittleFS.open("/display_settings.txt", "w");
  if (file) {
    file.println(ON_VALUE);
    file.println(DELAY_TIME);
    file.close();
  }
}

void loadDisplaySettings() {
  File file = LittleFS.open("/display_settings.txt", "r");
  if (file) {
    String brightness = file.readStringUntil('\n');
    String delay = file.readStringUntil('\n');
    brightness.trim();
    delay.trim();
    
    if (brightness.length() > 0) {
      ON_VALUE = brightness.toInt();
      // Ensure valid range
      if (ON_VALUE < 16) ON_VALUE = 16;
      if (ON_VALUE > 255) ON_VALUE = 255;
    }
    
    if (delay.length() > 0) {
      DELAY_TIME = delay.toInt();
      // Ensure valid range
      if (DELAY_TIME < 500) DELAY_TIME = 500;
      if (DELAY_TIME > 5000) DELAY_TIME = 5000;
    }
    file.close();
  }
}

// ----------------------- Weather & Time Functions -----------------------
void readWeather() {
  File file = LittleFS.open("/weather.txt", "r");
  if (file && file.size() > 10) {
    api_key = file.readStringUntil('\n');
    latitude = file.readStringUntil('\n');
    longitude = file.readStringUntil('\n');
    String readUnits = file.readStringUntil('\n');
    if (readUnits.length() > 0) units = readUnits;
    file.close();
  }
}

void loadTimeSettings() {
  if (!LittleFS.exists("/time.txt")) {
    Serial.println("No time settings file found. Using defaults.");
    return;
  }
  File timeFile = LittleFS.open("/time.txt", "r");
  if (!timeFile) { Serial.println("Failed to open /time.txt"); return; }
  String serverLine = timeFile.readStringUntil('\n'); serverLine.trim();
  String tzLine = timeFile.readStringUntil('\n'); tzLine.trim();
  timeOffset = tzLine.toInt();
  timeFile.close();
  timeClient.setPoolServerName(serverLine.c_str());
  timeClient.setTimeOffset(timeOffset * 3600);
  timeClient.update();
}

// Loading and error screens for weather
void displayWeatherLoading() {
  clearBuffer();
  drawText(0, 0, "LOADING");
  drawText(0, 7, "WEATHER");
  drawText(0, 14, "DATA ");
  drawDisplay();
}

void displayWeatherError() {
  clearBuffer();
  drawText(0, 0, "NO API");
  drawText(0, 7, "   KEY");
  drawDisplay();
}

void updateWeatherData() {
  if (api_key.length() == 0) {
    displayWeatherError();
    return;
  }

  displayWeatherLoading();
  api_key.trim();
  latitude.trim();
  longitude.trim();
  units.trim();
  language.trim();
  OW_forecast *forecast = new OW_forecast;
  ow.getForecast(forecast, api_key, latitude, longitude, units, language, false);
  if (forecast) {
    String line1 = String(forecast->temp[0], 1) + "~";
    String line2 = String(int(forecast->pop[0] * 100)) + "% " + String(forecast->humidity[0]) + "%h";
    String line3 = abbreviateCondition(forecast->description[0]);
    clearBuffer();
    drawText(0, 0, line1);
    drawText(0, 7, line2);
    drawText(0, 14, line3);
    drawDisplay();
  }
  delete forecast;
}

String abbreviateCondition(String cond) {
  cond.toLowerCase();
  if (cond.indexOf("thunderstorm") != -1) return "TStorm";
  if (cond.indexOf("drizzle") != -1) return "Drizzle";
  if (cond.indexOf("rain") != -1) return "Rain";
  if (cond.indexOf("snow") != -1) return "Snow";
  if (cond.indexOf("mist") != -1) return "Mist";
  if (cond.indexOf("smoke") != -1) return "Smoke";
  if (cond.indexOf("haze") != -1) return "Haze";
  if (cond.indexOf("dust") != -1) return "Dust";
  if (cond.indexOf("fog") != -1) return "Fog";
  if (cond.indexOf("sand") != -1) return "Sand";
  if (cond.indexOf("ash") != -1) return "Ash";
  if (cond.indexOf("squall") != -1) return "Squall";
  if (cond.indexOf("tornado") != -1) return "Tornado";
  if (cond.indexOf("clear") != -1) return "Clear";
  if (cond.indexOf("few clouds") != -1) return "FewClds";
  if (cond.indexOf("scattered clouds") != -1) return "PartCldy";
  if (cond.indexOf("broken clouds") != -1) return "BrknClds";
  if (cond.indexOf("overcast clouds") != -1) return "Overcast";
  if (cond.indexOf("cloud") != -1) return "Cloudy";
  return cond.substring(0, min((int)cond.length(), 8));
}

void checkWeatherUpdate() {
  if (millis() - lastWeatherUpdate >= WEATHER_UPDATE_INTERVAL || forceUpdate) {
    lastWeatherUpdate = millis();
    updateWeatherData();
    forceUpdate = false;
  }
}

// ----------------------- Edit Page Functions -----------------------

// Temporary buffer for editing page 0 (3x8)
char page0[3][8];
static int cursorRow = 0, cursorCol = 0;
static char currentSelection = 'A';

void initPageEdit() {
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 8; c++)
      page0[r][c] = ' ';
  cursorRow = 0; cursorCol = 0;
  currentSelection = 'A';
}


void editPageData() {
  // Combine physical and remote button states
  bool btnUp    = (!digitalRead(BTN_UP))    || remote_UP;
  bool btnDown  = (!digitalRead(BTN_DOWN))  || remote_DOWN;
  bool btnRight = (!digitalRead(BTN_RIGHT)) || remote_RIGHT;
  bool btnLeft  = (!digitalRead(BTN_LEFT))  || remote_LEFT;
  bool btnSelect= (!digitalRead(BTN_SELECT))|| remote_SELECT;
  
  // Process UP: cycle currentSelection upward (wraps if necessary)
  if (btnUp) {
    currentSelection = wrapChar(currentSelection - 1);
    remote_UP = false;
  }
  
  // Process DOWN: cycle currentSelection downward
  if (btnDown) {
    currentSelection = wrapChar(currentSelection + 1);
    remote_DOWN = false;
  }
  
  // Process RIGHT: confirm current character and advance cursor;
  // note: do not update currentSelection from the new cell.
  if (btnRight) {
    page0[cursorRow][cursorCol] = currentSelection;
    cursorCol = (cursorCol + 1) % 8;
    if (cursorCol == 0) {
      cursorRow = (cursorRow + 1) % 3;
    }
    remote_RIGHT = false;
  }
  
  // Process LEFT: move cursor left (like a backspace) and clear that cell.
  if (btnLeft) {
    if (cursorCol == 0 && cursorRow > 0) { 
      cursorRow--; 
      cursorCol = 7; 
    } else if (cursorCol > 0) { 
      cursorCol--; 
    }
    page0[cursorRow][cursorCol] = ' ';
    remote_LEFT = false;
  }
  
  // Process SELECT: confirm editing (copy temporary page0 to persistent pageData)
  if (btnSelect) {
    for (int r = 0; r < 3; r++) {
      String line = "";
      for (int c = 0; c < 8; c++) {
        line += page0[r][c];
      }
      pageData[0][r] = line;
    }
    saveToLittleFS();
    remote_SELECT = false;
    currentMode = 1;

  }
  
  // Redraw the editing screen.
  clearBuffer();
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 8; c++) {
      drawCharToBuffer(c * 5, r * 7, page0[r][c]);
    }
  }
  int cellX = cursorCol * 5;
  int cellY = cursorRow * 7;
  drawCharToBuffer(cellX, cellY - 14, wrapChar(currentSelection - 2));
  drawCharToBuffer(cellX, cellY - 7, wrapChar(currentSelection - 1));
  drawCharToBuffer(cellX, cellY, currentSelection);
  drawCharToBuffer(cellX, cellY + 7, wrapChar(currentSelection + 1));
  drawCharToBuffer(cellX, cellY + 14, wrapChar(currentSelection + 2));
  drawDisplay();
}

// ----------------------- WiFi Menu Functions -----------------------

enum WifiMenuState { WIFI_MENU_SCANNING, WIFI_MENU_SELECT, WIFI_MENU_EDIT_PASS, WIFI_MENU_CONNECTING, WIFI_MENU_CONNECTED };
WifiMenuState wifiMenuState = WIFI_MENU_SCANNING;
String scannedSSIDs[20];
int numScanned = 0, selectedNetworkIndex = 0;
String wifiPasswordBuffer = "";
bool wifiPasswordComplete = false;

bool menuButtonPressed(int pin, bool &lastState) {
  bool pressed = !digitalRead(pin);
  if (pressed && !lastState) { lastState = pressed; return true; }
  lastState = pressed;
  return false;
}
bool lastLeftMenu = false, lastRightMenu = false, lastUpMenu = false, lastDownMenu = false;

void initWiFiEdit() {
  wifiPasswordBuffer = "";
}

void updateWiFiEdit() {
  // Example: simply append currentSelection on right press; left deletes last char.
  bool btnRight = !digitalRead(BTN_RIGHT);
  if (btnRight) { wifiPasswordBuffer += currentSelection; }
  if (menuButtonPressed(BTN_LEFT, lastLeftMenu)) {
    if (wifiPasswordBuffer.length() > 0)
      wifiPasswordBuffer.remove(wifiPasswordBuffer.length() - 1);
  }
  bool btnSelect = !digitalRead(BTN_SELECT);
  if (btnSelect) { wifiPasswordComplete = true; }
  clearBuffer();
  drawText(0, 0, "Enter Password:");
  drawText(0, 7, wifiPasswordBuffer);
  drawDisplay();
}

void wifiMenu() {
  switch (wifiMenuState) {
    case WIFI_MENU_SCANNING: {
      int n = WiFi.scanComplete();
      if (n <= 0) {
        WiFi.scanNetworks(true, true);
        clearBuffer();
        drawText(0, 0, "Scanning...");
        drawDisplay();
      } else {
        numScanned = (n < 20) ? n : 20;
        for (int i = 0; i < numScanned; i++)
          scannedSSIDs[i] = WiFi.SSID(i);
        wifiMenuState = WIFI_MENU_SELECT;
      }
      break;
    }
    case WIFI_MENU_SELECT: {
      clearBuffer();
      drawText(0, 0, "L/R:Select");
      drawText(0, 7, scannedSSIDs[selectedNetworkIndex]);
      drawText(0, 14, "Up:OK Down:SCAN");
      drawDisplay();
      if (menuButtonPressed(BTN_LEFT, lastLeftMenu)) {
        selectedNetworkIndex = (selectedNetworkIndex - 1 + numScanned) % numScanned;
        lastButtonPressTime = millis();
      }
      if (menuButtonPressed(BTN_RIGHT, lastRightMenu)) {
        selectedNetworkIndex = (selectedNetworkIndex + 1) % numScanned;
        lastButtonPressTime = millis();
      }
      if (menuButtonPressed(BTN_UP, lastUpMenu)) {
        wifiMenuState = WIFI_MENU_EDIT_PASS;
        initWiFiEdit();
      }
      if (menuButtonPressed(BTN_DOWN, lastDownMenu)) {
        wifiMenuState = WIFI_MENU_SCANNING;
        WiFi.scanDelete();
        WiFi.scanNetworks(true, true);
      }
      break;
    }
    case WIFI_MENU_EDIT_PASS: {
      updateWiFiEdit();
      if (wifiPasswordComplete)
        wifiMenuState = WIFI_MENU_CONNECTING;
      break;
    }
    case WIFI_MENU_CONNECTING: {
      clearBuffer();
      drawText(0, 0, scannedSSIDs[selectedNetworkIndex]);
      drawText(0, 7, "Password:");
      drawText(0, 14, wifiPasswordBuffer);
      drawDisplay();
      pendingSSID = scannedSSIDs[selectedNetworkIndex];
      pendingPass = wifiPasswordBuffer;
      File credFile = LittleFS.open("/credentials.txt", "w");
      if (credFile) {
        credFile.println(pendingSSID);
        credFile.println(pendingPass);
        credFile.close();
      }
      connectStartTime = millis();
      connectInProgress = true;
      WiFi.mode(WIFI_STA);
      WiFi.begin(pendingSSID.c_str(), pendingPass.c_str());
      wifiMenuState = WIFI_MENU_CONNECTED;
      break;
    }
    case WIFI_MENU_CONNECTED: {
      if (WiFi.status() == WL_CONNECTED)
        displayIP();
      else if (millis() - connectStartTime >= 60000) {
        clearBuffer();
        drawText(0, 0, "ConnFail");
        drawDisplay();
        wifiMenuState = WIFI_MENU_SELECT;
      }
      ignoreMenu = false;
      currentMode = 0;
      break;
    }
  }
}

// ----------------------- Display Pages Function -----------------------
void displayPages() {
  unsigned long now = millis();
  if (currentPage < NUM_PAGES) {
    clearBuffer();
    String fname = "/page" + String(currentPage) + ".txt";
    File file = LittleFS.open(fname, "r");
    if (file && file.size() > 6) {
      for (int line = 0; line < 3 && file.available(); line++) {
        String txt = file.readStringUntil('\n');
        for (size_t i = 0; i < txt.length(); i++) {
          drawCharToBuffer(5 * i, line * 7, txt[i]);
        }
      }
      file.close();
      if (static_cast<unsigned long>(now - previousPageTime) >= static_cast<unsigned long>(DELAY_TIME) || changeNow) {
        drawDisplay();
        if (!changeNow)
          currentPage++;
        else
          changeNow = false;
        previousPageTime = now;
      }
    } else {
      currentPage++;
    }
  }
  if (currentPage < 0 || currentPage >= NUM_PAGES)
    currentPage = 0;
}

// ----------------------- Draw Display Routine -----------------------
void drawDisplay() {
  for (int y = 0; y < DISPLAY_HEIGHT; y++) {
    for (int x = 0; x < DISPLAY_WIDTH; x++) {
      uint8_t brightness = frameBuffer[x][y];
      uint8_t matrixIndex = 0;
      int localX = 0, localY = 0;
      if (y < 9) {
        if (x < 16) { matrixIndex = 0; localX = x; localY = y; }
        else if (x < 32) { matrixIndex = 1; localX = x - 16; localY = y; }
        else { matrixIndex = 2; localX = x - 24; localY = y; }
      } else if (y < 18) {
        if (x < 16) { matrixIndex = 3; localX = x; localY = y - 9; }
        else if (x < 32) { matrixIndex = 4; localX = x - 16; localY = y - 9; }
        else { matrixIndex = 2; localX = x - 32; localY = y - 9; }
      } else if (y <= 20) {
        if (x < 16) { matrixIndex = 5; localX = x; }
        else if (x < 32) { matrixIndex = 6; localX = x - 16; }
        else { matrixIndex = 7; localX = x - 32; }
        localY = y - 18;
      }
      if ((matrixIndex >= 5) && localY > 3) continue;
      tcaSelect(matrixIndex / 4);
      matrices[matrixIndex].drawPixel(localX, localY, brightness);
    }
  }
}

// ----------------------- Other Drawing Functions -----------------------
void drawLine(int x0, int y0, int x1, int y1, uint8_t brightness) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, e2;
  while (true) {
    if(x0 >= 0 && y0 >= 0) realPixel(x0, y0, brightness);
    if (x0 == x1 && y0 == y1) break;
    e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void drawCircle(int x0, int y0, int r, uint8_t brightness) {
  int x = r, y = 0, err = 0;
  while (x >= y) {
    realPixel(x0 + x, y0 + y, brightness);
    realPixel(x0 + y, y0 + x, brightness);
    realPixel(x0 - y, y0 + x, brightness);
    realPixel(x0 - x, y0 + y, brightness);
    realPixel(x0 - x, y0 - y, brightness);
    realPixel(x0 - y, y0 - x, brightness);
    realPixel(x0 + y, y0 - x, brightness);
    realPixel(x0 + x, y0 - y, brightness);
    y++;
    if (err <= 0) { err += 2 * y + 1; }
    if (err > 0) { x--; err -= 2 * x + 1; }
  }
}

void realPixel(int x, int y, uint8_t brightness) {
  if (x >= 0 && x < DISPLAY_WIDTH && y >= 0 && y < 27)
    bigBuffer[x][y] = brightness;
}

void drawBigBuffer() {
  for (int i = 0; i < DISPLAY_WIDTH; i++) {
    for (int j = 0; j < 27; j++) {
      if (j < 7)
        frameBuffer[i][j] = bigBuffer[i][j];
      else if (j >= 10 && j < 17)
        frameBuffer[i][j - 3] = bigBuffer[i][j];
      else if (j >= 20)
        frameBuffer[i][j - 6] = bigBuffer[i][j];
    }
  }
}

// ----------------------- Clock & Screensaver -----------------------
void analogClock() {
  clearBuffer();
  timeClient.update();
  int hr = timeClient.getHours(), mn = timeClient.getMinutes(), sc = timeClient.getSeconds();
  float radSc = (sc / 60.0) * 360.0 * DEG_TO_RAD;
  float radMn = ((mn + sc / 60.0) / 60.0) * 360.0 * DEG_TO_RAD;
  float radHr = (((hr % 12) + mn / 60.0) / 12.0) * 360.0 * DEG_TO_RAD;
  int centerX = 20, centerY = 13, radius = 18;
  drawCircle(centerX, centerY, radius, 32);
  for (int i = 0; i < 12; i++) {
    float angle = i * 30.0 * DEG_TO_RAD;
    int xOuter = centerX + round(radius * cos(angle));
    int yOuter = centerY + round(radius * sin(angle));
    int xInner = centerX + round((radius - 5) * cos(angle));
    int yInner = centerY + round((radius - 5) * sin(angle));
    drawLine(xOuter, yOuter, xInner, yInner, 32);
  }
  int xSec = centerX + round(9 * sin(radSc));
  int ySec = centerY - round(9 * cos(radSc));
  drawLine(centerX, centerY, xSec, ySec, 16);
  int xMin = centerX + round(8 * sin(radMn));
  int yMin = centerY - round(8 * cos(radMn));
  drawLine(centerX, centerY, xMin, yMin, 64);
  int xHr = centerX + round(6 * sin(radHr));
  int yHr = centerY - round(6 * cos(radHr));
  drawLine(centerX, centerY, xHr, yHr, 192);
  drawBigBuffer();
  drawDisplay();
}

void DVDScreensaver() {
  const int blockWidth = 15, blockHeight = 7;
  const int maxX = DISPLAY_WIDTH - blockWidth, maxY = 27 - blockHeight;
  const unsigned long updateInterval = 100;
  static bool init = false;
  static int dvdX, dvdY, dx, dy;
  static unsigned long lastUpdate = 0;
  if (!init) {
    dvdX = random(0, maxX + 1);
    dvdY = random(0, maxY + 1);
    dx = (random(0, 2) == 0) ? 1 : -1;
    dy = (random(0, 2) == 0) ? 1 : -1;
    init = true;
  }
  if (millis() - lastUpdate >= updateInterval) {
    lastUpdate = millis();
    if (dvdX + dx < 0 || dvdX + dx > maxX) dx = -dx;
    if (dvdY + dy < 0 || dvdY + dy > maxY) dy = -dy;
    dvdX += dx;
    dvdY += dy;
    // Clear the bigBuffer (instead of frameBuffer)
    memset(bigBuffer, 0, sizeof(bigBuffer));
    // Draw "DVD" into bigBuffer using our new text function
    drawTextBigBuffer(dvdX, dvdY, "DVD");
    // Copy the bigBuffer into the frameBuffer and update display
    drawBigBuffer();
    drawDisplay();
  }
}

// ----------------------- Menu & Button Handling -----------------------
void drawMenu(int selectedIndex) {
  clearBuffer();
  int firstItem = (selectedIndex >= 2) ? selectedIndex - 1 : 0;
  if (firstItem > MENU_ITEMS - 3) firstItem = MENU_ITEMS - 3;
  for (int i = 0; i < 3; i++) {
    int idx = firstItem + i;
    int y = i * 7;
    if (idx == selectedIndex) {
      drawCharToBuffer(0, y, '>');
      drawText(5, y, menuOptions[idx]);
    } else {
      drawText(0, y, menuOptions[idx]);
    }
  }
  drawDisplay();
}


void runMenu() {
  unsigned long now = millis();
  // Only process new input if the debounce period has passed:
  if (now - lastButtonPressTime < debounceDelay)
    return;

  // Reset buttonDown when all buttons are released.
  if (digitalRead(BTN_SELECT) == HIGH && digitalRead(BTN_LEFT) == HIGH &&
      digitalRead(BTN_RIGHT) == HIGH && digitalRead(BTN_UP) == HIGH && digitalRead(BTN_DOWN) == HIGH) {
    buttonDown = false;
  }

  // Process UP button press
  if ((digitalRead(BTN_UP) == LOW || remote_UP) && !buttonDown) {
    buttonDown = true;
    remote_UP = false;
    menuSelected = (menuSelected - 1 + MENU_ITEMS) % MENU_ITEMS;
    lastButtonPressTime = now;
  }
  // Process DOWN button press
  else if ((digitalRead(BTN_DOWN) == LOW || remote_DOWN) && !buttonDown) {
    buttonDown = true;
    remote_DOWN = false;
    menuSelected = (menuSelected + 1) % MENU_ITEMS;
    lastButtonPressTime = now;
  }
  // Process SELECT button press
  else if ((digitalRead(BTN_SELECT) == LOW || remote_SELECT) && !buttonDown) {
    buttonDown = true;
    remote_SELECT = false;
    // Execute the selected menu option:
    switch (menuSelected) {
      case 0: currentMode = 6; break; // EDIT
      case 1: currentMode = 0; break; // DISPLAY
      case 2: currentMode = 2; break; // CLOCK
      case 3: currentMode = 3; forceUpdate = true; break; // WEATHER
      case 4: currentMode = 7; break; // WIFI
      case 5: currentMode = 4; snake = false; break; // GAME
      case 6: currentMode = 5; break; // SCRNSAVR
      case 7: currentMode = 8; break; // STREAM
      case 8: LittleFS.format(); ESP.restart(); break; // RESET
      default: break;
    }
    lastButtonPressTime = now;
    menuOpen = false;
  }

  // Update the menu display at a rate controlled by debounceDelay
  if (now - previousDrawTime > debounceDelay) {
    drawMenu(menuSelected);
    previousDrawTime = now;
  }
}

void buttonListener() {
  unsigned long now = millis();
  // Only process new input if debounceDelay has passed
  if (now - lastButtonPressTime < debounceDelay)
    return;

  if (ignoreMenu) return;
  if (menuOpen) return;

  if (digitalRead(BTN_SELECT) == LOW || remote_SELECT) {
    menuOpen = true;
    currentMode = 1;
    lastButtonPressTime = now;  // update timestamp
    buttonDown = true;
    remote_SELECT = false;
    return;
  }
  if (digitalRead(BTN_LEFT) == LOW || remote_LEFT) {
    currentPage--;
    if (currentPage < 0) currentPage = NUM_PAGES - 1;
    lastButtonPressTime = now;
    buttonDown = true;
    remote_LEFT = false;
    return;
  }
  if (digitalRead(BTN_RIGHT) == LOW || remote_RIGHT) {
    currentPage++;
    if (currentPage >= NUM_PAGES) currentPage = 0;
    lastButtonPressTime = now;
    buttonDown = true;
    remote_RIGHT = false;
    return;
  }
  if (digitalRead(BTN_UP) == LOW || remote_UP) {
    ON_VALUE += 16;
    if (ON_VALUE > 255) ON_VALUE = 255;
    lastButtonPressTime = now;
    buttonDown = true;
    remote_UP = false;
    saveDisplaySettings();
  drawDisplay();
    return;
  }
  if (digitalRead(BTN_DOWN) == LOW || remote_DOWN) {
    ON_VALUE -= 16;
    if (ON_VALUE < 16) ON_VALUE = 16;
    lastButtonPressTime = now;
    buttonDown = true;
    remote_DOWN = false;
    saveDisplaySettings();
  drawDisplay();
    return;
  }
}


// ----------------------- Snake Game Functions -----------------------
void initializeSnake() {
  snakeLength = 5;
  snakeDirection = 1;
  for (int i = 0; i < snakeLength; i++) {
    snakeX[i] = 5 - i;
    snakeY[i] = 10;
  }
  foodEaten = false;
  foodX = random(0, DISPLAY_WIDTH);
  foodY = random(0, DISPLAY_HEIGHT);
}

void moveSnake() {
  for (int i = snakeLength - 1; i > 0; i--) {
    snakeX[i] = snakeX[i - 1];
    snakeY[i] = snakeY[i - 1];
  }
  switch (snakeDirection) {
    case 0: snakeY[0]--; break;
    case 1: snakeX[0]++; break;
    case 2: snakeY[0]++; break;
    case 3: snakeX[0]--; break;
  }
  if (snakeX[0] < 0 || snakeX[0] >= DISPLAY_WIDTH || snakeY[0] < 0 || snakeY[0] >= DISPLAY_HEIGHT)
    gameOver();
  for (int i = 1; i < snakeLength; i++) {
    if (snakeX[0] == snakeX[i] && snakeY[0] == snakeY[i])
      gameOver();
  }
  if (snakeX[0] == foodX && snakeY[0] == foodY) {
    snakeLength++;
    foodX = random(0, DISPLAY_WIDTH);
    foodY = random(0, DISPLAY_HEIGHT);
  }
}

void drawGame() {
  clearBuffer();
  for (int i = 0; i < snakeLength; i++) {
    frameBuffer[snakeX[i]][snakeY[i]] = map(i, 0, snakeLength, 255, 1);
  }
  frameBuffer[foodX][foodY] = 255;
  drawDisplay();
}

void gameOver() {
  clearBuffer();
  drawText(10, 0, "GAME");
  drawText(10, 14, "OVER");
  drawDisplay();
  delay(1500);
  ignoreMenu = false;
  snake = false;
  currentMode = 0;
}

void handleInput() {
  if (((!digitalRead(BTN_UP)) || remote_UP) && snakeDirection != 2) {
    snakeDirection = 0;
    remote_UP = false;
  }
  if (((!digitalRead(BTN_RIGHT)) || remote_RIGHT) && snakeDirection != 3) {
    snakeDirection = 1;
    remote_RIGHT = false;
  }
  if (((!digitalRead(BTN_DOWN)) || remote_DOWN) && snakeDirection != 0) {
    snakeDirection = 2;
    remote_DOWN = false;
  }
  if (((!digitalRead(BTN_LEFT)) || remote_LEFT) && snakeDirection != 1) {
    snakeDirection = 3;
    remote_LEFT = false;
  }
}


// ----------------------- Web Server Handlers -----------------------

void handleUpdateAll(AsyncWebServerRequest *request) {
  // Update page data
  for (int i = 0; i < NUM_PAGES; i++) {
    for (int j = 0; j < 3; j++) {
      String param = "p" + String(i) + "l" + String(j);
      if (request->hasParam(param, true))
        pageData[i][j] = request->getParam(param, true)->value();
    }
  }

  // Update settings
  if (request->hasParam("onValue", true))
    ON_VALUE = request->getParam("onValue", true)->value().toInt();
  if (request->hasParam("delayTime", true))
    DELAY_TIME = request->getParam("delayTime", true)->value().toInt();
  
  // Save changes
  saveToLittleFS();
  saveDisplaySettings();

  // Reset display state
  currentPage = 0;
  previousPageTime = 0;  // Force immediate page update
  changeNow = true;      // Force display refresh
  currentMode = 0;       // Switch to display mode
  
  // Respond to client
  request->redirect("/");
}

void handleClearAll(AsyncWebServerRequest *request) {
  for (int i = 0; i < NUM_PAGES; i++)
    for (int j = 0; j < 3; j++)
      pageData[i][j] = "";
  saveToLittleFS();
  request->redirect("/");
}

void handleSetMode(AsyncWebServerRequest *request) {
  if (request->hasParam("mode", true)) {
    String mode = request->getParam("mode", true)->value();
    if      (mode == "Display")    currentMode = 0;
    else if (mode == "Clock")      currentMode = 2;
    else if (mode == "Weather"){   currentMode = 3;  forceUpdate = true;}
    else if (mode == "Game"){      currentMode = 4;  ignoreMenu = true; snake = false;}
    else if (mode == "Screensaver")currentMode = 5;
    request->send(200, "text/plain", "Mode set to " + mode);
    Serial.println("Mode set to: " + mode);
  } else {
    request->send(400, "text/plain", "No mode parameter provided");
  }
}
String escapeHTML(String input) {
  input.replace("&", "&amp;");
  input.replace("<", "&lt;");
  input.replace(">", "&gt;");
  input.replace("\"", "&quot;");
  input.replace("'", "&#39;");
  input.replace("\\", "&#92;");
  return input;
}

void handleRemoteControl(AsyncWebServerRequest *request) {
  if (!request->hasParam("action", true)) {
    request->send(400, "text/plain", "Missing action parameter");
    return;
  }
  String action = request->getParam("action", true)->value();
  if      (action == "UP")     remote_UP = true;
  else if (action == "DOWN")   remote_DOWN = true;
  else if (action == "LEFT")   remote_LEFT = true;
  else if (action == "RIGHT")  remote_RIGHT = true;
  else if (action == "SELECT") remote_SELECT = true;
  request->send(200, "text/plain", "Remote action '" + action + "' received");
  Serial.println("Remote action received: " + action);
}

void handleSaveConnections(AsyncWebServerRequest *request) {
  String timeServer = "", timeZone = "";
  if (request->hasParam("api_key", true))
    api_key = request->getParam("api_key", true)->value();
  if (request->hasParam("latitude", true))
    latitude = request->getParam("latitude", true)->value();
  if (request->hasParam("longitude", true))
    longitude = request->getParam("longitude", true)->value();
  if (request->hasParam("units", true))
    units = request->getParam("units", true)->value();
  if (request->hasParam("timeServer", true))
    timeServer = request->getParam("timeServer", true)->value();
  if (request->hasParam("timeZone", true))
    timeZone = request->getParam("timeZone", true)->value();
  File file = LittleFS.open("/weather.txt", "w");
  if (file) {
    file.println(api_key);
    file.println(latitude);
    file.println(longitude);
    file.println(units);
    file.close();
  }
  file = LittleFS.open("/time.txt", "w");
  if (file) {
    file.println(timeServer);
    file.println(timeZone);
    file.close();
  }
  request->send(200, "text/plain", "Settings saved successfully!");
  request->redirect("/");
}

void handleScan(AsyncWebServerRequest *request) {
  int n = WiFi.scanComplete();
  if (n <= 0) {
    WiFi.scanNetworks(true, true);
    request->send(200, "application/json", "[]");
    return;
  }
  String json = "[";
  for (int i = 0; i < n; i++) {
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":\"" + String(WiFi.RSSI(i)) + "\"}";
    if (i < n - 1) json += ",";
  }
  json += "]";
  request->send(200, "application/json", json);
  WiFi.scanDelete();
  WiFi.scanNetworks(true, true);
}

void handleConnect(AsyncWebServerRequest *request) {
  if (!request->hasParam("ssid", true) || !request->hasParam("pass", true)) {
    request->send(400, "text/plain", "Missing SSID or password");
    return;
  }
  String ssid = request->getParam("ssid", true)->value();
  String pass = request->getParam("pass", true)->value();
  File file = LittleFS.open("/credentials.txt", "w");
  if (file) {
    file.println(ssid);
    file.println(pass);
    file.close();
  }
  pendingSSID = ssid;
  pendingPass = pass;
  connectStartTime = millis();
  connectInProgress = true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(pendingSSID.c_str(), pendingPass.c_str());
  request->send(200, "text/plain", "Connection attempt started");
}

void connectCredentials() {
  File credFile = LittleFS.open("/credentials.txt", "r");
  if (!credFile) {
    Serial.println("No credentials file found.");
    return;
  }
  if (credFile.size() <= 6) {
    Serial.println("Credentials file is blank.");
    credFile.close();
    return;
  }
  String ssid = credFile.readStringUntil('\n');
  ssid.trim();
  String pass = credFile.readStringUntil('\n');
  pass.trim();
  credFile.close();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long startTime = millis();
  uint8_t i = 0;
  while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < 30000) {
    clearBuffer();
    drawText(0, 0, "CONNECT ");
    drawText(0, 7, loadingBar[i]);
    drawText(5, 7, "    ING");
    drawText(0, 14, "TO WiFi!");
    drawDisplay();
    i = (i + 1 == 4 ? 0 : i + 1);
    delay(100);
  }
  if (WiFi.status() == WL_CONNECTED) {
    displayIP();
    Serial.println();
  } else {
    displayFail();
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, netMsk);
    WiFi.softAP(AP_SSID);
  }
}

void deleteCredentials() {
  LittleFS.remove("/credentials.txt");
  pageData[0][0] = "WiFi";
  pageData[0][1] = "cleared";
  pageData[0][2] = "";
  saveToLittleFS();
  ESP.restart();
}

// ----------------------- Captive Portal -----------------------
boolean captivePortal(AsyncWebServerRequest *request) {
  Serial.println("Captive Portal");
  if (!request->host().equals(WiFi.softAPIP().toString())) {
    request->redirect("http://" + WiFi.softAPIP().toString());
    return true;
  }
  return false;
}



void displayIP() {
  for (int j = 0; j < 3; j++) pageData[0][j] = "";
  clearBuffer();
  drawDisplay();
  IPAddress ip = WiFi.localIP();
  pageData[0][0] = "IP Addr:";
  pageData[0][1] = String(ip[0]) + "." + String(ip[1]);
  pageData[0][2] = "." + String(ip[2]) + "." + String(ip[3]);
  saveToLittleFS();
}

void displayFail() {
  for (int j = 0; j < 3; j++) pageData[0][j] = "";
  clearBuffer();
  drawDisplay();
  pageData[0][0] = "Connect";
  pageData[0][1] = "Failure";
  pageData[0][2] = "AP activ";
  saveToLittleFS();
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    connectInProgress = false;
    IPAddress ip = WiFi.localIP();
    Serial.print("Connected. IP: ");
    Serial.println(ip);
    displayIP();
  } else if (millis() - connectStartTime >= 60000) {
    connectInProgress = false;
    Serial.println("Connection timeout; reverting to AP mode.");
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, netMsk);
    WiFi.softAP(AP_SSID);
  } else {
  unsigned long startTime = millis();
  uint8_t i = 0;
    while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < 30000) {
    clearBuffer();
    drawText(0, 0, "CONNECT ");
    drawText(0, 7, loadingBar[i]);
    drawText(5, 7, "    ING");
    drawText(0, 14, "TO WiFi!");
    drawDisplay();
    i = (i + 1 == 4 ? 0 : i + 1);
    delay(100);
  }
  }
}

// ----------------------- Web Server Handlers -----------------------

void handleRoot(AsyncWebServerRequest *request) {
  String html = F("<!DOCTYPE html><html><head>"
                  "<title>D.O.T. Sign</title>"
                  "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                  "<style>"
                  "body { font-family: Arial, sans-serif; margin:0; padding:0; background:#222; color:#fff; }"
                  ".tab { overflow: hidden; background-color: #333; }"
                  ".tab button { background-color: inherit; border: none; outline: none; cursor: pointer; "
                  "padding: 14px 16px; transition: 0.3s; color: #fff; font-size: 17px; }"
                  ".tab button:hover { background-color: #575757; }"
                  ".tab button.active { background-color: #111; }"
                  ".tabcontent { display: none; padding: 20px; }"
                  "input[type=text], input[type=password], input[type=number], select { "
                  "background: #333; color: #fff; border: 1px solid #666; }"
                  "textarea { background: #333; color: #fff; border: 1px solid #666; }"
                  "input, select, button, textarea { font-size: 1em; margin: 5px 0; padding: 10px; "
                  "width: 90%; max-width: 600px; }"
                  "</style></head><body>"
                  "<div class='container' style='text-align: center;'>");

  // Add tab navigation
  html += F("<div class='tab'><table style='margin: 0 auto;'>"
            "<tr>"
            "<td><button class='tablinks active' onclick='openTab(event,\"Pages\")'>Pages</button></td>"
            "<td><button class='tablinks' onclick='openTab(event,\"WiFi\")'>WiFi Setup</button></td>"
            "</tr><tr>"
            "<td><button class='tablinks' onclick='openTab(event,\"Connections\")'>Connections</button></td>"
            "<td><button class='tablinks' onclick='openTab(event,\"RemoteControl\")'>Remote Control</button></td>"
            "</tr><tr>"
            "<td colspan='2'><button class='tablinks' onclick='openTab(event,\"TextInput\")'>Text Input</button></td>"
            "</tr></table></div>");

  // Pages tab content
  html += F("<div id='Pages' class='tabcontent' style='display:block'>"
            "<h2>Page Editor</h2>"
            "<form action='/updateAll' method='POST'>");
  for (int i = 0; i < 4; i++) {
    html += "<h3>Page " + String(i + 1) + "</h3>";
    for (int j = 0; j < 3; j++) {
      html += "<input type='text' maxlength='8' name='p" + String(i) + "l" + String(j) + "' value='";
      html += escapeHTML(pageData[i][j]);
      html += "' /><br>";
    }
  }
  html += F("<h3>Brightness</h3>"
            "<input type='range' name='onValue' min='16' max='255' value='") + 
            String(ON_VALUE) + F("' /><br>"
            "<h3>Page Display Time</h3>"
            "<input type='range' name='delayTime' min='500' max='5000' step='500' value='") +
            String(DELAY_TIME) + 
            F("' oninput='this.nextElementSibling.value = this.value + \"ms\"' />") +
            F("<output>") + String(DELAY_TIME) + F("ms</output><br>"
            "<button type='submit'>Update All</button></form>"
            "<form action='/clearAll' method='POST'>"
            "<button type='submit'>Clear All</button></form></div>");

  // WiFi Setup tab
  html += F("<div id='WiFi' class='tabcontent'>"
            "<h2>WiFi Setup</h2>"
            "<button onclick='scanNetworks()' style='width:auto;margin:10px'>Scan Networks</button>"
            "<div id='networks' style='margin:10px'>"
            "<select id='networkSelect' style='width:100%;max-width:400px;margin:10px 0'>"
            "<option value=''>Scanning...</option>"
            "</select></div>"
            "<form id='connectForm' action='/connect' method='POST'>"
            "<input type='hidden' name='ssid' id='ssidInput'>"
            "<input type='password' name='pass' placeholder='Password' style='width:100%;max-width:400px'><br>"
            "<button type='submit'>Connect</button></form>"
            "<form action='/deleteCredentials' method='POST'>"
            "<button type='submit'>Delete Saved Credentials</button></form></div>");

  // Connections tab
  html += F("<div id='Connections' class='tabcontent'>"
            "<h2>Weather & Time Settings</h2>"
            "<form action='/saveConnections' method='POST'>"
            "<h3>OpenWeather Settings</h3>"
            "<input type='text' name='api_key' placeholder='API Key' value='") +
            api_key + "'><br>" +
            F("<input type='text' name='latitude' placeholder='Latitude' value='") +
            latitude + "'><br>" +
            F("<input type='text' name='longitude' placeholder='Longitude' value='") +
            longitude + "'><br>" +
            F("<select name='units'>"
            "<option value='imperial'>Imperial (&#176;F)</option>"
            "<option value='metric'>Metric (&#176;C)</option>"
            "</select><br>"
            "<h3>Time Settings</h3>"
            "<input type='text' name='timeServer' placeholder='NTP Server' value='pool.ntp.org'><br>"
            "<select name='timeZone'>") +
            generateTimezoneOptions(timeOffset) +
            F("</select><br>"
            "<button type='submit'>Save Settings</button></form></div>");

  // Remote Control tab content
  html += F("<div id='RemoteControl' class='tabcontent'>"
            "<h2>Remote Control</h2>"
            "<table style='margin:0 auto'><tr>"
            "<td></td><td><button onclick='sendControl(\"UP\")'>&uarr;</button></td><td></td></tr>"
            "<tr><td><button onclick='sendControl(\"LEFT\")'>&larr;</button></td>"
            "<td><button onclick='sendControl(\"SELECT\")'>OK</button></td>"
            "<td><button onclick='sendControl(\"RIGHT\")'>&rarr;</button></td></tr>"
            "<tr><td></td><td><button onclick='sendControl(\"DOWN\")'>&darr;</button></td><td></td></tr></table>"
            "<h3>Display Mode</h3>"
            "<button onclick='setMode(\"Display\")'>Pages</button>"
            "<button onclick='setMode(\"Clock\")'>Clock</button>"
            "<button onclick='setMode(\"Weather\")'>Weather</button>"
            "<button onclick='setMode(\"Game\")'>Snake</button>"
            "<button onclick='setMode(\"Screensaver\")'>DVD</button></div>");

  // Text Input tab
  html += F("<div id='TextInput' class='tabcontent'>"
            "<h2>Text Input</h2>"
            "<p>Enter or paste your text below. It will be automatically formatted into pages.</p>"
            "<form action='/submitText' method='POST'>"
            "<textarea name='text' rows='10' style='width: 90%; max-width: 600px; margin: 10px; "
            "padding: 10px; font-family: monospace;' "
            "placeholder='Type or paste your text here. Text will be automatically formatted into pages "
            "of 3 lines with 8 characters each.'></textarea><br>"
            "<button type='submit'>Process Text</button></form></div>");

  // Add JavaScript
  html += F("<script>"
            "function openTab(evt, tabName) {"
            "  var i, tabcontent, tablinks;"
            "  tabcontent = document.getElementsByClassName('tabcontent');"
            "  for(i = 0; i < tabcontent.length; i++) {"
            "    tabcontent[i].style.display = 'none';"
            "  }"
            "  tablinks = document.getElementsByClassName('tablinks');"
            "  for(i = 0; i < tablinks.length; i++) {"
            "    tablinks[i].className = tablinks[i].className.replace(' active', '');"
            "  }"
            "  document.getElementById(tabName).style.display = 'block';"
            "  evt.currentTarget.className += ' active';"
            "  if(tabName === 'WiFi') scanNetworks();"
            "}"
            "function scanNetworks() {"
            "  fetch('/scan').then(r=>r.json()).then(networks=>{"
            "    let select = document.getElementById('networkSelect');"
            "    select.innerHTML = '';" // Clear existing options
            "    networks.forEach(n => {"
            "      let opt = document.createElement('option');"
            "      opt.value = n.ssid;"
            "      opt.innerHTML = n.ssid + ' (' + n.rssi + ' dBm)';"
            "      select.appendChild(opt);"
            "    });"
            "    select.onchange = function() {"
            "      document.getElementById('ssidInput').value = this.value;"
            "    };"
            "  });"
            "}"
            "function sendControl(action) {"
            "  fetch('/remoteControl',{method:'POST',body:'action='+action,"
            "  headers:{'Content-Type':'application/x-www-form-urlencoded'}});"
            "}"
            "function setMode(mode) {"
            "  fetch('/setMode',{method:'POST',body:'mode='+mode,"
            "  headers:{'Content-Type':'application/x-www-form-urlencoded'}});"
            "}"
            "</script></body></html>");

  request->send(200, "text/html", html);
}

void udpFrameReceiver() {
  clearBuffer();
 
  int packetSize = udpReceiver.parsePacket();
  if (packetSize >= (DISPLAY_WIDTH * 27)) { 
    byte buffer[DISPLAY_WIDTH * 27];
    int len = udpReceiver.read(buffer, sizeof(buffer));
    if (len >= (DISPLAY_WIDTH * 27)) {
      int index = 0;
     
      for (int y = 0; y < 27; y++) {
        for (int x = 0; x < DISPLAY_WIDTH; x++) {
          bigBuffer[x][y] = buffer[index++];
        }
      }
     
      drawBigBuffer();
      drawDisplay();
    }
  }
}

void handleSubmitText(AsyncWebServerRequest *request) {
  if (!request->hasParam("text", true)) {
    request->send(400, "text/plain", "No text provided");
    return;
  }

 
  currentPage = 0;
  NUM_PAGES = 4;
  for (int i = 0; i < MAX_PAGES; i++) {
    for (int j = 0; j < 3; j++) {
      pageData[i][j] = "";
    }
  }

  String text = request->getParam("text", true)->value();
  int currentPage = 0;
  int currentLine = 0;
  String currentWord = "";
  String buffer = "";
  bool lineStart = true;

 
  for (size_t i = 0; i < text.length() && currentPage < MAX_PAGES; i++) {
    char c = text[i];
    
   
    if (c == '\n') {
     
      if (currentWord.length() > 0) {
        if (buffer.length() + currentWord.length() > 8) {
         
          if (buffer.length() > 0) {
            pageData[currentPage][currentLine] = buffer;
            buffer = "";
            currentLine++;
            if (currentLine >= 3) {
              currentLine = 0;
              currentPage++;
              NUM_PAGES = max(NUM_PAGES, currentPage + 1);
            }
          }
          buffer = currentWord;
        } else {
          buffer += currentWord;
        }
        currentWord = "";
      }
      
     
      if (buffer.length() > 0) {
        pageData[currentPage][currentLine] = buffer;
        buffer = "";
        currentLine++;
        if (currentLine >= 3) {
          currentLine = 0;
          currentPage++;
          NUM_PAGES = max(NUM_PAGES, currentPage + 1);
        }
      }
      lineStart = true;
      continue;
    }

   
    if (c == ' ') {
     
      if (lineStart && currentWord.length() == 0) {
        continue;
      }
      
     
      if (currentWord.length() > 0) {
        if (buffer.length() + currentWord.length() > 8) {
         
          if (buffer.length() > 0) {
            pageData[currentPage][currentLine] = buffer;
            buffer = "";
            currentLine++;
            if (currentLine >= 3) {
              currentLine = 0;
              currentPage++;
              NUM_PAGES = max(NUM_PAGES, currentPage + 1);
            }
          }
          buffer = currentWord;
        } else {
          buffer += currentWord;
        }
        currentWord = "";
        lineStart = false;
      }
      
     
      if (buffer.length() < 8) {
        buffer += " ";
      }
      continue;
    }

   
    currentWord += c;
    
   
    if (currentWord.length() >= 8) {
      if (buffer.length() > 0) {
        pageData[currentPage][currentLine] = buffer;
        buffer = "";
        currentLine++;
        if (currentLine >= 3) {
          currentLine = 0;
          currentPage++;
          NUM_PAGES = max(NUM_PAGES, currentPage + 1);
        }
      }
      pageData[currentPage][currentLine] = currentWord;
      currentWord = "";
      currentLine++;
      if (currentLine >= 3) {
        currentLine = 0;
        currentPage++;
        NUM_PAGES = max(NUM_PAGES, currentPage + 1);
      }
      lineStart = true;
    }
  }

 
  if (currentWord.length() > 0) {
    if (buffer.length() + currentWord.length() > 8) {
      if (buffer.length() > 0) {
        pageData[currentPage][currentLine] = buffer;
        currentLine++;
        if (currentLine >= 3) {
          currentLine = 0;
          currentPage++;
          NUM_PAGES = max(NUM_PAGES, currentPage + 1);
        }
      }
      pageData[currentPage][currentLine] = currentWord;
    } else {
      buffer += currentWord;
      pageData[currentPage][currentLine] = buffer;
    }
  } else if (buffer.length() > 0) {
    pageData[currentPage][currentLine] = buffer;
  }

 
  saveToLittleFS();

  request->send(200, "text/plain", 
    String("Text processed into ") + String(NUM_PAGES) + 
    String(" pages"));
}

String generateTimezoneOptions(int currentOffset) {
  String options;
  for (int i = -12; i <= 14; i++) {
    options += "<option value='" + String(i) + "'";
    if (i == currentOffset) {
      options += " selected";
    }
    options += ">UTC";
    options += (i >= 0 ? "+" : "");
    options += String(i);
    options += "</option>";
  }
  return options;
}

// ----------------------- Setup and Loop -----------------------
void setup() {
  Serial.begin(115200);
  Wire.begin();
  if (!LittleFS.begin()) { Serial.println("LittleFS init failed"); return; }
  Serial.println("LittleFS init");
    NUM_PAGES = 4; 
  for (int i = 4; i < MAX_PAGES; i++) {
    String fname = "/page" + String(i) + ".txt";
    if (LittleFS.exists(fname)) {
      LittleFS.remove(fname); 
    }
   
    for (int j = 0; j < 3; j++) {
      pageData[i][j] = "";
    }
  }
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(AP_SSID);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
  pinMode(BTN_LEFT, INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  setupMatrices();
  dnsServer.start(DNS_PORT, "*", apIP);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/updateAll", HTTP_POST, handleUpdateAll);
  server.on("/clearAll", HTTP_POST, handleClearAll);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/connect", HTTP_POST, handleConnect);
  server.on("/saveConnections", HTTP_POST, handleSaveConnections);
  server.on("/deleteCredentials", HTTP_POST, [](AsyncWebServerRequest *request) { deleteCredentials(); });
  server.on("/setMode", HTTP_POST, handleSetMode);
  server.on("/remoteControl", HTTP_POST, handleRemoteControl);
  server.on("/submitText", HTTP_POST, handleSubmitText);
  server.onNotFound([](AsyncWebServerRequest *request) {
    if (!captivePortal(request))
      request->send(404, "text/plain", "404: Not Found");
  });
  server.begin();
  udpReceiver.begin(UDP_FRAME_PORT);

  Serial.println("Captive Portal running");
  loadFromLittleFS();
  loadDisplaySettings();
  connectCredentials();
  readWeather();
  timeClient.begin();
  loadTimeSettings();
  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() { Serial.println("\nEnd"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if      (error == OTA_AUTH_ERROR)    Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR)   Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR)     Serial.println("End Failed");
  });

  ArduinoOTA.begin();
}

void loop() {
  dnsServer.processNextRequest();
  switch (currentMode) {
    case 0: displayPages(); break;
    case 1: runMenu(); break;
    case 2: analogClock(); break;
    case 3: checkWeatherUpdate(); break;
    case 4:
      if (!snake) { snake = true; initializeSnake(); }
      else { handleInput(); moveSnake(); drawGame(); }
      break;
    case 5: DVDScreensaver(); break;
    case 6: editPageData(); break;
    case 7: wifiMenu(); break;
    case 8: udpFrameReceiver(); break;
    default: break;
  }
  if (connectInProgress) connectWifi();
  if (WiFi.status() == WL_CONNECTED) timeClient.update();
  if (!ignoreMenu && currentMode != 1 && currentMode != 4 && currentMode != 6 && currentMode != 7) buttonListener();
  ArduinoOTA.handle();
}
