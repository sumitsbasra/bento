// Bento
// A desk-top voice assistant on a 466x466 touchscreen ESP32 that connects to OpenClaw
// Swipe between 5 screens: YouTube (top), Calendar (left), Assistant (center), Clock (right), HomeKit Lights (bottom)
// Tap the assistant screen to talk to it

#include <Waveshare_AMOLED.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <TouchLib.h>
#include <time.h>
#include "HomeSpan.h"
#include <AnimatedGIF.h>

// === GIF ASSETS ===
// Each GIF should be ~240x240px, exported as a looping animation.
// Convert to C arrays with: xxd -i ambient.gif > gif_ambient.h  (repeat for each)
// Then place the .h files alongside bento.ino.
#include "gif_ambient.h"    // const uint8_t gif_ambient[]   PROGMEM; extern const uint32_t gif_ambient_len;
#include "gif_listening.h"  // const uint8_t gif_listening[] PROGMEM; extern const uint32_t gif_listening_len;
#include "gif_thinking.h"   // const uint8_t gif_thinking[]  PROGMEM; extern const uint32_t gif_thinking_len;
#include "gif_speaking.h"   // const uint8_t gif_speaking[]  PROGMEM; extern const uint32_t gif_speaking_len;

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

// === OPENCLAW-VOICE WEBSOCKET (for assistant only) ===
const char* WS_HOST = "192.168.1.100"; // Your openclaw-voice server IP
const int WS_PORT = 8765;
WebSocketsClient webSocket;

// === HTTP CLIENT ===
WiFiClientSecure httpsClient;

// === SCREEN STATES ===
enum Screen {
  SCREEN_YOUTUBE,   // Top
  SCREEN_CALENDAR,  // Left
  SCREEN_ASSISTANT, // Center (default)
  SCREEN_CLOCK,     // Right
  SCREEN_LIGHTS,    // Bottom
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

// === LIGHTS STATE ===
struct LightState {
  const char* name;
  bool on;
  int brightness; // 0-100
};

LightState lights[4] = {
  {"Desk",    false, 80},
  {"Lamp",    false, 60},
  {"Strip",   false, 100},
  {"Monitor", false, 70}
};

// HAP characteristic pointers (set during HomeSpan setup)
SpanCharacteristic* hapPower[4];
SpanCharacteristic* hapBright[4];

// === GIF PLAYER ===
AnimatedGIF gif;
AssistantState activeGifState = (AssistantState)-1; // tracks which GIF is open

// GIF canvas offset — centres the animation on the 466×466 display.
// Populated after gif.open() using gif.getCanvasSize().
static int gifOffsetX = 0;
static int gifOffsetY = 80; // leave ~80px at top for screen indicator + breathing room

// Per-scanline draw callback used by AnimatedGIF
void gifDrawCallback(GIFDRAW* pDraw) {
  // Build a temporary RGB565 line, respecting transparency
  uint16_t lineBuffer[pDraw->iWidth];
  uint8_t* src = pDraw->pPixels;
  uint16_t* palette = pDraw->pPalette;

  for (int i = 0; i < pDraw->iWidth; i++) {
    uint8_t idx = src[i];
    // Skip transparent pixels (keep whatever is already on screen)
    if (pDraw->ucHasTransparency && idx == pDraw->ucTransparent) {
      // Read back the current pixel so we don't corrupt the text area.
      // For simplicity we just write black — the GIF bg is already black.
      lineBuffer[i] = TFT_BLACK;
    } else {
      lineBuffer[i] = palette[idx];
    }
  }

  int screenX = gifOffsetX + pDraw->iX;
  int screenY = gifOffsetY + pDraw->iY + pDraw->y;
  tft.pushImage(screenX, screenY, pDraw->iWidth, 1, lineBuffer);
}

// Open the correct GIF for the given state. Clears screen and resets offset.
void openGifForState(AssistantState state) {
  gif.close();

  const uint8_t* data = nullptr;
  uint32_t len = 0;

  switch (state) {
    case IDLE:      data = gif_ambient;   len = gif_ambient_len;   break;
    case LISTENING: data = gif_listening; len = gif_listening_len; break;
    case THINKING:  data = gif_thinking;  len = gif_thinking_len;  break;
    case SPEAKING:  data = gif_speaking;  len = gif_speaking_len;  break;
  }

  if (!data || !gif.open((uint8_t*)data, len, gifDrawCallback)) {
    Serial.printf("GIF open failed for state %d\n", state);
    return;
  }

  // Centre the GIF horizontally; leave top 80px for indicator
  int w, h;
  gif.getCanvasSize(&w, &h);
  gifOffsetX = (466 - w) / 2;
  // gifOffsetY stays at 80 unless the GIF is taller than the usable area
  gifOffsetY = 80;
  if (gifOffsetY + h > 350) gifOffsetY = max(0, 350 - h); // keep bottom 116px for text

  activeGifState = state;
}

// Call every loop iteration while on the assistant screen.
// Returns true if a new frame was rendered.
bool tickAssistantGif() {
  if (activeGifState == (AssistantState)-1) return false;

  int nextFrameMs;
  int result = gif.playFrame(false, &nextFrameMs);

  if (result == 0) {
    // End of GIF — loop it
    gif.reset();
  }

  return (result >= 0);
}

void drawLightsScreen(); // forward declaration needed by DEV_Lightbulb::update()

struct DEV_Lightbulb : Service::LightBulb {
  SpanCharacteristic* power;
  SpanCharacteristic* bright;
  int idx;

