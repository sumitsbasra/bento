// ESP32 Desk Assistant
// A desk-top voice assistant on a 466x466 touchscreen ESP32 that connects to OpenClaw
// Swipe between 5 screens: Assistant (center), Calendar (left), Clock (right), YouTube (top), Weather (bottom)
// Tap the assistant screen to talk to it

#include <Waveshare_AMOLED.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <TouchLib.h>
#include <time.h>

// === DISPLAY & TOUCH ===
Waveshare_AMOLED amoled;
#define tft amoled  // Alias so the rest of the code works
TouchLib touch; // Adjust based on your touchscreen controller (e.g., CST816S, FT6236)

// === WIFI CONFIG ===
const char* WIFI_SSID = "ggggg uwu";
const char* WIFI_PASSWORD = "tE9Denbb-iitbiuts";

// === API KEYS ===
const char* YOUTUBE_API_KEY = "your-youtube-api-key";
const char* YOUTUBE_CHANNEL_ID = "your-channel-id";
const char* OPENWEATHER_API_KEY = "your-openweather-api-key";
const char* WEATHER_LOCATION = "San Francisco,US"; // City,Country code

// === OPENCLAW-VOICE WEBSOCKET (for assistant only) ===
const char* WS_HOST = "192.168.1.100"; // Your openclaw-voice server IP
const int WS_PORT = 8765;
WebSocketsClient webSocket;

// === HTTP CLIENT ===
WiFiClientSecure httpsClient;

// === SCREEN STATES ===
enum Screen {
  SCREEN_YOUTUBE,   // Top (swipe down from assistant)
  SCREEN_CALENDAR,  // Left
  SCREEN_ASSISTANT, // Center (default)
  SCREEN_CLOCK,     // Right
  SCREEN_WEATHER    // Bottom (swipe up from assistant)
};

Screen currentScreen = SCREEN_ASSISTANT;

// === ASSISTANT STATES ===
enum AssistantState {
  IDLE,
  LISTENING,
  THINKING,
  SPEAKING
};

AssistantState assistantState = IDLE;

// === TOUCH TRACKING ===
struct TouchPoint {
  int16_t x;
  int16_t y;
  unsigned long timestamp;
};

TouchPoint touchStart = {0, 0, 0};
TouchPoint touchEnd = {0, 0, 0};
const int SWIPE_THRESHOLD = 100; // pixels

// === DATA ===
String currentTranscription = "";
String currentResponse = "";
int subscriberCount = 0;
int subscriberGainToday = 0;

struct CalendarEvent {
  String title;
  String time;
  int minutesUntil;
};
std::vector<CalendarEvent> todaysEvents;

struct WeatherData {
  int temperature;
  String condition;
  String location;
};
WeatherData currentWeather = {72, "Sunny", "San Francisco"};

// === SETUP ===
void setup() {
  Serial.begin(115200);
  
  // Initialize display
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  
  // Initialize touch
  touch.begin();
  
  // Connect to WiFi
  connectWiFi();
  
  // Configure time (NTP)
  configTime(-8 * 3600, 0, "pool.ntp.org"); // PST, adjust for your timezone
  
  // Connect to WebSocket
  webSocket.begin(WS_HOST, WS_PORT, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  
  // Request initial data
  requestYouTubeStats();
  requestWeatherData();
  requestCalendarEvents();
  
  // Initial screen
  drawCurrentScreen();
}

// === MAIN LOOP ===
void loop() {
  webSocket.loop();
  handleTouch();
  updateCurrentScreen();
  delay(10);
}

// === WIFI CONNECTION ===
void connectWiFi() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(100, 200);
  tft.print("Connecting to WiFi...");
  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWiFi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(100, 200);
  tft.print("Connected!");
  delay(1000);
}

// === TOUCH HANDLING ===
void handleTouch() {
  if (touch.read()) {
    TouchPoint tp;
    touch.getPoint(tp.x, tp.y);
    tp.timestamp = millis();
    
    static bool touchActive = false;
    
    if (!touchActive) {
      // Touch started
      touchStart = tp;
      touchActive = true;
    } else {
      touchEnd = tp;
      
      // Check if touch released
      if (!touch.read()) {
        touchActive = false;
        handleGesture();
      }
    }
  }
}

