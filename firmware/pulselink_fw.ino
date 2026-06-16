/*
 * PulseLink firmware — Stage 1 (ESP32 / FreeRTOS, runs in Wokwi)
 * Milestone M5: the device actually talks to telemetryd.
 *
 *   [sensor_task] --(bounded queue)--> [net_task] --TCP--> telemetryd
 *     prio 3, core 1                     prio 2, core 0   (HELLO, then stream)
 *   [heartbeat_task] --UDP--> telemetryd   (every 5s)
 *   [stats_task]     --> Serial            (every 5s)
 *
 * net_task owns a small connection state machine: join Wi-Fi -> TCP connect ->
 * HELLO/HELLO_ACK handshake -> stream framed packets, draining ACK/ERROR
 * replies. Any failure tears down and retries with exponential backoff; the
 * sampler keeps filling the (bounded) queue throughout, so a transient outage
 * just costs the oldest unsent samples, not a crash. A task watchdog resets the
 * chip if net_task ever wedges.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <math.h>
#include "DHTesp.h"   /* library: "DHT sensor library for ESPx" (see libraries.txt) */
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

/* telemetry.c + checksum.c are plain C shared with the server; the C++ sketch
 * only needs telemetry.h (no protocol.h, keeping C11 _Static_assert out of C++). */
extern "C" {
#include "telemetry.h"
}

/* ---- config ---- */
static const uint32_t DEVICE_ID        = 0x1001;
static const uint32_t FW_VERSION       = 0x00010005;   /* 1.0.5 (M5) */
static const uint32_t CAPABILITIES     = 0x1;
static const int      LED_PIN          = 23;  /* GPIO23: plain output pin, LED wired direct */
static const int      DHT_PIN          = 15;           /* wokwi-dht22 data line */
static const uint32_t SAMPLE_PERIOD_MS = 2000;         /* DHT22 samples at ~0.5 Hz */
static const int      QUEUE_LEN        = 16;

static const char    *WIFI_SSID        = "Wokwi-GUEST";  /* Wokwi's virtual AP */
static const char    *WIFI_PASS        = "";
/* With the Wokwi Private IoT Gateway running on the machine that also runs
 * telemetryd, the sim reaches it at host.wokwi.internal (TCP + UDP supported).
 * On real hardware, set this to the server's LAN IP instead. */
static const char    *SERVER_HOST      = "host.wokwi.internal";
static const uint16_t SERVER_TCP_PORT  = 9000;
static const uint16_t SERVER_UDP_PORT  = 9001;

static const uint32_t BACKOFF_MIN_MS   = 500;
static const uint32_t BACKOFF_MAX_MS   = 16000;
static const uint32_t WDT_TIMEOUT_S    = 10;

typedef struct {
    uint32_t seq;
    uint32_t timestamp_ms;
    int32_t  temperature;   /* centi-degC */
    int32_t  humidity;      /* centi-%    */
    bool     simulated;
} sensor_reading_t;

static QueueHandle_t g_queue;
static volatile uint32_t g_produced = 0;
static volatile uint32_t g_consumed = 0;
static volatile uint32_t g_dropped  = 0;
static volatile bool     g_connected = false;
static DHTesp g_dht;
static Adafruit_SSD1306 g_oled(128, 64, &Wire, -1);   /* I2C OLED dashboard */
static volatile int32_t  g_last_temp_c100 = 2500;     /* last reading for the screen */
static volatile int32_t  g_last_hum_c100  = 5000;

/* ---- sensor read: real wokwi-dht22, synthetic fallback on error ---------- */
static bool read_sensor(float *temp_c, float *humidity)
{
    TempAndHumidity d = g_dht.getTempAndHumidity();
    if (g_dht.getStatus() != DHTesp::ERROR_NONE ||
        isnan(d.temperature) || isnan(d.humidity)) {
        /* sensor glitch: synthesize so the pipeline keeps flowing, flag it */
        float phase = millis() / 1000.0f;
        *temp_c   = 25.0f + 3.0f * sinf(phase * 0.20f);
        *humidity = 50.0f + 6.0f * sinf(phase * 0.13f);
        return false;
    }
    *temp_c   = d.temperature;
    *humidity = d.humidity;
    return true;
}

