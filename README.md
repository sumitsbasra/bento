# ESP32 Desk Assistant

A desk-top voice assistant on a 466x466 touchscreen ESP32 that connects to OpenClaw - swipe between 5 screens showing your assistant, calendar, clock, weather, and YouTube subscriber count. Tap the assistant screen to talk to it, and it responds with voice while showing transcriptions and responses on the display.

## Features

### 5 Swipeable Screens

**Screen Layout:**
```
        YouTube (top)
           ↕
Calendar ← Assistant → Clock
           ↕
        Weather (bottom)
```

1. **Assistant (Center)** - Main voice interaction screen
   - Tap to start listening
   - Shows transcription while listening
   - Displays AI response while speaking
   - Visual states: Idle, Listening, Thinking, Speaking

2. **YouTube (Top)** - Subscriber stats
   - Large subscriber count display
   - Daily gain indicator (faint grey at bottom)
   - Auto-updates every 5 minutes

3. **Calendar (Left)** - Today's events
   - Shows upcoming events
   - Time until next event
   - Clean card-based layout

4. **Clock (Right)** - Time and date
   - Large time display
   - Current date
   - Updates every second

5. **Weather (Bottom)** - Current weather
   - Temperature
   - Condition (Sunny, Cloudy, Rainy)
   - Location
   - Simple weather icons

## Hardware Requirements

- ESP32-S3 (or compatible)
- 466x466 touchscreen display (ST7789, GC9A01, or similar)
- I2S MEMS microphone
- I2S speaker or 3.5mm audio output
- WiFi connection

## Software Dependencies

```cpp
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <TouchLib.h>
#include <time.h>
```

Install via Arduino Library Manager:
- TFT_eSPI
- WebSockets by Markus Sattler
- ArduinoJson by Benoit Blanchon
- HTTPClient (included with ESP32 core)

## Setup

### 1. Configure WiFi

Update in code:
```cpp
const char* WIFI_SSID = "your-wifi-ssid";
const char* WIFI_PASSWORD = "your-wifi-password";
```

### 2. Get API Keys

**YouTube Data API:**
1. Go to [Google Cloud Console](https://console.cloud.google.com/)
2. Create a new project or select existing
3. Enable "YouTube Data API v3"
4. Create credentials → API Key
5. Find your Channel ID: Go to YouTube Studio → Settings → Channel → Advanced settings

**OpenWeatherMap API:**
1. Sign up at [OpenWeatherMap](https://openweathermap.org/api)
2. Get free API key from your account dashboard

Update in code:
```cpp
const char* YOUTUBE_API_KEY = "your-youtube-api-key";
const char* YOUTUBE_CHANNEL_ID = "your-channel-id";
const char* OPENWEATHER_API_KEY = "your-openweather-api-key";
const char* WEATHER_LOCATION = "San Francisco,US"; // City,Country code
```

### 3. Configure WebSocket Server (for Assistant & Calendar only)

Point to your openclaw-voice server:
```cpp
const char* WS_HOST = "192.168.1.100"; // Your openclaw-voice server IP
const int WS_PORT = 8765;
```

### 4. Configure Timezone

Update for your timezone:
```cpp
configTime(-8 * 3600, 0, "pool.ntp.org"); // PST example
```

### 5. Setup OpenClaw-Voice Backend (for Assistant & Calendar)

The ESP32 connects to OpenClaw for voice assistant functionality and calendar events only. YouTube and Weather data are fetched directly via APIs.

**Server Repository:** https://github.com/Purple-Horizons/openclaw-voice

**Expected WebSocket Messages:**

From ESP32 → Server:
```json
{"type": "start_listening"}
{"type": "stop_listening"}
{"type": "get_calendar"}
```

From Server → ESP32:
```json
{"type": "transcript", "text": "what you said", "final": true}
{"type": "response_chunk", "text": "AI response..."}
{"type": "response_complete", "text": "full response"}
{"type": "calendar", "events": [{"title": "Meeting", "time": "2:00 PM", "minutes_until": 30}]}
```

**Note:** YouTube stats and Weather data are fetched directly from Google/OpenWeatherMap APIs, not through OpenClaw.

### 6. Upload to ESP32

1. Open `esp32_desk_assistant.ino` in Arduino IDE
2. Select your ESP32 board
3. Configure TFT_eSPI for your display (edit `User_Setup.h`)
4. Upload!

## Touch Gestures

- **Tap** (on assistant screen) - Start/stop voice listening
- **Swipe left/right** - Navigate between Calendar ↔ Assistant ↔ Clock
- **Swipe up/down** - Navigate between YouTube ↔ Assistant ↔ Weather

## Navigation Rules

- Horizontal swipes only work from Calendar/Assistant/Clock
- Vertical swipes only work from YouTube/Assistant/Weather
- Assistant screen is the hub - connects to all other screens

## TODO / Future Enhancements

- [ ] Implement I2S audio input (microphone)
- [ ] Implement I2S audio output (speaker playback)
- [ ] Add wake word detection
- [ ] Improve animations (smoother transitions)
- [ ] Add more weather conditions and icons
- [ ] Persistent settings storage
- [ ] OTA updates
- [ ] Battery level indicator
- [ ] Custom voices/personas

## Architecture

```
┌─────────────────────────────────────────┐
│              ESP32 Display              │
└──┬──────────────┬───────────────┬───────┘
   │              │               │
   │ WiFi/WS      │ HTTPS         │ HTTPS
   │              │               │
   ↓              ↓               ↓
┌──────────┐  ┌─────────┐  ┌──────────┐
│OpenClaw- │  │ YouTube │  │OpenWeather│
│  Voice   │  │Data API │  │   API    │
│ WebSocket│  │         │  │          │
└────┬─────┘  └─────────┘  └──────────┘
     │
     ↓
┌──────────┐
│ OpenClaw │
│ Gateway  │
└────┬─────┘
     │
     ↓
┌──────────┐
│LLM+Tools │
│(Calendar)│
└──────────┘
```

**Data Flow:**
- **Assistant & Calendar** → OpenClaw WebSocket → LLM + Calendar API
- **YouTube Stats** → Direct HTTPS to YouTube Data API v3
- **Weather** → Direct HTTPS to OpenWeatherMap API

## License

MIT

## Credits

Built for use with [OpenClaw](https://openclaw.ai) - Personal AI Assistant