void handleGesture() {
  int deltaX = touchEnd.x - touchStart.x;
  int deltaY = touchEnd.y - touchStart.y;
  unsigned long duration = touchEnd.timestamp - touchStart.timestamp;
  
  // Determine if it's a swipe or tap
  if (duration > 500) return; // Too slow, ignore
  
  if (abs(deltaX) > abs(deltaY) && abs(deltaX) > SWIPE_THRESHOLD) {
    // Horizontal swipe
    if (deltaX > 0) {
      // Swipe right
      swipeHorizontal(-1);
    } else {
      // Swipe left
      swipeHorizontal(1);
    }
  } else if (abs(deltaY) > abs(deltaX) && abs(deltaY) > SWIPE_THRESHOLD) {
    // Vertical swipe
    if (deltaY > 0) {
      // Swipe down
      swipeVertical(-1);
    } else {
      // Swipe up
      swipeVertical(1);
    }
  } else if (abs(deltaX) < 50 && abs(deltaY) < 50 && duration < 300) {
    // Tap
    handleTap(touchStart.x, touchStart.y);
  }
}

void swipeHorizontal(int direction) {
  // Only works on horizontal screens (Calendar, Assistant, Clock)
  if (currentScreen == SCREEN_YOUTUBE || currentScreen == SCREEN_WEATHER) {
    return; // Can't swipe horizontally from vertical screens
  }
  
  if (direction > 0) {
    // Swipe left = go right
    if (currentScreen == SCREEN_CALENDAR) {
      currentScreen = SCREEN_ASSISTANT;
    } else if (currentScreen == SCREEN_ASSISTANT) {
      currentScreen = SCREEN_CLOCK;
    }
  } else {
    // Swipe right = go left
    if (currentScreen == SCREEN_CLOCK) {
      currentScreen = SCREEN_ASSISTANT;
    } else if (currentScreen == SCREEN_ASSISTANT) {
      currentScreen = SCREEN_CALENDAR;
    }
  }
  
  drawCurrentScreen();
}

void swipeVertical(int direction) {
  // Only works on vertical screens (YouTube, Assistant, Weather)
  if (currentScreen == SCREEN_CALENDAR || currentScreen == SCREEN_CLOCK) {
    return; // Can't swipe vertically from horizontal screens
  }
  
  if (direction > 0) {
    // Swipe up = go down
    if (currentScreen == SCREEN_YOUTUBE) {
      currentScreen = SCREEN_ASSISTANT;
    } else if (currentScreen == SCREEN_ASSISTANT) {
      currentScreen = SCREEN_WEATHER;
    }
  } else {
    // Swipe down = go up
    if (currentScreen == SCREEN_WEATHER) {
      currentScreen = SCREEN_ASSISTANT;
    } else if (currentScreen == SCREEN_ASSISTANT) {
      currentScreen = SCREEN_YOUTUBE;
    }
  }
  
  drawCurrentScreen();
}

void handleTap(int x, int y) {
  if (currentScreen == SCREEN_ASSISTANT) {
    if (assistantState == IDLE) {
      // Start listening
      startListening();
    } else if (assistantState == LISTENING) {
      // Cancel listening
      stopListening();
    }
  }
  // Other screens don't respond to tap (only swipe)
}

// === SCREEN DRAWING ===
void drawCurrentScreen() {
  switch (currentScreen) {
    case SCREEN_YOUTUBE:
      drawYouTubeScreen();
      break;
    case SCREEN_CALENDAR:
      drawCalendarScreen();
      break;
    case SCREEN_ASSISTANT:
      drawAssistantScreen();
      break;
    case SCREEN_CLOCK:
      drawClockScreen();
      break;
    case SCREEN_WEATHER:
      drawWeatherScreen();
      break;
  }
}

