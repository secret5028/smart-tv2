#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <JPEGDEC.h>
#include <WiFi.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <esp32-hal-psram.h>

#ifndef WIFI_SSID
#define WIFI_SSID "YOUR_SSID"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "YOUR_PASSWORD"
#endif

#ifndef SERVER_HOST
#define SERVER_HOST "168.107.29.57"
#endif

#ifndef SERVER_PORT
#define SERVER_PORT 9000
#endif

#ifndef API_PORT
#define API_PORT 9001
#endif

static constexpr gpio_num_t PIN_SYS_EN = GPIO_NUM_41;
static constexpr gpio_num_t PIN_SYS_OUT = GPIO_NUM_40;
static constexpr gpio_num_t PIN_I2S_BCLK = GPIO_NUM_14;
static constexpr gpio_num_t PIN_I2S_LRCK = GPIO_NUM_13;
static constexpr gpio_num_t PIN_I2S_DOUT = GPIO_NUM_12;

static constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;
static constexpr size_t AUDIO_PLAY_CHUNK = 512;
static constexpr size_t RX_BUF_SIZE = 80 * 1024;
static constexpr uint32_t TCP_TIMEOUT_MS = 8000;
static constexpr uint32_t RECONNECT_DELAY_MS = 3000;
static constexpr uint8_t MAGIC0 = 0xAA;
static constexpr uint8_t MAGIC1 = 0xBB;
static constexpr uint8_t TOUCH_ADDR = 0x15;
static constexpr uint16_t SWIPE_MIN_DELTA = 60;
static constexpr uint16_t SWIPE_MAX_OFF_AXIS = 50;
static constexpr uint32_t CHANNEL_LABEL_MS = 3000;
static constexpr uint32_t BUFFER_SCREEN_INTERVAL_MS = 120;
static constexpr size_t MAX_CHANNELS = 24;

struct TouchPins {
    int sda;
    int scl;
};

static constexpr TouchPins TOUCH_CANDIDATES[] = {
    {11, 10},
    {18, 17},
    {47, 48},
    {2, 1},
    {39, 38},
};

enum class UiMode {
    Connecting,
    Buffering,
    Streaming,
    Error,
};

struct TouchState {
    bool active = false;
    bool wasDown = false;
    uint16_t startX = 0;
    uint16_t startY = 0;
    uint32_t startAt = 0;
};

static TFT_eSPI tft;
static JPEGDEC jpeg;
static uint8_t *gRxBuf = nullptr;
static bool gI2sReady = false;
static bool gTouchReady = false;
static TouchPins gTouchPins = {-1, -1};
static TouchState gTouchState;
static UiMode gUiMode = UiMode::Connecting;
static String gUiMessage = "connecting...";
static uint32_t gLastBufferDrawAt = 0;
static uint32_t gChannelOverlayUntil = 0;
static uint32_t gLastFrameAt = 0;
static bool gReconnectRequested = false;
static String gPendingChannelName;
static String gChannelKeys[MAX_CHANNELS];
static String gChannelNames[MAX_CHANNELS];
static size_t gChannelCount = 0;
static int gCurrentChannelIndex = 0;