  DEV_Lightbulb(int lightIndex) : Service::LightBulb() {
    idx   = lightIndex;
    power = new Characteristic::On(lights[idx].on);
    bright = new Characteristic::Brightness(lights[idx].brightness);
    bright->setRange(0, 100, 1);
  }

  boolean update() override {
    if (power->updated())  lights[idx].on         = power->getNewVal<bool>();
    if (bright->updated()) lights[idx].brightness = bright->getNewVal<int>();
    if (currentScreen == SCREEN_LIGHTS) drawLightsScreen();
    return true;
  }
};

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
  requestCalendarEvents();

  // HomeKit via HomeSpan (WiFi already connected above)
  homeSpan.setWifiCredentials(WIFI_SSID, WIFI_PASSWORD);
  homeSpan.setPairingCode("031-45-154"); // change before pairing
  homeSpan.setSketchVersion("1.0");
  homeSpan.begin(Category::Bridges, "Bento");

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Name("Bento");
      new Characteristic::Manufacturer("DIY");
      new Characteristic::SerialNumber("DA-001");
      new Characteristic::Model("ESP32");
      new Characteristic::FirmwareRevision("1.0");
      new Characteristic::Identify();

  const char* accNames[] = {"Desk Light", "Lamp", "LED Strip", "Monitor Light"};
  for (int i = 0; i < 4; i++) {
    new SpanAccessory();
      new Service::AccessoryInformation();
        new Characteristic::Name(accNames[i]);
        new Characteristic::Identify();
      DEV_Lightbulb* lb = new DEV_Lightbulb(i);
      hapPower[i]  = lb->power;
      hapBright[i] = lb->bright;
  }

  // Initial screen
  drawCurrentScreen();
}

// === MAIN LOOP ===
void loop() {
  homeSpan.poll();
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
  // Horizontal row: Calendar ← Assistant → Clock
  if (currentScreen == SCREEN_YOUTUBE || currentScreen == SCREEN_LIGHTS) {
    return;
  }

  if (direction > 0) {
    if (currentScreen == SCREEN_CALENDAR)  currentScreen = SCREEN_ASSISTANT;
    else if (currentScreen == SCREEN_ASSISTANT) currentScreen = SCREEN_CLOCK;
  } else {
    if (currentScreen == SCREEN_CLOCK)     currentScreen = SCREEN_ASSISTANT;
    else if (currentScreen == SCREEN_ASSISTANT) currentScreen = SCREEN_CALENDAR;
  }

  drawCurrentScreen();
}

void swipeVertical(int direction) {
  // Vertical column: YouTube ↕ Assistant ↕ Lights
  if (currentScreen == SCREEN_CALENDAR || currentScreen == SCREEN_CLOCK) {
    return;
  }

  if (direction > 0) {
    if (currentScreen == SCREEN_YOUTUBE)   currentScreen = SCREEN_ASSISTANT;
    else if (currentScreen == SCREEN_ASSISTANT) currentScreen = SCREEN_LIGHTS;
  } else {
    if (currentScreen == SCREEN_LIGHTS)    currentScreen = SCREEN_ASSISTANT;
    else if (currentScreen == SCREEN_ASSISTANT) currentScreen = SCREEN_YOUTUBE;
  }

  drawCurrentScreen();
}