void updateCurrentScreen() {
  static unsigned long lastUpdate = 0;
  static unsigned long lastDataRefresh = 0;
  unsigned long now = millis();
  
  // Update screen every second for clock
  if (now - lastUpdate > 1000) {
    if (currentScreen == SCREEN_CLOCK) {
      drawClockScreen();
    }
    lastUpdate = now;
  }
  
  // Refresh data every 5 minutes
  if (now - lastDataRefresh > 300000) {
    requestYouTubeStats();
    requestWeatherData();
    requestCalendarEvents();
    lastDataRefresh = now;
  }
  
  // Update assistant screen if state changed
  static AssistantState lastState = IDLE;
  if (currentScreen == SCREEN_ASSISTANT && assistantState != lastState) {
    drawAssistantScreen();
    lastState = assistantState;
  }
}

// === YOUTUBE SCREEN ===
void drawYouTubeScreen() {
  tft.fillScreen(TFT_BLACK);
  
  // Screen indicator
  drawScreenIndicator();
  
  // YouTube logo colors
  tft.setTextColor(TFT_RED);
  tft.setTextSize(3);
  tft.setCursor(150, 70);
  tft.print("YouTube");
  
  // Subscriber count - HUGE and centered
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(10);
  
  String subCount = formatNumber(subscriberCount);
  int textWidth = subCount.length() * 60; // Approximate width for size 10
  tft.setCursor((466 - textWidth) / 2, 160);
  tft.print(subCount);
  
  // "Subscribers" label
  tft.setTextSize(3);
  tft.setTextColor(TFT_LIGHTGREY);
  int labelWidth = 11 * 18; // "Subscribers" is 11 chars
  tft.setCursor((466 - labelWidth) / 2, 280);
  tft.print("Subscribers");
  
  // Subscriber gain today - small and faint grey at bottom
  if (subscriberGainToday != 0) {
    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY);
    
    String gain;
    if (subscriberGainToday > 0) {
      gain = "+" + String(subscriberGainToday) + " today";
    } else {
      gain = String(subscriberGainToday) + " today";
    }
    
    int gainWidth = gain.length() * 6; // Size 1 chars are ~6px wide
    tft.setCursor((466 - gainWidth) / 2, 440);
    tft.print(gain);
  }
}

String formatNumber(int num) {
  if (num >= 1000000) {
    return String(num / 1000000) + "." + String((num % 1000000) / 100000) + "M";
  } else if (num >= 1000) {
    return String(num / 1000) + "." + String((num % 1000) / 100) + "K";
  }
  return String(num);
}

// === ASSISTANT SCREEN ===
void drawAssistantScreen() {
  tft.fillScreen(TFT_BLACK);
  
  // Screen indicator
  drawScreenIndicator();
  
  int centerX = 233;
  int centerY = 233;
  
  switch (assistantState) {
    case IDLE:
      drawIdleState(centerX, centerY);
      break;
    case LISTENING:
      drawListeningState(centerX, centerY);
      break;
    case THINKING:
      drawThinkingState(centerX, centerY);
      break;
    case SPEAKING:
      drawSpeakingState(centerX, centerY);
      break;
  }
  
  // Show instruction at bottom
  if (assistantState == IDLE) {
    tft.setTextColor(TFT_DARKGREY);
    tft.setTextSize(2);
    tft.setCursor(120, 420);
    tft.print("Tap to speak");
  } else if (assistantState == LISTENING) {
    tft.setTextColor(TFT_DARKGREY);
    tft.setTextSize(2);
    tft.setCursor(120, 420);
    tft.print("Tap to cancel");
  }
}

void drawIdleState(int cx, int cy) {
  // Draw large circle icon
  tft.drawCircle(cx, cy - 50, 80, TFT_BLUE);
  tft.drawCircle(cx, cy - 50, 81, TFT_BLUE);
  tft.drawCircle(cx, cy - 50, 82, TFT_BLUE);
  
  // Draw microphone icon inside
  drawMicrophoneIcon(cx, cy - 50, TFT_BLUE);
  
  // Status text
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(3);
  tft.setCursor(180, cy + 70);
  tft.print("Ready");
}