static uint32_t readLe32(const uint8_t *buf) {
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

static uint16_t gray565(uint8_t gray) {
    return (uint16_t)(((gray >> 3) << 11) | ((gray >> 2) << 5) | (gray >> 3));
}

static void setUiMode(UiMode mode, const String &message) {
    gUiMode = mode;
    gUiMessage = message;
    if (mode != UiMode::Streaming) {
        gLastBufferDrawAt = 0;
    }
}

static void showChannelOverlay() {
    if (millis() > gChannelOverlayUntil || gPendingChannelName.isEmpty()) {
        return;
    }

    const int pad = 4;
    const int x = 6;
    const int y = 6;
    tft.setTextFont(1);
    tft.setTextSize(1);
    int w = (int)gPendingChannelName.length() * 6 + pad * 2;
    if (w > tft.width() - 12) {
        w = tft.width() - 12;
    }
    tft.fillRect(x - 2, y - 2, w + 4, 14, TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString(gPendingChannelName, x + pad, y + 1);
}

static void drawBufferingNoise() {
    uint32_t now = millis();
    if (now - gLastBufferDrawAt < BUFFER_SCREEN_INTERVAL_MS) {
        return;
    }
    gLastBufferDrawAt = now;

    for (int y = 0; y < tft.height(); ++y) {
        uint8_t gray = 70 + (esp_random() % 120);
        tft.drawFastHLine(0, y, tft.width(), gray565(gray));
    }

    for (int i = 0; i < 180; ++i) {
        int x = esp_random() % tft.width();
        int y = esp_random() % tft.height();
        uint16_t c = (esp_random() & 1U) ? TFT_WHITE : TFT_BLACK;
        tft.drawPixel(x, y, c);
    }

    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(gUiMessage, tft.width() / 2, tft.height() / 2 - 8);
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.drawString("analog buffer screen", tft.width() / 2, tft.height() / 2 + 18);
    tft.setTextDatum(TL_DATUM);
    showChannelOverlay();
}

static int jpegDraw(JPEGDRAW *pDraw) {
    tft.pushImage(pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight, pDraw->pPixels);
    return 1;
}

static void showStatus(const char *title, const char *msg, uint16_t color) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextSize(2);
    tft.setTextColor(color, TFT_BLACK);
    tft.drawString(title, 12, 40);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(msg, 12, 100);
}

static void initI2S() {
    if (gI2sReady) {
        return;
    }

    i2s_config_t config = {};
    config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    config.sample_rate = AUDIO_SAMPLE_RATE;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = 0;
    config.dma_buf_count = 8;
    config.dma_buf_len = 512;
    config.use_apll = false;
    config.tx_desc_auto_clear = true;
    config.fixed_mclk = 0;

    i2s_pin_config_t pins = {};
    pins.bck_io_num = PIN_I2S_BCLK;
    pins.ws_io_num = PIN_I2S_LRCK;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num = I2S_PIN_NO_CHANGE;

    if (i2s_driver_install(I2S_NUM_1, &config, 0, nullptr) != ESP_OK) {
        Serial.println("i2s_driver_install failed");
        return;
    }
    if (i2s_set_pin(I2S_NUM_1, &pins) != ESP_OK) {
        Serial.println("i2s_set_pin failed");
        return;
    }

    i2s_zero_dma_buffer(I2S_NUM_1);
    gI2sReady = true;
}

static void playPcm(const uint8_t *samples, size_t count) {
    if (!gI2sReady || count == 0) {
        return;
    }

    static int16_t stereo[AUDIO_PLAY_CHUNK * 2];
    size_t offset = 0;

    while (offset < count) {
        size_t chunkSamples = count - offset;
        if (chunkSamples > AUDIO_PLAY_CHUNK) {
            chunkSamples = AUDIO_PLAY_CHUNK;
        }

        for (size_t i = 0; i < chunkSamples; ++i) {
            int16_t s = (int16_t)((((int)samples[offset + i]) - 128) << 8);
            stereo[i * 2] = s;
            stereo[i * 2 + 1] = s;
        }

        size_t written = 0;
        i2s_write(
            I2S_NUM_1,
            stereo,
            chunkSamples * sizeof(int16_t) * 2,
            &written,
            portMAX_DELAY
        );
        offset += chunkSamples;
    }
}

static bool httpRequest(const char *method, const String &path, String &body) {
    WiFiClient client;
    if (!client.connect(SERVER_HOST, API_PORT)) {
        return false;
    }

    client.printf(
        "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\nContent-Length: 0\r\n\r\n",
        method,
        path.c_str(),
        SERVER_HOST,
        API_PORT
    );

    uint32_t deadline = millis() + 5000;
    while (client.connected() && !client.available()) {
        if ((int32_t)(millis() - deadline) > 0) {
            client.stop();
            return false;
        }
        delay(1);
    }

    String response;
    while (client.connected() || client.available()) {
        while (client.available()) {
            response += (char)client.read();
        }
        delay(1);
    }
    client.stop();

    int sep = response.indexOf("\r\n\r\n");
    if (sep < 0) {
        return false;
    }
    body = response.substring(sep + 4);
    return true;
}

static int findMatchingBrace(const String &json, int openPos) {
    int depth = 0;
    for (int i = openPos; i < json.length(); ++i) {
        char c = json[i];
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }
    return -1;
}

static String extractJsonString(const String &json, const String &field, int start, int end) {
    String needle = "\"" + field + "\"";
    int keyPos = json.indexOf(needle, start);
    if (keyPos < 0 || keyPos >= end) {
        return String();
    }
    int colon = json.indexOf(':', keyPos + needle.length());
    if (colon < 0 || colon >= end) {
        return String();
    }
    int q1 = json.indexOf('"', colon + 1);
    if (q1 < 0 || q1 >= end) {
        return String();
    }
    int q2 = json.indexOf('"', q1 + 1);
    if (q2 < 0 || q2 >= end) {
        return String();
    }
    return json.substring(q1 + 1, q2);
}

static void setFallbackChannelList() {
    gChannelCount = 1;
    gChannelKeys[0] = "kbs";
    gChannelNames[0] = "kbs";
    gCurrentChannelIndex = 0;
    gPendingChannelName = gChannelNames[0];
}

static bool loadChannelList() {
    String body;
    if (!httpRequest("GET", "/channels", body)) {
        return false;
    }

    size_t count = 0;
    int pos = 0;
    while (count < MAX_CHANNELS) {
        int keyOpen = body.indexOf('"', pos);
        if (keyOpen < 0) {
            break;
        }
        int keyClose = body.indexOf('"', keyOpen + 1);
        if (keyClose < 0) {
            break;
        }
        String key = body.substring(keyOpen + 1, keyClose);
        int colon = body.indexOf(':', keyClose + 1);
        int objOpen = body.indexOf('{', colon + 1);
        if (colon < 0 || objOpen < 0) {
            break;
        }
        int objClose = findMatchingBrace(body, objOpen);
        if (objClose < 0) {
            break;
        }

        String name = extractJsonString(body, "name", objOpen, objClose);
        if (!key.isEmpty()) {
            gChannelKeys[count] = key;
            gChannelNames[count] = name.isEmpty() ? key : name;
            ++count;
        }
        pos = objClose + 1;
    }

    if (count == 0) {
        return false;
    }

    gChannelCount = count;
    return true;
}

static bool syncCurrentChannel() {
    String body;
    if (!httpRequest("GET", "/status", body)) {
        return false;
    }

    String channel = extractJsonString(body, "channel", 0, body.length());
    String name = extractJsonString(body, "name", 0, body.length());
    if (channel.isEmpty()) {
        return false;
    }

    for (size_t i = 0; i < gChannelCount; ++i) {
        if (gChannelKeys[i] == channel) {
            gCurrentChannelIndex = (int)i;
            gPendingChannelName = name.isEmpty() ? gChannelNames[i] : name;
            return true;
        }
    }
    return false;
}

static bool postChannelChange(const String &key) {
    String body;
    return httpRequest("POST", "/channel/" + key, body);
}

static bool cstRead(uint8_t reg, uint8_t *dst, size_t len) {
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    int got = Wire.requestFrom((int)TOUCH_ADDR, (int)len);
    if (got != (int)len) {
        while (Wire.available()) {
            (void)Wire.read();
        }
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        dst[i] = (uint8_t)Wire.read();
    }
    return true;
}

static bool initTouch() {
    for (const TouchPins &pins : TOUCH_CANDIDATES) {
        Wire.end();
        Wire.begin(pins.sda, pins.scl, 400000U);
        Wire.beginTransmission(TOUCH_ADDR);
        if (Wire.endTransmission() == 0) {
            gTouchPins = pins;
            gTouchReady = true;
            Serial.printf("touch ready on SDA=%d SCL=%d\n", pins.sda, pins.scl);
            uint8_t mode = 0x00;
            (void)cstRead(0x00, &mode, 1);
            return true;
        }
    }

    Serial.println("touch controller not found");
    return false;
}

static bool readTouchPoint(uint16_t &x, uint16_t &y) {
    if (!gTouchReady) {
        return false;
    }

    uint8_t buf[5];
    if (!cstRead(0x02, buf, sizeof(buf))) {
        return false;
    }
    if ((buf[0] & 0x0F) == 0) {
        return false;
    }

    uint16_t rawX = (uint16_t)(((buf[1] & 0x0F) << 8) | buf[2]);
    uint16_t rawY = (uint16_t)(((buf[3] & 0x0F) << 8) | buf[4]);

    uint16_t mappedX = rawY;
    uint16_t mappedY = rawX;

    if (mappedX >= (uint16_t)tft.width()) {
        mappedX = (uint16_t)(tft.width() - 1);
    }
    if (mappedY >= (uint16_t)tft.height()) {
        mappedY = (uint16_t)(tft.height() - 1);
    }

    x = mappedX;
    y = mappedY;
    return true;
}

static void beginChannelOverlay(const String &name) {
    gPendingChannelName = name;
    gChannelOverlayUntil = millis() + CHANNEL_LABEL_MS;
}

static void requestChannelDelta(int delta) {
    if (gChannelCount == 0 || delta == 0) {
        return;
    }

    int next = gCurrentChannelIndex + delta;
    if (next < 0) {
        next = (int)gChannelCount - 1;
    } else if (next >= (int)gChannelCount) {
        next = 0;
    }

    if (!postChannelChange(gChannelKeys[next])) {
        setUiMode(UiMode::Error, "channel change failed");
        drawBufferingNoise();
        delay(400);
        return;
    }

    gCurrentChannelIndex = next;
    beginChannelOverlay(gChannelNames[next]);
    setUiMode(UiMode::Buffering, "buffering...");
    gReconnectRequested = true;
}

static void pollTouch() {
    uint16_t x = 0;
    uint16_t y = 0;
    bool down = readTouchPoint(x, y);
    uint32_t now = millis();

    if (down && !gTouchState.wasDown) {
        gTouchState.active = true;
        gTouchState.startX = x;
        gTouchState.startY = y;
        gTouchState.startAt = now;
    } else if (!down && gTouchState.wasDown && gTouchState.active) {
        int dx = (int)x - (int)gTouchState.startX;
        int dy = (int)y - (int)gTouchState.startY;

        if (abs(dx) >= SWIPE_MIN_DELTA && abs(dy) <= SWIPE_MAX_OFF_AXIS) {
            if (dx < 0) {
                requestChannelDelta(+1);
            } else {
                requestChannelDelta(-1);
            }
        }
        gTouchState.active = false;
    }

    gTouchState.wasDown = down;
}

static void serviceUi() {
    pollTouch();
    if (gUiMode != UiMode::Streaming) {
        drawBufferingNoise();
    }
}

static void connectWifi() {
    showStatus("WIFI", "connecting...", TFT_YELLOW);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t startAt = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if ((uint32_t)(millis() - startAt) >= 20000U) {
            showStatus("WIFI", "timeout, reboot", TFT_RED);
            delay(1000);
            ESP.restart();
        }
        delay(250);
    }

    String ip = WiFi.localIP().toString();
    Serial.printf("WiFi connected: %s\n", ip.c_str());
    showStatus("WIFI", ip.c_str(), TFT_GREEN);
    delay(500);
}

