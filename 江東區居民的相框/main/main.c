/*
 * Build 1 — XIAO ESP32-C6 + Waveshare 3.6" Spectra 6 e-paper photo frame.
 *
 * Each boot: connect to saved WiFi, fetch the day's photo over HTTPS, push it
 * to the panel, deep sleep until just past JST midnight. The button (D0→GND)
 * wakes the chip and bumps a persistent offset, so each press shows the next
 * photo. With no saved credentials — or after a failed connect — the frame
 * becomes a SoftAP with a captive setup page instead.
 */

#include "epd.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <driver/gpio.h>

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_sleep.h>
#include <esp_mac.h>
#include <esp_http_client.h>
#include <esp_http_server.h>
#include <esp_crt_bundle.h>
#include <esp_attr.h>

#include <nvs_flash.h>
#include <nvs.h>

#include <esp_sntp.h>
#include <time.h>

#include <lwip/sockets.h>

#define IMAGE_URL             "https://www.hsuyuting.com/build/1/photo"
#define PIN_BUTTON            GPIO_NUM_0   /* XIAO D0 → tactile switch → GND */
#define WIFI_CONNECT_MS       30000
#define WIFI_MAX_RETRY        3
#define PROVISION_TIMEOUT_MS  (10 * 60 * 1000)
#define NVS_NS           "wifi"
#define AP_IP            "192.168.4.1"

static const char *TAG = "frame";

/* Survives deep sleep, cleared on cold boot. Set on connect failure so the
 * next boot goes straight to provisioning instead of looping on bad creds. */
static RTC_DATA_ATTR bool s_force_provisioning = false;

static uint8_t s_image_buf[EPD_BUFFER_SIZE];
static size_t  s_image_len;

static void    provision(void);   /* never returns */
static bool    sta_connect(const char *ssid, const char *pass);
static bool    sync_time(void);
static size_t  fetch(int32_t offset);
static void    sleep_until_midnight(bool have_time);
static void    deep_sleep(int64_t secs);
static bool    load_creds(char *ssid, size_t ssid_max, char *pass, size_t pass_max);
static bool    save_creds(const char *ssid, const char *pass);
static int32_t load_offset(void);
static void    save_offset(int32_t v);

void app_main(void)
{
    ESP_LOGI(TAG, "Build 1 booting");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));

    /* On button wake, show the next photo. Wait for release so the line is
     * back HIGH before wakeup is re-armed at sleep. */
    int32_t offset = load_offset();
    if (esp_sleep_get_wakeup_causes() & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        while (gpio_get_level(PIN_BUTTON) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        offset++;
        save_offset(offset);
        ESP_LOGI(TAG, "woke from button — offset now %d", (int)offset);
    } else {
        ESP_LOGI(TAG, "boot/timer wake, offset stays at %d", (int)offset);
    }

    char ssid[33] = {0}, pass[65] = {0};
    if (s_force_provisioning || !load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        s_force_provisioning = false;
        provision();
    }

    ESP_LOGI(TAG, "have saved creds, connecting to '%s'", ssid);
    if (!sta_connect(ssid, pass)) {
        ESP_LOGW(TAG, "connect failed; provisioning on next boot");
        esp_wifi_stop();
        const char *msg[] = { "WiFi failed", "Cannot connect", "Setup screen", "coming next..." };
        epd_show_message(msg, 4, EPD_WHITE, EPD_RED);
        s_force_provisioning = true;
        esp_restart();
    }

    bool have_time = sync_time();

    if (fetch(offset) == EPD_BUFFER_SIZE) {
        /* Cut WiFi before driving the panel: the C6's LDO can't feed the WiFi
         * PA and the e-paper boost converter at once — the 3.3 V rail sags and
         * the panel never finishes its 0x04 power-on. */
        esp_wifi_stop();
        ESP_LOGI(TAG, "WiFi stopped; powering panel");

        epd_init();
        epd_display(s_image_buf, EPD_BUFFER_SIZE);
        epd_sleep();
        sleep_until_midnight(have_time);
    }

    ESP_LOGE(TAG, "fetch failed; retrying in 1h");
    deep_sleep(3600);
}

static httpd_handle_t s_httpd;
static volatile bool  s_provision_done;