void drawListeningState(int cx, int cy) {
  // Animated waveform circle
  static int waveOffset = 0;
  waveOffset = (waveOffset + 5) % 360;
  
  for (int i = 0; i < 360; i += 10) {
    int radius = 80 + sin((i + waveOffset) * PI / 180.0) * 15;
    float angle = i * PI / 180.0;
    int x1 = cx + cos(angle) * radius;
    int y1 = (cy - 50) + sin(angle) * radius;
    tft.drawPixel(x1, y1, TFT_GREEN);
  }
  
  // Microphone icon
  drawMicrophoneIcon(cx, cy - 50, TFT_GREEN);
  
  // Status text
  tft.setTextColor(TFT_GREEN);
  tft.setTextSize(3);
  tft.setCursor(150, cy + 70);
  tft.print("Listening...");
  
  // Show transcription if available
  if (currentTranscription.length() > 0) {
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(2);
    drawWrappedText(currentTranscription, 40, cy + 120, 386);
  }
}

void drawThinkingState(int cx, int cy) {
  // Spinning dots animation
  static int spinAngle = 0;
  spinAngle = (spinAngle + 10) % 360;
  
  for (int i = 0; i < 3; i++) {
    float angle = (spinAngle + i * 120) * PI / 180.0;
    int x = cx + cos(angle) * 60;
    int y = (cy - 50) + sin(angle) * 60;
    tft.fillCircle(x, y, 8, TFT_YELLOW);
  }
  
  // Status text
  tft.setTextColor(TFT_YELLOW);
  tft.setTextSize(3);
  tft.setCursor(150, cy + 70);
  tft.print("Thinking...");
}

void drawSpeakingState(int cx, int cy) {
  // Pulsing circle
  static int pulseRadius = 70;
  static int pulseDirection = 1;
  pulseRadius += pulseDirection * 3;
  if (pulseRadius > 90 || pulseRadius < 70) {
    pulseDirection *= -1;
  }
  
  tft.drawCircle(cx, cy - 50, pulseRadius, TFT_MAGENTA);
  tft.drawCircle(cx, cy - 50, pulseRadius + 1, TFT_MAGENTA);
  
  // Speaker icon
  drawSpeakerIcon(cx, cy - 50, TFT_MAGENTA);
  
  // Status text
  tft.setTextColor(TFT_MAGENTA);
  tft.setTextSize(3);
  tft.setCursor(150, cy + 70);
  tft.print("Speaking...");
  
  // Show response text
  if (currentResponse.length() > 0) {
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(2);
    drawWrappedText(currentResponse, 40, cy + 120, 386);
  }
}

// === CLOCK SCREEN ===
void drawClockScreen() {
  tft.fillScreen(TFT_BLACK);
  drawScreenIndicator();
  
  // Get current time
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    tft.setTextColor(TFT_RED);
    tft.setTextSize(2);
    tft.setCursor(100, 200);
    tft.print("Time sync failed");
    return;
  }
  
  // Draw time - large
  char timeStr[10];
  strftime(timeStr, sizeof(timeStr), "%I:%M", &timeinfo);
  
  // Remove leading zero from hour
  String timeString = String(timeStr);
  if (timeString.startsWith("0")) {
    timeString = timeString.substring(1);
  }
  
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(8);
  int textWidth = timeString.length() * 48;
  tft.setCursor((466 - textWidth) / 2, 160);
  tft.print(timeString);
  
  // AM/PM
  char ampm[3];
  strftime(ampm, sizeof(ampm), "%p", &timeinfo);
  tft.setTextSize(3);
  tft.setCursor(200, 250);
  tft.print(ampm);
  
  // Draw date
  char dateStr[30];
  strftime(dateStr, sizeof(dateStr), "%A, %B %d", &timeinfo);
  
  tft.setTextSize(2);
  textWidth = strlen(dateStr) * 12;
  tft.setCursor((466 - textWidth) / 2, 300);
  tft.print(dateStr);
}