void handleTap(int x, int y) {
  if (currentScreen == SCREEN_ASSISTANT) {
    if (assistantState == IDLE) {
      startListening();
    } else if (assistantState == LISTENING) {
      stopListening();
    }
  } else if (currentScreen == SCREEN_LIGHTS) {
    // 2x2 card grid — determine which card was tapped
    const int MARGIN  = 20;
    const int GAP     = 14;
    const int TOP_OFF = 50;
    const int CARD_W  = (466 - MARGIN * 2 - GAP) / 2;       // 206
    const int CARD_H  = (466 - TOP_OFF - MARGIN - GAP) / 2; // 191

    for (int i = 0; i < 4; i++) {
      int col = i % 2;
      int row = i / 2;
      int cx  = MARGIN + col * (CARD_W + GAP);
      int cy  = TOP_OFF + row * (CARD_H + GAP);

      if (x >= cx && x < cx + CARD_W && y >= cy && y < cy + CARD_H) {
        toggleLight(i);
        break;
      }
    }
  }
}

// === SCREEN DRAWING ===
void drawCurrentScreen() {
  switch (currentScreen) {
    case SCREEN_YOUTUBE:    drawYouTubeScreen();   break;
    case SCREEN_CALENDAR:   drawCalendarScreen();  break;
    case SCREEN_ASSISTANT:  drawAssistantScreen(); break;
    case SCREEN_CLOCK:      drawClockScreen();     break;
    case SCREEN_LIGHTS:     drawLightsScreen();    break;
  }
}