static const char SETUP_HTML[] =
"<!DOCTYPE html><html lang='zh-Hant'><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>手工相框設定</title>"
"<style>"
"body{font-family:-apple-system,system-ui,'PingFang TC',sans-serif;max-width:380px;margin:1.5rem auto;padding:0 1rem;color:#222}"
"h1{font-size:1.25rem;margin-top:0}p{color:#555}"
"ol{color:#555;padding-left:1.3rem;line-height:1.7;margin:.5rem 0 1rem}"
"input,select,button{font-size:16px;padding:12px;width:100%;box-sizing:border-box;margin:.5rem 0;border:1px solid #ccc;border-radius:6px}"
"button{background:#0066ff;color:#fff;border:0;font-weight:600;cursor:pointer}"
".muted{color:#888;font-size:.85rem;line-height:1.6}"
"</style></head><body>"
"<h1>手工相框 WiFi 設定</h1>"
"<ol>"
"<li>在下面選擇家裡的 WiFi</li>"
"<li>輸入 WiFi 密碼</li>"
"<li>按「儲存並連線」</li>"
"</ol>"
"<form action='/save' method='POST'>"
"<select name='ssid' id='ssid'><option>正在掃描附近的 WiFi…</option></select>"
"<input type='password' name='pass' placeholder='WiFi 密碼' required>"
"<button type='submit'>儲存並連線</button>"
"</form>"
"<p class='muted'>沒看到你的 WiFi？請重新整理此頁面。儲存後相框會重新開機，約一分鐘後開始顯示照片。</p>"
"<script>"
"fetch('/scan').then(r=>r.json()).then(nets=>{"
"const s=document.getElementById('ssid');s.innerHTML='';"
"if(!nets.length){s.innerHTML='<option>找不到 WiFi，請重新整理</option>';return}"
"nets.forEach(n=>{const o=document.createElement('option');o.value=n.s;o.textContent=n.s+' ('+n.r+' dBm)';s.appendChild(o)});"
"}).catch(()=>{document.getElementById('ssid').innerHTML='<option>掃描失敗，請重新整理</option>'});"
"</script></body></html>";

static esp_err_t h_setup(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, SETUP_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    wifi_scan_config_t scan = {0};
    esp_wifi_scan_start(&scan, true);

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;

    wifi_ap_record_t *records = calloc(n, sizeof(wifi_ap_record_t));
    if (!records) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    esp_wifi_scan_get_ap_records(&n, records);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    bool first = true;
    for (int i = 0; i < n; i++) {
        if (records[i].ssid[0]) {   /* hidden networks scan as empty SSIDs */
            char chunk[160];
            snprintf(chunk, sizeof(chunk), "%s{\"s\":\"%.32s\",\"r\":%d}",
                     first ? "" : ",", (char *)records[i].ssid, records[i].rssi);
            httpd_resp_sendstr_chunk(req, chunk);
            first = false;
        }
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    free(records);
    return ESP_OK;
}

/* URL-decode in place — handles + and %XX */
static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; ) {
        if (*p == '+') { *o++ = ' '; p++; }
        else if (*p == '%' && p[1] && p[2]) {
            char h[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            p += 3;
        } else {
            *o++ = *p++;
        }
    }
    *o = '\0';
}

static const char SAVED_HTML[] =
"<!DOCTYPE html><html lang='zh-Hant'><head><meta charset='utf-8'>"
"<title>已儲存</title>"
"<style>body{font-family:-apple-system,system-ui,'PingFang TC',sans-serif;max-width:380px;margin:2rem auto;padding:0 1rem;text-align:center;color:#222}p{color:#555}</style>"
"</head><body><h1>已儲存！</h1>"
"<p>相框正在重新開機，連線到你家的 WiFi。</p>"
"<p>約一分鐘後就會顯示照片，可以關閉此頁面了。</p>"
"</body></html>";