// === CALENDAR SCREEN ===
void drawCalendarScreen() {
  tft.fillScreen(TFT_BLACK);
  drawScreenIndicator();
  
  // Title
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(3);
  tft.setCursor(150, 50);
  tft.print("Today");
  
  // Draw events
  int y = 120;
  for (const auto& event : todaysEvents) {
    drawEventCard(event, 40, y);
    y += 80;
    
    if (y > 380) break; // Don't overflow screen
  }
  
  // If no events
  if (todaysEvents.empty()) {
    tft.setTextColor(TFT_DARKGREY);
    tft.setTextSize(2);
    tft.setCursor(120, 200);
    tft.print("No events today");
  }
}

void drawEventCard(const CalendarEvent& event, int x, int y) {
  // Card background
  tft.fillRoundRect(x, y, 386, 70, 10, TFT_DARKGREY);
  
  // Event title
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(x + 15, y + 15);
  tft.print(event.title.substring(0, 20)); // Truncate if too long
  
  // Event time
  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(2);
  tft.setCursor(x + 15, y + 40);
  tft.print(event.time);
  
  // Time until
  if (event.minutesUntil > 0) {
    tft.setTextColor(TFT_YELLOW);
    tft.setCursor(x + 250, y + 40);
    if (event.minutesUntil < 60) {
      tft.printf("in %d min", event.minutesUntil);
    } else {
      tft.printf("in %dh %dm", event.minutesUntil / 60, event.minutesUntil % 60);
    }
  }
}

// === WEATHER SCREEN ===
void drawWeatherScreen() {
  tft.fillScreen(TFT_BLACK);
  drawScreenIndicator();
  
  // Location
  tft.setTextColor(TFT_LIGHTGREY);
  tft.setTextSize(2);
  int locWidth = currentWeather.location.length() * 12;
  tft.setCursor((466 - locWidth) / 2, 80);
  tft.print(currentWeather.location);
  
  // Temperature - BIG
  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(10);
  String temp = String(currentWeather.temperature) + "°";
  int tempWidth = temp.length() * 60;
  tft.setCursor((466 - tempWidth) / 2, 160);
  tft.print(temp);
  
  // Condition
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(3);
  int condWidth = currentWeather.condition.length() * 18;
  tft.setCursor((466 - condWidth) / 2, 280);
  tft.print(currentWeather.condition);
  
  // Weather icon based on condition
  drawWeatherIcon(233, 370, currentWeather.condition);
}

void drawWeatherIcon(int cx, int cy, String condition) {
  // Simple weather icons
  if (condition == "Sunny" || condition == "Clear") {
    // Sun
    tft.fillCircle(cx, cy, 30, TFT_YELLOW);
    for (int i = 0; i < 8; i++) {
      float angle = i * 45 * PI / 180.0;
      int x1 = cx + cos(angle) * 40;
      int y1 = cy + sin(angle) * 40;
      int x2 = cx + cos(angle) * 55;
      int y2 = cy + sin(angle) * 55;
      tft.drawLine(x1, y1, x2, y2, TFT_YELLOW);
    }
  } else if (condition == "Cloudy" || condition == "Partly Cloudy") {
    // Cloud
    tft.fillCircle(cx - 20, cy, 20, TFT_LIGHTGREY);
    tft.fillCircle(cx, cy - 10, 25, TFT_LIGHTGREY);
    tft.fillCircle(cx + 20, cy, 20, TFT_LIGHTGREY);
    tft.fillRect(cx - 40, cy, 60, 20, TFT_LIGHTGREY);
  } else if (condition == "Rainy" || condition == "Rain") {
    // Cloud with rain
    tft.fillCircle(cx - 20, cy - 20, 20, TFT_LIGHTGREY);
    tft.fillCircle(cx, cy - 30, 25, TFT_LIGHTGREY);
    tft.fillCircle(cx + 20, cy - 20, 20, TFT_LIGHTGREY);
    tft.fillRect(cx - 40, cy - 20, 60, 20, TFT_LIGHTGREY);
    // Rain drops
    for (int i = 0; i < 5; i++) {
      int x = cx - 30 + i * 15;
      tft.drawLine(x, cy + 5, x, cy + 20, TFT_BLUE);
    }
  }
}