static bool recvExact(WiFiClient &client, uint8_t *dst, size_t n) {
    size_t received = 0;
    uint32_t deadline = millis() + TCP_TIMEOUT_MS;

    while (received < n) {
        serviceUi();

        if (gReconnectRequested) {
            client.stop();
            return false;
        }
        if (!client.connected()) {
            return false;
        }
        if ((int32_t)(millis() - deadline) > 0) {
            return false;
        }

        int available = client.available();
        if (available <= 0) {
            delay(1);
            continue;
        }

        size_t want = n - received;
        if (want > (size_t)available) {
            want = (size_t)available;
        }

        int got = client.read(dst + received, want);
        if (got <= 0) {
            delay(1);
            continue;
        }

        received += (size_t)got;
        deadline = millis() + TCP_TIMEOUT_MS;
    }

    return true;
}

static void streamLoop() {
    while (true) {
        if (WiFi.status() != WL_CONNECTED) {
            setUiMode(UiMode::Connecting, "wifi reconnecting...");
            WiFi.reconnect();
            uint32_t waitUntil = millis() + 5000;
            while ((int32_t)(millis() - waitUntil) < 0) {
                serviceUi();
                delay(1);
            }
            continue;
        }

        if (gChannelCount == 0 && !loadChannelList()) {
            setFallbackChannelList();
        }
        (void)syncCurrentChannel();

        WiFiClient client;
        setUiMode(UiMode::Connecting, "connecting...");
        Serial.printf("Connecting to %s:%d\n", SERVER_HOST, SERVER_PORT);

        uint32_t connectStartedAt = millis();
        while (!client.connect(SERVER_HOST, SERVER_PORT)) {
            if (WiFi.status() != WL_CONNECTED) {
                break;
            }
            setUiMode(UiMode::Connecting, "connect failed");
            uint32_t until = millis() + RECONNECT_DELAY_MS;
            while ((int32_t)(millis() - until) < 0) {
                serviceUi();
                delay(1);
            }
            connectStartedAt = millis();
        }

        if (!client.connected()) {
            continue;
        }

        client.setNoDelay(true);
        initI2S();
        gReconnectRequested = false;
        setUiMode(UiMode::Buffering, "buffering...");
        beginChannelOverlay(gPendingChannelName.isEmpty() ? gChannelKeys[gCurrentChannelIndex] : gPendingChannelName);

        uint32_t fpsCount = 0;
        uint32_t lastStatsAt = millis();
        bool haveFrame = false;

        while (client.connected()) {
            uint8_t magic[2];
            if (!recvExact(client, magic, sizeof(magic))) {
                break;
            }

            while (magic[0] != MAGIC0 || magic[1] != MAGIC1) {
                Serial.printf("bad magic: %02X %02X\n", magic[0], magic[1]);
                magic[0] = magic[1];
                if (!recvExact(client, &magic[1], 1)) {
                    client.stop();
                    break;
                }
            }

            if (!client.connected() || gReconnectRequested) {
                break;
            }

            uint8_t sizeBuf[4];
            if (!recvExact(client, sizeBuf, sizeof(sizeBuf))) {
                break;
            }
            uint32_t videoSize = readLe32(sizeBuf);
            if (videoSize == 0 || videoSize >= RX_BUF_SIZE) {
                Serial.printf("invalid video size: %u\n", (unsigned)videoSize);
                break;
            }

            if (!recvExact(client, gRxBuf, videoSize)) {
                break;
            }

            if (!recvExact(client, sizeBuf, sizeof(sizeBuf))) {
                break;
            }
            uint32_t audioSize = readLe32(sizeBuf);
            if (audioSize >= (RX_BUF_SIZE - videoSize)) {
                Serial.printf("invalid audio size: %u\n", (unsigned)audioSize);
                break;
            }

            if (audioSize > 0 && !recvExact(client, gRxBuf + videoSize, audioSize)) {
                break;
            }

            int opened = jpeg.openRAM(gRxBuf, (int)videoSize, jpegDraw);
            if (opened) {
                jpeg.setPixelType(RGB565_BIG_ENDIAN);
                if (jpeg.decode(0, 0, 0) == 0) {
                    Serial.println("jpeg decode failed");
                }
                jpeg.close();
                haveFrame = true;
                setUiMode(UiMode::Streaming, "");
                gLastFrameAt = millis();
                showChannelOverlay();
            } else {
                Serial.println("jpeg openRAM failed");
            }

            playPcm(gRxBuf + videoSize, audioSize);
            ++fpsCount;
            pollTouch();

            uint32_t now = millis();
            if ((uint32_t)(now - lastStatsAt) >= 1000U) {
                Serial.printf(
                    "fps=%u free_heap=%u wifi=%d frame_age=%u\n",
                    (unsigned)fpsCount,
                    (unsigned)ESP.getFreeHeap(),
                    (int)WiFi.status(),
                    (unsigned)(haveFrame ? (now - gLastFrameAt) : (now - connectStartedAt))
                );
                fpsCount = 0;
                lastStatsAt = now;
            }
        }

        client.stop();
        gReconnectRequested = false;
        setUiMode(UiMode::Buffering, "reconnecting...");
        uint32_t until = millis() + RECONNECT_DELAY_MS;
        while ((int32_t)(millis() - until) < 0) {
            serviceUi();
            delay(1);
        }
    }
}

void setup() {
    pinMode(PIN_SYS_EN, OUTPUT);
    digitalWrite(PIN_SYS_EN, HIGH);
    pinMode(PIN_SYS_OUT, INPUT_PULLUP);

    Serial.begin(115200);
    delay(200);

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
    tft.init();
    tft.setRotation(3);
    tft.fillScreen(TFT_BLACK);

    gRxBuf = (uint8_t *)ps_malloc(RX_BUF_SIZE);
    if (gRxBuf == nullptr) {
        gRxBuf = (uint8_t *)malloc(RX_BUF_SIZE);
    }

    if (gRxBuf == nullptr) {
        showStatus("ERROR", "buffer alloc failed", TFT_RED);
        while (true) {
            delay(1000);
        }
    }

    connectWifi();
    initTouch();
    if (!loadChannelList()) {
        setFallbackChannelList();
    }
    (void)syncCurrentChannel();
    beginChannelOverlay(gPendingChannelName.isEmpty() ? gChannelKeys[gCurrentChannelIndex] : gPendingChannelName);
    streamLoop();
}

void loop() {
}