static esp_err_t h_save(httpd_req_t *req)
{
    char body[256] = {0};
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }

    char ssid[64] = {0}, pass[64] = {0};
    if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK ||
        httpd_query_key_value(body, "pass", pass, sizeof(pass)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing fields");
        return ESP_FAIL;
    }
    url_decode(ssid);
    url_decode(pass);

    if (!ssid[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty ssid");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "saving creds for SSID: %s", ssid);
    if (!save_creds(ssid, pass)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, SAVED_HTML, HTTPD_RESP_USE_STRLEN);
    s_provision_done = true;
    return ESP_OK;
}

/* Every other GET → redirect to our portal IP. This combined with the
 * DNS hijack below is what triggers the OS's captive-portal popup. */
static esp_err_t h_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_IP "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* Tiny captive DNS — answers every A query with our AP IP. */
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(53),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[256];
    while (1) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
        if (n < 12) continue;

        /* Header: set QR=1 (response), RA=1, ANCOUNT=1, leave question intact */
        buf[2] = 0x81;
        buf[3] = 0x80;
        buf[6] = 0; buf[7] = 1;

        /* Walk the question name to find the end of QNAME + QTYPE + QCLASS */
        int q = 12;
        while (q < n && buf[q] != 0) q += buf[q] + 1;
        q += 5;  /* 1 byte null + 2 QTYPE + 2 QCLASS */
        if (q + 16 > (int)sizeof(buf)) continue;

        /* Answer RR: name pointer to QNAME, TYPE A, CLASS IN, TTL 60,
         * RDLEN 4, RDATA = AP_IP */
        uint8_t *p = buf + q;
        p[0]  = 0xC0; p[1]  = 0x0C;
        p[2]  = 0x00; p[3]  = 0x01;
        p[4]  = 0x00; p[5]  = 0x01;
        p[6]  = 0x00; p[7]  = 0x00; p[8] = 0x00; p[9] = 0x3C;
        p[10] = 0x00; p[11] = 0x04;
        p[12] = 192;  p[13] = 168;  p[14] = 4;   p[15] = 1;

        sendto(sock, buf, q + 16, 0, (struct sockaddr *)&src, slen);
    }
}

static void provision(void)
{
    ESP_LOGI(TAG, "entering provisioning mode");

    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Unique SSID from our MAC so multiple frames don't collide */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[32];
    int slen = snprintf(ssid, sizeof(ssid), "build-1-%02X%02X", mac[4], mac[5]);

    /* Tell the recipient what to do, on the panel itself — they can't see the
     * serial log. The IP is the fallback for phones that don't auto-open the
     * captive portal. Shown before the radio powers on, so the refresh isn't
     * fighting the WiFi PA for current. */
    const char *msg[] = { "WiFi Setup", "Join WiFi:", ssid, "Then open:", "192.168.4.1" };
    epd_show_message(msg, 5, EPD_BLACK, EPD_WHITE);

    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, ssid, slen);
    ap.ap.ssid_len = slen;
    ap.ap.channel = 1;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;

    /* APSTA so we can also scan nearby networks for the dropdown */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP up: %s, http://%s", ssid, AP_IP);

    xTaskCreate(dns_task, "dns", 4096, NULL, 5, NULL);

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.uri_match_fn = httpd_uri_match_wildcard;
    hcfg.max_uri_handlers = 8;
    ESP_ERROR_CHECK(httpd_start(&s_httpd, &hcfg));

    httpd_uri_t u_setup    = { .uri = "/",      .method = HTTP_GET,  .handler = h_setup };
    httpd_uri_t u_scan     = { .uri = "/scan",  .method = HTTP_GET,  .handler = h_scan };
    httpd_uri_t u_save     = { .uri = "/save",  .method = HTTP_POST, .handler = h_save };
    httpd_uri_t u_redirect = { .uri = "/*",     .method = HTTP_GET,  .handler = h_redirect };
    httpd_register_uri_handler(s_httpd, &u_setup);
    httpd_register_uri_handler(s_httpd, &u_scan);
    httpd_register_uri_handler(s_httpd, &u_save);
    httpd_register_uri_handler(s_httpd, &u_redirect);

    /* No one configuring within the timeout → sleep instead of draining the
     * battery with the SoftAP. Saved creds are untouched, so the next wake
     * (button or timer) retries WiFi normally; with no creds, the button
     * brings this screen back. The panel keeps showing the setup text. */
    for (int ms = 0; !s_provision_done; ms += 500) {
        if (ms >= PROVISION_TIMEOUT_MS) {
            ESP_LOGI(TAG, "no setup after %d min; sleeping", PROVISION_TIMEOUT_MS / 60000);
            esp_wifi_stop();
            deep_sleep(24 * 3600);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI(TAG, "credentials saved; rebooting in 2s");
    vTaskDelay(pdMS_TO_TICKS(2000));  /* let the HTTP response reach the phone */
    esp_restart();
}

static EventGroupHandle_t s_sta_eg;
#define STA_CONNECTED  BIT0
#define STA_FAILED     BIT1
static int s_sta_retry;

static void sta_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_sta_retry++ < WIFI_MAX_RETRY) {
            ESP_LOGW(TAG, "WiFi disconnect, retry %d/%d", s_sta_retry, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_sta_eg, STA_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_sta_retry = 0;
        xEventGroupSetBits(s_sta_eg, STA_CONNECTED);
    }
}