// === SCREEN INDICATOR ===
void drawScreenIndicator() {
  // Draw a cross pattern showing position
  int cx = 233;
  int cy = 20;
  int spacing = 25;
  
  // Horizontal dots (Calendar - Assistant - Clock)
  for (int i = 0; i < 3; i++) {
    int x = cx - spacing + i * spacing;
    bool active = false;
    
    if (i == 0 && currentScreen == SCREEN_CALENDAR) active = true;
    if (i == 1 && currentScreen == SCREEN_ASSISTANT) active = true;
    if (i == 2 && currentScreen == SCREEN_CLOCK) active = true;
    
    if (active) {
      tft.fillCircle(x, cy, 5, TFT_WHITE);
    } else {
      tft.drawCircle(x, cy, 5, TFT_DARKGREY);
    }
  }
  
  // Vertical dots (YouTube - Assistant - Weather)
  for (int i = 0; i < 3; i++) {
    int y = cy - spacing + i * spacing;
    bool active = false;
    
    if (i == 0 && currentScreen == SCREEN_YOUTUBE) active = true;
    if (i == 1 && currentScreen == SCREEN_ASSISTANT) active = true;
    if (i == 2 && currentScreen == SCREEN_WEATHER) active = true;
    
    // Skip the center dot (already drawn in horizontal)
    if (i == 1) continue;
    
    if (active) {
      tft.fillCircle(cx, y, 5, TFT_WHITE);
    } else {
      tft.drawCircle(cx, y, 5, TFT_DARKGREY);
    }
  }
}

// === ICON DRAWING ===
void drawMicrophoneIcon(int cx, int cy, uint16_t color) {
  // Simplified microphone icon
  tft.fillRoundRect(cx - 15, cy - 25, 30, 35, 8, color);
  tft.fillRect(cx - 2, cy + 12, 4, 15, color);
  tft.drawLine(cx - 15, cy + 27, cx + 15, cy + 27, color);
}

void drawSpeakerIcon(int cx, int cy, uint16_t color) {
  // Simplified speaker icon
  tft.fillTriangle(cx - 10, cy - 15, cx - 10, cy + 15, cx + 5, cy + 10, color);
  tft.fillTriangle(cx - 10, cy - 15, cx + 5, cy - 10, cx + 5, cy + 10, color);
  
  // Sound waves
  for (int i = 1; i <= 3; i++) {
    int offset = i * 8;
    tft.drawArc(cx + 5, cy, 15 + offset, 10 + offset, 300, 60, color, TFT_BLACK);
  }
}

// === TEXT UTILITIES ===
void drawWrappedText(String text, int x, int y, int maxWidth) {
  int cursorX = x;
  int cursorY = y;
  int charWidth = 12; // For size 2
  int lineHeight = 20;
  
  for (int i = 0; i < text.length(); i++) {
    if (cursorX + charWidth > x + maxWidth || text[i] == '\n') {
      cursorX = x;
      cursorY += lineHeight;
    }
    
    if (text[i] != '\n') {
      tft.setCursor(cursorX, cursorY);
      tft.print(text[i]);
      cursorX += charWidth;
    }
  }
}

// === VOICE INTERACTION ===
void startListening() {
  assistantState = LISTENING;
  currentTranscription = "";
  currentResponse = "";
  
  // Send WebSocket message to start listening
  StaticJsonDocument<200> doc;
  doc["type"] = "start_listening";
  String json;
  serializeJson(doc, json);
  webSocket.sendTXT(json);
  
  drawAssistantScreen();
}

void stopListening() {
  assistantState = IDLE;
  
  // Send WebSocket message to stop
  StaticJsonDocument<200> doc;
  doc["type"] = "stop_listening";
  String json;
  serializeJson(doc, json);
  webSocket.sendTXT(json);
  
  drawAssistantScreen();
}

