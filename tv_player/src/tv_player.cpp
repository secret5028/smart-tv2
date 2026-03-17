#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <JPEGDEC.h>
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
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 250;
static constexpr uint32_t CHANNEL_LABEL_MS = 3000;

struct ChannelEntry {
    const char *key;
    const char *label;
};

static constexpr ChannelEntry CHANNELS[] = {
    {"kbs", "KBS"},
    {"ytn", "YTN"},
    {"sbsnews", "SBS"},
    {"kbsworld", "KBSW"},
    {"jtbc", "JTBC"},
    {"mbn", "MBN"},
    {"goodtv", "GOODTV"},
    {"ebs1", "EBS1"},
    {"ebs2", "EBS2"},
    {"ebskids", "EBSKIDS"},
    {"fgtv", "FGTV"},
    {"lotte", "LOTTE"},
    {"mbc_chuncheon", "MBC-CC"},
    {"mbc_yeosu", "MBC-YS"},
    {"jibs", "JIBS"},
    {"jtv", "JTV"},
    {"oun", "OUN"},
    {"wshopping", "WSHOP"},
    {"shinsegae", "SSG"},
    {"nhtv", "NHTV"},
};

static TFT_eSPI tft;
static JPEGDEC jpeg;
static uint8_t *gRxBuf = nullptr;
static bool gI2sReady = false;
static size_t gChannelIndex = 0;
static uint32_t gChannelOverlayUntil = 0;
static uint32_t gLastButtonAt = 0;
static bool gLastButtonPressed = false;
static bool gLastButtonLevel = true;