void updateCurrentScreen() {
  static unsigned long lastUpdate = 0;
  static unsigned long lastDataRefresh = 0;
  unsigned long now = millis();

  // Clock refreshes every second
  if (now - lastUpdate > 1000) {
    if (currentScreen == SCREEN_CLOCK) drawClockScreen();
    lastUpdate = now;
  }

  // Data refresh every 5 minutes
  if (now - lastDataRefresh > 300000) {
    requestYouTubeStats();
    requestCalendarEvents();
    lastDataRefresh = now;
  }

  if (currentScreen == SCREEN_ASSISTANT) {
    static AssistantState lastState = (AssistantState)-1;

    // State changed → swap GIF and redraw text overlay
    if (assistantState != lastState) {
      openGifForState(assistantState);
      tft.fillScreen(TFT_BLACK);
      gif.playFrame(false, nullptr); // first frame
      drawAssistantTextOverlay();
      lastState = assistantState;
    } else {
      // Continuously advance the GIF animation
      tickAssistantGif();
    }

    // Keep the text overlay fresh when transcription/response updates arrive.
    // Only re-draw the text band — never overwrite the GIF area.
    static String lastTranscription;
    static String lastResponse;
    if (currentTranscription != lastTranscription || currentResponse != lastResponse) {
      drawAssistantTextOverlay();
      lastTranscription = currentTranscription;
      lastResponse = currentResponse;
    }
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

// Draw the static text layer (indicator + hint/transcription/response).
// Called once when the state changes; the GIF tick redraws the orb each frame
// but does NOT touch the text rows at the bottom.
void drawAssistantTextOverlay() {
  // Screen indicator (top ~30px)
  drawScreenIndicator();

  // Clear the text band at the bottom (y 350-466)
  tft.fillRect(0, 350, 466, 116, TFT_BLACK);

  switch (assistantState) {
    case IDLE:
      tft.setTextColor(TFT_DARKGREY);
      tft.setTextSize(2);
      tft.setCursor(145, 430);
      tft.print("Tap to speak");
      break;

    case LISTENING:
      tft.setTextColor(TFT_DARKGREY);
      tft.setTextSize(2);
      tft.setCursor(133, 356);
      tft.print("Tap to cancel");
      if (currentTranscription.length() > 0) {
        tft.setTextColor(TFT_WHITE);
        tft.setTextSize(2);
        drawWrappedText(currentTranscription, 30, 384, 406);
      }
      break;

    case THINKING:
      tft.setTextColor(TFT_WHITE);
      tft.setTextSize(2);
      tft.setCursor(155, 356);
      tft.print("Thinking...");
      break;

    case SPEAKING:
      if (currentResponse.length() > 0) {
        tft.setTextColor(TFT_WHITE);
        tft.setTextSize(2);
        drawWrappedText(currentResponse, 30, 356, 406);
      }
      break;
  }
}

// Full redraw — used when first entering the assistant screen or changing state.
void drawAssistantScreen() {
  tft.fillScreen(TFT_BLACK);
  openGifForState(assistantState);   // opens (or re-opens) the right GIF
  gif.playFrame(false, nullptr);     // render the first frame immediately
  drawAssistantTextOverlay();
}

// === CLOCK SCREEN ===

// Colors matching the Figma design (node 1-53, 466px frame)
#define CLOCK_FACE_COLOR  tft.color565(208, 208, 212)  // light gray
#define CLOCK_HAND_COLOR  tft.color565(  0,   9, 170)  // #0009AA deep blue
#define CLOCK_DOT_COLOR   tft.color565(220,  40,  40)  // red minute dot

// Blend two RGB565 colours linearly.  t=0 → c1,  t=1 → c2.
static uint16_t blendColor565(uint16_t c1, uint16_t c2, float t) {
  int r1 = (c1 >> 11) & 0x1F,  r2 = (c2 >> 11) & 0x1F;
  int g1 = (c1 >>  5) & 0x3F,  g2 = (c2 >>  5) & 0x3F;
  int b1 =  c1        & 0x1F,  b2 =  c2        & 0x1F;
  return (uint16_t)((int)(r1 + (r2 - r1) * t) << 11) |
         (uint16_t)((int)(g1 + (g2 - g1) * t) <<  5) |
         (uint16_t) (int)(b1 + (b2 - b1) * t);
}

// Draw a clock hand as a blurred pill.
// TFT_eSPI has no native blur, so we layer four drawWideLine passes:
// three progressively wider rings fading toward the face colour (soft glow),
// then the solid core — producing the same frosted look as canvas blur(6px).
void drawClockHand(int cx, int cy, float angleDeg,
                   float offset, float len, float W, uint16_t color) {
  float rad  = angleDeg * PI / 180.0f;
  float sinA = sin(rad),  cosA = cos(rad);
  float half = len / 2.0f;

  float x0 = cx + sinA * (offset + half);   // tip end
  float y0 = cy - cosA * (offset + half);
  float x1 = cx + sinA * (offset - half);   // back end
  float y1 = cy - cosA * (offset - half);

  uint16_t bg = CLOCK_FACE_COLOR;
  // Outer glow → inner core  (bg-blend fraction decreases inward)
  tft.drawWideLine(x0, y0, x1, y1, W + 18, blendColor565(color, bg, 0.82f), bg);
  tft.drawWideLine(x0, y0, x1, y1, W + 10, blendColor565(color, bg, 0.58f), bg);
  tft.drawWideLine(x0, y0, x1, y1, W +  4, blendColor565(color, bg, 0.28f), bg);
  tft.drawWideLine(x0, y0, x1, y1, W,      color, bg);
}

void drawClockScreen() {
  tft.fillScreen(TFT_BLACK);
  drawScreenIndicator();

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    tft.setTextColor(TFT_RED);
    tft.setTextSize(2);
    tft.setCursor(100, 200);
    tft.print("Time sync failed");
    return;
  }

  int hour   = timeinfo.tm_hour % 12;
  int minute = timeinfo.tm_min;

  const int cx = 233, cy = 233, faceRadius = 233;

  // ── Clock face ───────────────────────────────────────────────────────────
  tft.fillCircle(cx, cy, faceRadius, CLOCK_FACE_COLOR);

  // ── Hands — blurred via glow layers (frosted-glass approximation) ─────────
  // Each hand is drawn as four concentric passes: wide faint rings outward
  // fading toward the face colour, then the solid core inward.
  // The outermost ring also serves as the frosted-overlay: its very light
  // tint lightens the face around each hand, giving the same washed-out look
  // as the rgba(255,255,255,0.35) overlay used in the HTML preview.
  float minuteAngle = minute * 6.0f;
  drawClockHand(cx, cy, minuteAngle, 95.0f, 190.0f, 35.0f, CLOCK_HAND_COLOR);

  float hourAngle = hour * 30.0f + minute * 0.5f;
  drawClockHand(cx, cy, hourAngle, 116.0f, 88.0f, 35.0f, CLOCK_HAND_COLOR);

  // ── Red dot — sharp, drawn last so it sits above the blurred hands ────────
  float dotRad = minuteAngle * PI / 180.0f;
  int dotX = cx + (int)(sin(dotRad) * 153.0f);
  int dotY = cy - (int)(cos(dotRad) * 153.0f);
  tft.fillCircle(dotX, dotY, 38, CLOCK_DOT_COLOR);
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

// === SCREEN INDICATOR ===
void drawScreenIndicator() {
  // Cross pattern: 3 horizontal dots (Calendar/Assistant/Clock)
  //               + YouTube above + Lights below (both on the Assistant axis)
  int cx = 233;
  int cy = 30;
  int hSpacing = 25;
  int vSpacing = 22;

  // Horizontal row: Calendar(0), Assistant(1), Clock(2)
  Screen hScreens[3] = {SCREEN_CALENDAR, SCREEN_ASSISTANT, SCREEN_CLOCK};
  for (int i = 0; i < 3; i++) {
    int x = cx - hSpacing + i * hSpacing;
    bool active = (currentScreen == hScreens[i]);
    if (active) tft.fillCircle(x, cy, 5, TFT_WHITE);
    else        tft.drawCircle(x, cy, 5, TFT_DARKGREY);
  }

  // Vertical axis on Assistant (cx = 233)
  bool activeY = (currentScreen == SCREEN_YOUTUBE);
  if (activeY) tft.fillCircle(cx, cy - vSpacing, 5, TFT_WHITE);
  else         tft.drawCircle(cx, cy - vSpacing, 5, TFT_DARKGREY);

  bool activeL = (currentScreen == SCREEN_LIGHTS);
  if (activeL) tft.fillCircle(cx, cy + vSpacing, 5, TFT_WHITE);
  else         tft.drawCircle(cx, cy + vSpacing, 5, TFT_DARKGREY);
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

// === LIGHTS SCREEN ===

// Bulb icon — globe dome + tapered body + 3-bar socket.
// cx/cy = icon centre.  Matches Figma node 1-91 / 1-94 (≈47×82 px).
void drawBulbIcon(int cx, int cy, bool isOn) {
  uint16_t color = isOn
    ? tft.color565(255, 208,   0)   // bright yellow — ON
    : tft.color565(200, 160,  40);  // dim amber     — OFF

  // Globe dome
  tft.fillCircle(cx, cy - 14, 23, color);

  // Body below dome (filled rect + rounded bottom to merge with dome)
  tft.fillRect(cx - 14, cy - 5, 28, 26, color);
  tft.fillCircle(cx, cy + 20, 14, color);

  // Base/socket — 3 narrowing bars
  tft.fillRect(cx - 13, cy + 28, 26, 5, color);
  tft.fillRect(cx - 11, cy + 34, 22, 5, color);
  tft.fillRect(cx -  9, cy + 40, 18, 5, color);
}

void drawLightsScreen() {
  // ── Colours (Figma node 1-73) ──────────────────────────────────────────
  const uint16_t leftBg    = tft.color565( 20,  20,  20);  // #141414 near-black
  const uint16_t rightBg   = tft.color565( 38,  38,  38);  // #262626 medium dark
  const uint16_t leftCard  = tft.color565( 34,  34,  34);  // #222222
  const uint16_t rightCard = tft.color565( 54,  54,  54);  // #363636

  const int cx = 233, cy = 233, r = 233;

  tft.fillScreen(TFT_BLACK);

  // ── Left half: fill whole circle with left bg ──────────────────────────
  tft.fillCircle(cx, cy, r, leftBg);

  // ── Right half: scanline fill within the circle (x > cx+1) ────────────
  for (int scanY = -r; scanY <= r; scanY++) {
    int hw = (int)sqrtf((float)(r * r - scanY * scanY));
    int x0 = cx + 2;          // 2 px divider gap
    int x1 = cx + hw;
    if (x1 > x0)
      tft.drawFastHLine(x0, cy + scanY, x1 - x0, rightBg);
  }

  // ── Cards — lower half of each panel, rounded top corners ─────────────
  // Left card: x=17, y=233, w=209, h fills to circle bottom
  tft.fillRoundRect(17,  cy, 209, r, 20, leftCard);
  // Right card: x=240, mirrored
  tft.fillRoundRect(240, cy, 209, r, 20, rightCard);

  // ── Screen indicator (lights dot goes active automatically) ───────────
  drawScreenIndicator();

  // ── Bulb icons — Figma centres: left=(158,369) right=(307,369) ─────────
  drawBulbIcon(158, 369, lights[0].on);
  drawBulbIcon(307, 369, lights[1].on);
}

void toggleLight(int idx) {
  if (idx < 0 || idx >= 4) return;
  lights[idx].on = !lights[idx].on;
  // Notify HomeKit of the new state
  hapPower[idx]->setVal(lights[idx].on ? 1 : 0);
  hapBright[idx]->setVal(lights[idx].brightness);
  drawLightsScreen();
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
      // Request calendar data on connect
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