/* ---- producer: never blocks on the network ---- */
static void sensor_task(void *arg)
{
    (void)arg;
    uint32_t seq = 0;
    for (;;) {
        float t = 0.0f, h = 0.0f;
        bool ok = read_sensor(&t, &h);
        if (!ok || isnan(t) || isnan(h)) { t = 25.0f; h = 50.0f; ok = false; }

        sensor_reading_t r;
        r.seq          = ++seq;
        r.timestamp_ms = millis();
        r.temperature  = (int32_t)lroundf(t * 100.0f);
        r.humidity     = (int32_t)lroundf(h * 100.0f);
        r.simulated    = !ok;
        g_last_temp_c100 = r.temperature;
        g_last_hum_c100  = r.humidity;

        if (xQueueSend(g_queue, &r, 0) == pdTRUE) {
            g_produced++;
        } else {
            g_dropped++;   /* link down long enough to fill the queue: drop newest */
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

/* ---- network helpers ---- */
static bool wifi_ensure(void)
{
    if (WiFi.status() == WL_CONNECTED) return true;
    Serial.printf("[net] Wi-Fi connecting to %s ...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    bool ok = (WiFi.status() == WL_CONNECTED);
    if (ok) Serial.printf("[net] Wi-Fi up, IP %s\n", WiFi.localIP().toString().c_str());
    else    Serial.println("[net] Wi-Fi connect timeout");
    return ok;
}

/* Read exactly len bytes (TCP is a byte stream; a read can return a partial). */
static bool read_exact(WiFiClient &c, uint8_t *buf, size_t len, uint32_t deadline_ms)
{
    size_t got = 0;
    while (got < len) {
        if ((int32_t)(millis() - deadline_ms) >= 0) return false;
        if (!c.connected() && c.available() == 0)   return false;
        int n = c.read(buf + got, len - got);
        if (n > 0) got += (size_t)n;
        else { esp_task_wdt_reset(); vTaskDelay(pdMS_TO_TICKS(5)); }
    }
    return true;
}

/* Send HELLO, expect HELLO_ACK within 2s (the spec's handshake deadline). */
static bool do_handshake(WiFiClient &c)
{
    uint8_t pkt[PL_HELLO_PACKET_SIZE];
    size_t n = pl_build_hello(pkt, sizeof pkt, DEVICE_ID, FW_VERSION, CAPABILITIES);
    if ((size_t)c.write(pkt, n) != n) return false;

    uint32_t deadline = millis() + 2000;
    uint8_t hdr[PL_FW_HDR_SIZE];
    if (!read_exact(c, hdr, sizeof hdr, deadline)) return false;

    pl_rx_header_t rh;
    if (!pl_parse_header_fw(hdr, &rh) || rh.payload_len > 64) return false;
    uint8_t pl[64];
    if (rh.payload_len && !read_exact(c, pl, rh.payload_len, deadline)) return false;
    if (pl_checksum(pl, rh.payload_len) != rh.checksum) return false;
    if (rh.type != PL_TYPE_HELLO_ACK) return false;

    uint32_t session = (rh.payload_len >= 4) ? pl_rd_u32(pl) : 0;
    Serial.printf("[net] HELLO_ACK session=%u\n", session);
    return true;
}

/* Consume any queued ACK/ERROR replies without blocking. */
static void drain_replies(WiFiClient &c)
{
    while (c.available() >= PL_FW_HDR_SIZE) {
        uint8_t hdr[PL_FW_HDR_SIZE];
        if (!read_exact(c, hdr, sizeof hdr, millis() + 200)) return;
        pl_rx_header_t rh;
        if (!pl_parse_header_fw(hdr, &rh) || rh.payload_len > 64) return;
        uint8_t pl[64];
        if (rh.payload_len && !read_exact(c, pl, rh.payload_len, millis() + 200)) return;
        if (pl_checksum(pl, rh.payload_len) != rh.checksum) continue;

        if (rh.type == PL_TYPE_ACK)
            Serial.printf("[net] <- ACK seq=%u\n", pl_rd_u32(pl));
        else if (rh.type == PL_TYPE_ERROR)
            Serial.printf("[net] <- ERROR code=%u\n", pl_rd_u16(pl));
    }
}

static void backoff_wait(uint32_t *ms)
{
    Serial.printf("[net] backoff %lums\n", (unsigned long)*ms);
    uint32_t waited = 0;
    while (waited < *ms) {           /* feed the watchdog through long waits */
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(500));
        waited += 500;
    }
    *ms = (*ms * 2 > BACKOFF_MAX_MS) ? BACKOFF_MAX_MS : *ms * 2;
}

/* ---- consumer / uplink (the M4 reporter, now a real TCP sender) ---- */
static void net_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);          /* this task must check in or the chip resets */
    WiFiClient tcp;
    uint32_t backoff = BACKOFF_MIN_MS;

    for (;;) {
        esp_task_wdt_reset();

        if (!wifi_ensure()) { g_connected = false; backoff_wait(&backoff); continue; }

        if (!tcp.connected()) {
            Serial.printf("[net] TCP connect %s:%u\n", SERVER_HOST, SERVER_TCP_PORT);
            esp_task_wdt_reset();                 /* pet WDT before a blocking call */
            if (!tcp.connect(SERVER_HOST, SERVER_TCP_PORT, 4000)) {  /* 4s < 10s WDT */
                g_connected = false; backoff_wait(&backoff); continue;
            }
            if (!do_handshake(tcp)) {
                Serial.println("[net] handshake failed -> reconnect");
                tcp.stop(); g_connected = false; backoff_wait(&backoff); continue;
            }
            backoff = BACKOFF_MIN_MS;     /* reset backoff on a good connection */
            g_connected = true;
            Serial.println("[net] STREAMING");
        }

        /* block up to 1s for a sample, so we still feed the WDT + poll replies
         * during idle periods */
        sensor_reading_t r;
        if (xQueueReceive(g_queue, &r, pdMS_TO_TICKS(1000)) == pdTRUE) {
            uint8_t pkt[PL_SENSOR_PACKET_SIZE];
            size_t n = pl_build_sensor(pkt, sizeof pkt, DEVICE_ID,
                                       r.seq, r.timestamp_ms,
                                       r.temperature, r.humidity);
            if ((size_t)tcp.write(pkt, n) != n) {
                Serial.println("[net] send failed -> reconnect");
                tcp.stop(); g_connected = false; backoff_wait(&backoff); continue;
            }
            g_consumed++;
        }
        drain_replies(tcp);
    }
}

/* ---- UDP heartbeat: fire-and-forget liveness ---- */
static void heartbeat_task(void *arg)
{
    (void)arg;
    WiFiUDP udp;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (WiFi.status() != WL_CONNECTED) continue;
        uint8_t pkt[PL_HEARTBEAT_PACKET_SIZE];
        size_t n = pl_build_heartbeat(pkt, sizeof pkt, DEVICE_ID,
                                      (uint32_t)millis(), (uint32_t)ESP.getFreeHeap());
        udp.beginPacket(SERVER_HOST, SERVER_UDP_PORT);
        udp.write(pkt, n);
        udp.endPacket();
    }
}