static uint32_t readLe32(const uint8_t *buf) {
    return (uint32_t)buf[0] |
           ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

int jpegDraw(JPEGDRAW *pDraw) {
    tft.pushImage(pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight, pDraw->pPixels);
    return 1;
}

static void drawChannelOverlay() {
    if ((int32_t)(millis() - gChannelOverlayUntil) > 0) {
        return;
    }
    const char *label = CHANNELS[gChannelIndex].label;
    int w = (int)strlen(label) * 6 + 10;
    tft.fillRect(4, 4, w, 14, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString(label, 8, 7);
}

void showStatus(const char *title, const char *msg, uint16_t color) {
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

void connectWifi() {
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

void initI2S() {
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

    esp_err_t err = i2s_driver_install(I2S_NUM_1, &config, 0, nullptr);
    if (err != ESP_OK) {
        Serial.printf("i2s_driver_install failed: %d\n", (int)err);
        return;
    }

    err = i2s_set_pin(I2S_NUM_1, &pins);
    if (err != ESP_OK) {
        Serial.printf("i2s_set_pin failed: %d\n", (int)err);
        return;
    }

    i2s_zero_dma_buffer(I2S_NUM_1);
    gI2sReady = true;
}

void playPcm(const uint8_t *samples, size_t count) {
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
        i2s_write(I2S_NUM_1, stereo, chunkSamples * sizeof(int16_t) * 2, &written, portMAX_DELAY);
        offset += chunkSamples;
    }
}

bool postChannel(const char *key) {
    WiFiClient api;
    Serial.printf("channel api -> %s:%d / %s\n", SERVER_HOST, API_PORT, key);
    if (!api.connect(SERVER_HOST, API_PORT)) {
        Serial.println("channel api connect failed");
        return false;
    }

    api.printf(
        "POST /channel/%s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\nContent-Length: 0\r\n\r\n",
        key,
        SERVER_HOST,
        API_PORT
    );

    uint32_t deadline = millis() + 3000;
    while (api.connected() && !api.available()) {
        if ((int32_t)(millis() - deadline) > 0) {
            api.stop();
            return false;
        }
        delay(1);
    }

    bool ok = false;
    while (api.connected() || api.available()) {
        String line = api.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
            Serial.printf("channel api resp: %s\n", line.c_str());
        }
        if (line.startsWith("HTTP/1.1 200") || line.startsWith("HTTP/1.0 200")) {
            ok = true;
        }
        if (line.length() == 0) {
            break;
        }
    }
    api.stop();

    if (ok) {
        gChannelOverlayUntil = millis() + CHANNEL_LABEL_MS;
        Serial.printf("channel -> %s\n", key);
    }
    return ok;
}

bool handleChannelButton(WiFiClient *streamClient) {
    int rawLevel = digitalRead(PIN_SYS_OUT);
    bool pressed = rawLevel == LOW;
    uint32_t now = millis();

    if ((rawLevel == LOW) != gLastButtonLevel) {
        Serial.printf("button raw=%d pressed=%d\n", rawLevel, pressed ? 1 : 0);
        gLastButtonLevel = (rawLevel == LOW);
    }

    if (pressed && !gLastButtonPressed && (uint32_t)(now - gLastButtonAt) >= BUTTON_DEBOUNCE_MS) {
        gLastButtonAt = now;
        gChannelIndex = (gChannelIndex + 1) % (sizeof(CHANNELS) / sizeof(CHANNELS[0]));
        Serial.printf("button accepted -> next channel %s\n", CHANNELS[gChannelIndex].key);
        if (postChannel(CHANNELS[gChannelIndex].key)) {
            showStatus("CHANNEL", CHANNELS[gChannelIndex].label, TFT_GREEN);
            delay(250);
            if (streamClient != nullptr) {
                streamClient->stop();
            }
            gLastButtonPressed = pressed;
            return true;
        }
    }

    gLastButtonPressed = pressed;
    return false;
}

bool recvExact(WiFiClient &client, uint8_t *dst, size_t n) {
    size_t received = 0;
    uint32_t deadline = millis() + TCP_TIMEOUT_MS;

    while (received < n) {
        if (handleChannelButton(&client)) {
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

void streamLoop() {
    while (true) {
        if (WiFi.status() != WL_CONNECTED) {
            showStatus("WIFI", "reconnecting...", TFT_ORANGE);
            WiFi.reconnect();
            delay(5000);
            continue;
        }

        WiFiClient client;
        showStatus("TCP", "connecting...", TFT_YELLOW);
        Serial.printf("Connecting to %s:%d\n", SERVER_HOST, SERVER_PORT);

        if (!client.connect(SERVER_HOST, SERVER_PORT)) {
            showStatus("TCP", "connect failed", TFT_RED);
            delay(RECONNECT_DELAY_MS);
            continue;
        }

        client.setNoDelay(true);
        initI2S();
        showStatus("TCP", "connected", TFT_GREEN);
        delay(250);

        uint32_t fpsCount = 0;
        uint32_t lastStatsAt = millis();

        while (client.connected()) {
            if (handleChannelButton(&client)) {
                break;
            }

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

            if (!client.connected()) {
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
                int rc = jpeg.decode(0, 0, 0);
                if (rc == 0) {
                    Serial.println("jpeg decode failed");
                }
                jpeg.close();
                drawChannelOverlay();
            } else {
                Serial.println("jpeg openRAM failed");
            }

            playPcm(gRxBuf + videoSize, audioSize);
            ++fpsCount;

            uint32_t now = millis();
            if ((uint32_t)(now - lastStatsAt) >= 1000U) {
                Serial.printf("fps=%u free_heap=%u\n", (unsigned)fpsCount, (unsigned)ESP.getFreeHeap());
                fpsCount = 0;
                lastStatsAt = now;
            }
        }

        client.stop();
        showStatus("TCP", "reconnecting...", TFT_ORANGE);
        delay(RECONNECT_DELAY_MS);
    }
}

void setup() {
    pinMode(PIN_SYS_EN, OUTPUT);
    digitalWrite(PIN_SYS_EN, HIGH);
    pinMode(PIN_SYS_OUT, INPUT_PULLUP);

    Serial.begin(115200);
    delay(200);
    Serial.printf("button boot raw=%d\n", digitalRead(PIN_SYS_OUT));

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
    streamLoop();
}

void loop() {
}