// === DATA REQUESTS ===
void requestYouTubeStats() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  HTTPClient http;
  String url = "https://www.googleapis.com/youtube/v3/channels?part=statistics&id=" + 
               String(YOUTUBE_CHANNEL_ID) + "&key=" + String(YOUTUBE_API_KEY);
  
  http.begin(url);
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error) {
      int newSubCount = doc["items"][0]["statistics"]["subscriberCount"].as<int>();
      
      // Calculate gain (simple - just difference from last check)
      if (subscriberCount > 0) {
        subscriberGainToday = newSubCount - subscriberCount;
      }
      
      subscriberCount = newSubCount;
      
      if (currentScreen == SCREEN_YOUTUBE) {
        drawYouTubeScreen();
      }
    }
  }
  
  http.end();
}

void requestWeatherData() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  HTTPClient http;
  String url = "http://api.openweathermap.org/data/2.5/weather?q=" + 
               String(WEATHER_LOCATION) + "&appid=" + String(OPENWEATHER_API_KEY) + 
               "&units=imperial";
  
  http.begin(url);
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error) {
      currentWeather.temperature = doc["main"]["temp"].as<int>();
      currentWeather.location = doc["name"].as<String>();
      
      // Map weather condition
      String weatherMain = doc["weather"][0]["main"].as<String>();
      if (weatherMain == "Clear") {
        currentWeather.condition = "Sunny";
      } else if (weatherMain == "Clouds") {
        currentWeather.condition = "Cloudy";
      } else if (weatherMain == "Rain" || weatherMain == "Drizzle") {
        currentWeather.condition = "Rainy";
      } else {
        currentWeather.condition = weatherMain;
      }
      
      if (currentScreen == SCREEN_WEATHER) {
        drawWeatherScreen();
      }
    }
  }
  
  http.end();
}

void requestCalendarEvents() {
  // This still goes through OpenClaw since calendar requires OAuth
  StaticJsonDocument<200> doc;
  doc["type"] = "get_calendar";
  String json;
  serializeJson(doc, json);
  webSocket.sendTXT(json);
}

// === WEBSOCKET HANDLING ===
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.println("WebSocket Disconnected");
      break;
      
    case WStype_CONNECTED:
      Serial.println("WebSocket Connected");
      // Request calendar data on connect (YouTube and Weather use direct API calls)
      requestCalendarEvents();
      break;
      
    case WStype_TEXT:
      handleWebSocketMessage((char*)payload);
      break;
      
    case WStype_BIN:
      handleAudioChunk(payload, length);
      break;
  }
}

void handleWebSocketMessage(char* payload) {
  StaticJsonDocument<2048> doc;
  DeserializationError error = deserializeJson(doc, payload);
  
  if (error) {
    Serial.print("JSON parse error: ");
    Serial.println(error.c_str());
    return;
  }
  
  const char* type = doc["type"];
  
  if (strcmp(type, "transcript") == 0) {
    currentTranscription = doc["text"].as<String>();
    if (currentScreen == SCREEN_ASSISTANT) {
      drawAssistantScreen();
    }
  }
  else if (strcmp(type, "response_chunk") == 0) {
    assistantState = THINKING;
    currentResponse += doc["text"].as<String>();
    if (currentScreen == SCREEN_ASSISTANT) {
      drawAssistantScreen();
    }
  }
  else if (strcmp(type, "audio_chunk") == 0) {
    assistantState = SPEAKING;
    // Audio data will come via binary frame
  }
  else if (strcmp(type, "response_complete") == 0) {
    assistantState = IDLE;
    currentTranscription = "";
    currentResponse = "";
    if (currentScreen == SCREEN_ASSISTANT) {
      drawAssistantScreen();
    }
  }
  else if (strcmp(type, "calendar") == 0) {
    todaysEvents.clear();
    JsonArray events = doc["events"];
    for (JsonObject event : events) {
      CalendarEvent ce;
      ce.title = event["title"].as<String>();
      ce.time = event["time"].as<String>();
      ce.minutesUntil = event["minutes_until"];
      todaysEvents.push_back(ce);
    }
    if (currentScreen == SCREEN_CALENDAR) {
      drawCalendarScreen();
    }
  }
}

void handleAudioChunk(uint8_t* data, size_t length) {
  // Play audio through I2S speaker
  // TODO: Implement I2S audio playback
  Serial.printf("Received audio chunk: %d bytes\n", length);
}