/* ---- periodic stats ---- */
static void stats_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        Serial.printf("[stats] %s  produced=%u sent=%u dropped=%u  queue=%u/%d  heap=%uB\n",
                      g_connected ? "online" : "offline",
                      g_produced, g_consumed, g_dropped,
                      (unsigned)uxQueueMessagesWaiting(g_queue), QUEUE_LEN,
                      (unsigned)ESP.getFreeHeap());
    }
}

/* ---- OLED status dashboard (I2C SSD1306 on GPIO21/22) --------------------
 * Live readout: connection state, packet counts, last temp/humidity, free heap.
 * If the panel isn't found it logs and exits without stalling the firmware. */
static void display_task(void *arg)
{
    (void)arg;
    Wire.begin(21, 22);                          /* SDA=GPIO21, SCL=GPIO22 */
    if (!g_oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("[oled] not found (running without display)");
        vTaskDelete(NULL);
        return;
    }
    g_oled.setTextColor(SSD1306_WHITE);
    g_oled.setTextSize(1);
    for (;;) {
        int32_t t = g_last_temp_c100, h = g_last_hum_c100;
        int ti = (int)(t / 100), tf = (int)(t % 100); if (tf < 0) tf = -tf;
        int hi = (int)(h / 100), hf = (int)(h % 100); if (hf < 0) hf = -hf;
        g_oled.clearDisplay();
        g_oled.setCursor(0, 0);
        g_oled.println("PulseLink");
        g_oled.print("state: "); g_oled.println(g_connected ? "ONLINE" : "SEARCHING");
        g_oled.printf("sent:%u  drop:%u\n", (unsigned)g_consumed, (unsigned)g_dropped);
        g_oled.printf("temp: %d.%02d C\n", ti, tf);
        g_oled.printf("hum : %d.%02d %%\n", hi, hf);
        g_oled.printf("heap: %u\n", (unsigned)ESP.getFreeHeap());
        g_oled.display();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---- status LED: blink rate encodes the connection state ----------------
 * fast (~5 Hz) = offline / connecting,  slow (~1 Hz) = online / streaming.
 * Runs independently of Wi-Fi and the sensor so it always reflects state. */
static void led_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t half = g_connected ? 500 : 100;   /* ms on, ms off */
        digitalWrite(LED_PIN, HIGH);
        vTaskDelay(pdMS_TO_TICKS(half));
        digitalWrite(LED_PIN, LOW);
        vTaskDelay(pdMS_TO_TICKS(half));
    }
}

static void setup_watchdog(void)
{
    /* Arduino-ESP32 v3 auto-inits the TWDT; v2 needs an explicit init. */
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t cfg = {
        .timeout_ms     = WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&cfg);
#else
    esp_task_wdt_init(WDT_TIMEOUT_S, true);   /* true = panic+reset on timeout */
#endif
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    pinMode(LED_PIN, OUTPUT);
    g_dht.setup(DHT_PIN, DHTesp::DHT22);   /* wokwi-dht22 on GPIO15 */

    g_queue = xQueueCreate(QUEUE_LEN, sizeof(sensor_reading_t));
    if (g_queue == NULL) {
        Serial.println("FATAL: queue allocation failed");
        for (;;) delay(1000);
    }

    setup_watchdog();

    Serial.println();
    Serial.println("PulseLink firmware (M5): Wi-Fi + TCP uplink, reconnect, heartbeat, watchdog");

    /* Sampler on core 1; the (blocking) network work on core 0 so a stalled
     * send can never jitter the sample cadence. */
    xTaskCreatePinnedToCore(sensor_task,    "sensor",    4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(net_task,       "net",       8192, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(heartbeat_task, "heartbeat", 4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(stats_task,     "stats",     3072, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(led_task,       "led",       2048, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(display_task,   "display",   4096, NULL, 1, NULL, 1);
}

void loop()
{
    vTaskDelay(pdMS_TO_TICKS(1000));   /* all work happens in the tasks */
}