static bool sta_connect(const char *ssid, const char *pass)
{
    s_sta_eg = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, sta_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, sta_event, NULL, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid,     ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    /* Accept any auth mode the AP advertises */
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_sta_eg,
        STA_CONNECTED | STA_FAILED,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_MS));

    return (bits & STA_CONNECTED) != 0;
}

/* Pull time from NTP and pin the C runtime to JST — for readable logs and to
 * compute next-JST-midnight for the long sleep. */
static bool sync_time(void)
{
    ESP_LOGI(TAG, "syncing time via NTP...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    time_t now = 0;
    for (int i = 0; i < 30; i++) {
        now = time(NULL);
        if (now > 1700000000) break;  /* somewhere past 2023-11 */
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    esp_sntp_stop();

    setenv("TZ", "JST-9", 1);  /* POSIX: STD<offset west of UTC>, so -9 = UTC+9 */
    tzset();

    if (now <= 1700000000) {
        ESP_LOGW(TAG, "NTP sync timed out; will fall back to 24h sleep");
        return false;
    }

    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm_now);
    ESP_LOGI(TAG, "current JST time: %s", buf);
    return true;
}

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (s_image_len + evt->data_len > EPD_BUFFER_SIZE) {
            ESP_LOGE(TAG, "image too large: %u + %d > %d",
                     (unsigned)s_image_len, evt->data_len, EPD_BUFFER_SIZE);
            return ESP_FAIL;
        }
        memcpy(s_image_buf + s_image_len, evt->data, evt->data_len);
        s_image_len += evt->data_len;
    }
    return ESP_OK;
}

static size_t fetch(int32_t offset)
{
    s_image_len = 0;
    char url[160];
    /* ?offset=N → today's photo, N button presses forward */
    snprintf(url, sizeof(url), IMAGE_URL "?offset=%d", (int)offset);
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP transport error: %s", esp_err_to_name(err));
        return 0;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d", status);
        return 0;
    }
    ESP_LOGI(TAG, "fetched %u bytes", (unsigned)s_image_len);
    return s_image_len;
}

/* Sleep until the next JST 00:00:30 — 30s past midnight so the server's day
 * index has already rolled over. Falls back to 24h if NTP wasn't synced. */
static void sleep_until_midnight(bool have_time)
{
    if (!have_time) {
        deep_sleep(24 * 3600);
    }

    time_t now = time(NULL);
    struct tm tm_target;
    localtime_r(&now, &tm_target);
    tm_target.tm_mday += 1;
    tm_target.tm_hour = 0;
    tm_target.tm_min = 0;
    tm_target.tm_sec = 30;
    tm_target.tm_isdst = -1;
    time_t target = mktime(&tm_target);

    int64_t secs = (int64_t)difftime(target, now);
    if (secs < 60 || secs > 25 * 3600) {
        ESP_LOGW(TAG, "computed sleep %lld s out of range, falling back to 24h", (long long)secs);
        deep_sleep(24 * 3600);
    }

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm_target);
    ESP_LOGI(TAG, "deep sleeping until next JST midnight: %s (%lld s away)",
             buf, (long long)secs);
    deep_sleep(secs);
}

static void deep_sleep(int64_t secs)
{
    /* Pull-up holds D0 HIGH; pressing the button shorts it to GND and wakes us */
    esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(1ULL << PIN_BUTTON, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
    esp_deep_sleep_start();
}

static bool load_creds(char *ssid, size_t ssid_max, char *pass, size_t pass_max)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t s = ssid_max, p = pass_max;
    bool ok = nvs_get_str(h, "ssid", ssid, &s) == ESP_OK
           && nvs_get_str(h, "pass", pass, &p) == ESP_OK
           && ssid[0] != '\0';
    nvs_close(h);
    return ok;
}

static bool save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

static int32_t load_offset(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    int32_t v = 0;
    nvs_get_i32(h, "offset", &v);
    nvs_close(h);
    return v;
}

static void save_offset(int32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "offset", v);
    nvs_commit(h);
    nvs_close(h);
}
