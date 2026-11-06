#include "cardputer_modem.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <nvs.h>
#include "cardputer_settings.h"

// Minimal COM1 8250/16550 + Hayes modem. Keep this deliberately small:
// no PPP, no DNS cache, no terminal UI, no dynamic buffers.
//
// What a guest can do over COM1 (see the command table in parse_command):
//   AT / ATZ / ATI / ATE0 / ATE1 / ATH / AT&V
//   ATDT <host>[:port]        raw TCP (telnet, BBS, SMTP...)
//   AT+HTTPGET <host[:port]>/<path>   connect, send GET, stream the answer
//   AT+CIPCLOSE               hang up
//   AT+CIFSR                  local IP
//   AT+WIFI?                  link state + IP / SSID
//   AT+WIFI=<ssid>,<pass>     store credentials in NVS and reconnect
//   AT+WIFI=ON | AT+WIFI=OFF  enable / disable the radio
//   AT+WIFISCAN               list up to 8 access points (SSID, RSSI, chan)

#define COM1_BASE 0x3F8
#define UART_LSR_DR 0x01
#define UART_LSR_THRE 0x20
#define UART_LSR_TEMT 0x40
#define UART_MSR_CTS 0x10
#define UART_MSR_DSR 0x20
#define UART_MSR_RI  0x40
#define UART_MSR_DCD 0x80

#define MODEM_CONNECT_TIMEOUT_MS 6000
#define MODEM_PLUS_ESCAPE_MS 1000

static WiFiClient tcp_client;
static uint8_t ier = 0;
static uint8_t lcr = 0;
static uint8_t mcr = 0;
static uint8_t scr = 0;
static uint16_t divisor = 12; // 9600 bps compatible default

static bool echo_on = true;
static bool command_mode = true;
static bool wifi_started = false;
static bool tcp_read_only = false;   // HTTP mode: ignore what the guest types
static uint32_t last_wifi_attempt_ms = 0;
static uint32_t plus_last_ms = 0;
static uint8_t plus_count = 0;
static uint32_t connect_count = 0;
static uint32_t connect_fail_count = 0;

static char wifi_ssid[33] = {};
static char wifi_pass[65] = {};
static char cmd_line[128] = {};
static uint8_t cmd_len = 0;

static uint8_t rx_buf[256];
static volatile uint8_t rx_head = 0;
static volatile uint8_t rx_tail = 0;


static void start_wifi(void);

static bool rx_empty(void) {
    return rx_head == rx_tail;
}

static bool rx_full(void) {
    return (uint8_t)(rx_head + 1) == rx_tail;
}

static void rx_clear(void) {
    rx_head = rx_tail = 0;
}

static void rx_push(uint8_t value) {
    if (rx_full()) return;
    rx_buf[rx_head++] = value;
}

static int rx_pop(void) {
    if (rx_empty()) return -1;
    const uint8_t value = rx_buf[rx_tail++];
    return value;
}

static void queue_text(const char *text) {
    while (*text) rx_push((uint8_t)*text++);
}

static void result(const char *text) {
    queue_text("\r\n");
    queue_text(text);
    queue_text("\r\n");
}

static void load_wifi_settings(void) {
    nvs_handle_t handle;
    if (nvs_open("cardputer86", NVS_READONLY, &handle) != ESP_OK) return;
    size_t len = sizeof(wifi_ssid);
    nvs_get_str(handle, "wifi_ssid", wifi_ssid, &len);
    len = sizeof(wifi_pass);
    nvs_get_str(handle, "wifi_pass", wifi_pass, &len);
    nvs_close(handle);
}

bool cardputer_modem_save_wifi(const char *ssid, const char *pass) {
    if (!ssid || !pass || ssid[0] == '\0' ||
        strlen(ssid) >= sizeof(wifi_ssid) ||
        strlen(pass) >= sizeof(wifi_pass)) {
        return false;
    }
    nvs_handle_t handle;
    if (nvs_open("cardputer86", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result_ssid = nvs_set_str(handle, "wifi_ssid", ssid);
    esp_err_t result_pass = nvs_set_str(handle, "wifi_pass", pass);
    esp_err_t commit = (result_ssid == ESP_OK && result_pass == ESP_OK)
        ? nvs_commit(handle) : ESP_FAIL;
    nvs_close(handle);
    const bool ok = result_ssid == ESP_OK && result_pass == ESP_OK && commit == ESP_OK;
    if (ok) {
        strlcpy(wifi_ssid, ssid, sizeof(wifi_ssid));
        strlcpy(wifi_pass, pass, sizeof(wifi_pass));
        WiFi.disconnect(true);
        delay(20);
        start_wifi();
    }
    return ok;
}

static void start_wifi(void) {
    if (!cardputer_settings_com1_enabled()) return;
    if (wifi_ssid[0] == '\0') return;
    WiFi.mode(WIFI_STA);
    /* 省电（modem sleep）不是可选项，是共存条件：
       实测 WiFi 用主动模式时，射频 + lwIP 一共要 ~103 KB，且关联那一刻要一次性
       拿到 ~54 KB 连续块。而内部盘 FatFS 现在（正确地）先挂载，占了十几 KB 并让
       最大连续块从 73.7 KB 掉到 45 KB → 关联直接 `AUTH_FAIL`（status=4），
       同时射频任务把模拟器拖到 0.65 s/批（正常 ~3.5 ms）。DTOM 省电把 RX 缓冲
       压下来，既省堆又几乎不占 CPU。 */
    WiFi.setSleep(true);
    WiFi.begin(wifi_ssid, wifi_pass);
    wifi_started = true;
    last_wifi_attempt_ms = millis();
#ifdef use_lib_log_serial
    Serial.printf("[wifi] begin ssid='%s' heap=%lu max=%lu\n", wifi_ssid,
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getMaxAllocHeap());
#endif
}

static void hangup(void) {
    if (tcp_client.connected()) tcp_client.stop();
    command_mode = true;
    tcp_read_only = false;
    plus_count = 0;
}

static void flush_plus(void) {
    while (plus_count > 0) {
        if (tcp_client.connected()) tcp_client.write('+');
        plus_count--;
    }
}

/* "host", "host:port" -> host buffer + port (default kept when absent) */
static bool split_host_port(const char *target, char *host, size_t host_size,
                            uint16_t *port, uint16_t dflt) {
    const char *colon = strrchr(target, ':');
    const size_t total = strlen(target);
    if (total == 0) return false;
    if (!colon) {
        if (total >= host_size) return false;
        strlcpy(host, target, host_size);
        *port = dflt;
        return true;
    }
    {
        const size_t n = (size_t)(colon - target);
        const int p = atoi(colon + 1);
        if (n == 0 || n >= host_size) return false;
        memcpy(host, target, n);
        host[n] = '\0';
        *port = (p > 0 && p < 65536) ? (uint16_t)p : dflt;
    }
    return true;
}

static void wifi_report(void) {
    if (WiFi.status() == WL_CONNECTED) {
        result("WIFI CONNECTED");
        queue_text("SSID ");
        queue_text(wifi_ssid);
        queue_text(" IP ");
        queue_text(WiFi.localIP().toString().c_str());
        queue_text(" RSSI ");
        queue_text(String((long)WiFi.RSSI()).c_str());
        queue_text(" dBm\r\n");
    } else if (wifi_ssid[0] != '\0') {
        result(wifi_started ? "WIFI CONFIGURED, NOT CONNECTED" : "WIFI CONFIGURED, RADIO OFF");
    } else {
        result("WIFI NOT CONFIGURED");
        result("USE AT+WIFI=<SSID>,<PASS>");
    }
}

static void wifi_scan(void) {
    const int16_t found = WiFi.scanNetworks();
    if (found <= 0) {
        result("NO NETWORK FOUND");
        result("OK");
        return;
    }
    {
        int16_t shown = 0;
        for (int16_t i = 0; i < found && shown < 8; i++) {
            char line[56];
            snprintf(line, sizeof(line), "%-20.20s %4d dBm ch%02d %s",
                     WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i), (int)WiFi.channel(i),
                     WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "key");
            result(line);
            shown++;
        }
    }
    WiFi.scanDelete();
    result("OK");
}

static void parse_wifi_command(const char *args) {
    /* AT+WIFI=<ssid>,<pass> means "I want this machine on the network", so it also
       switches COM1 on. That is not cosmetic: start_wifi() is gated on the COM1
       setting, and the radio MUST come up during modem_init() — i.e. before
       CreateRAM() grabs the guest page cache — otherwise esp_wifi_init() fails
       with 257 (ESP_ERR_NO_MEM) because the heap is already fragmented into 4 KB
       cache pages (measured: free=29704 max=9204 at the point of failure). */
    const size_t arg_len = strlen(args);
    const bool wifi_on = (arg_len == 3 && args[0] == '=' &&
                          (args[1] == 'O' || args[1] == 'o') &&
                          (args[2] == 'N' || args[2] == 'n'));
    const bool wifi_off = (arg_len == 4 && args[0] == '=' &&
                           (args[1] == 'O' || args[1] == 'o') &&
                           (args[2] == 'F' || args[2] == 'f') &&
                           (args[3] == 'F' || args[3] == 'f'));
    if (arg_len > 1 && args[0] == '=' && !wifi_on && !wifi_off &&
        !cardputer_settings_com1_enabled()) {
        cardputer_settings_set_com1_enabled(true);
    }
    if (!cardputer_settings_com1_enabled()) {
        result("NETWORK DISABLED");
        return;
    }
    if (*args == '\0' || *args == '?') {
        wifi_report();
        result("OK");
        return;
    }
    if (*args != '=') {
        result("ERROR");
        return;
    }
    args++;
    if ((args[0] == 'O' || args[0] == 'o') &&
        (args[1] == 'N' || args[1] == 'n') && args[2] == '\0') {
        cardputer_modem_resume_wifi();
        result("OK");
        return;
    }
    if ((args[0] == 'O' || args[0] == 'o') &&
        (args[1] == 'F' || args[1] == 'f') &&
        (args[2] == 'F' || args[2] == 'f') && args[3] == '\0') {
        cardputer_modem_pause_wifi();
        result("OK");
        return;
    }
    {
        char ssid[33] = {};
        char pass[65] = {};
        const char *comma = strchr(args, ',');
        size_t ssid_len;
        if (!comma) {
            result("ERROR");
            return;
        }
        ssid_len = (size_t)(comma - args);
        if (ssid_len == 0 || ssid_len >= sizeof(ssid) ||
            strlen(comma + 1) >= sizeof(pass)) {
            result("ERROR");
            return;
        }
        memcpy(ssid, args, ssid_len);
        strlcpy(pass, comma + 1, sizeof(pass));
        result(cardputer_modem_save_wifi(ssid, pass) ? "OK" : "ERROR");
    }
}

static void dial_tcp(const char *target) {
    char host[64];
    uint16_t port = 23;

    if (!cardputer_settings_com1_enabled()) {
        result("NO CARRIER");
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        if (!wifi_started) start_wifi();
        result("NO CARRIER");
        return;
    }
    if (!split_host_port(target, host, sizeof(host), &port, 23)) {
        result("ERROR");
        return;
    }

    hangup();
    /* blocking connect: DNS + handshake, capped so a dead host cannot stall
     * the emulator for long (the guest freezes for up to this long). */
    if (tcp_client.connect(host, port, MODEM_CONNECT_TIMEOUT_MS)) {
        command_mode = false;
        tcp_read_only = false;
        connect_count++;
        result("CONNECT");
    } else {
        connect_fail_count++;
        result("NO CARRIER");
    }
}

/* AT+HTTPGET host[:port]/path -- one blocking connect, then the reply is
 * streamed to the guest as ordinary modem data (read-only, the guest's
 * keystrokes are ignored while it lasts).  Connection: close means the
 * server hangs up and the guest gets NO CARRIER at the end of the body. */
static void http_get(const char *target) {
    char host_spec[80];
    char host[64];
    char path[64];
    uint16_t port = 80;
    const char *slash;
    size_t host_len;

    if (!cardputer_settings_com1_enabled()) {
        result("NO CARRIER");
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        if (!wifi_started) start_wifi();
        result("NO CARRIER");
        return;
    }
    if (!target || *target == '\0') {
        result("ERROR");
        return;
    }
    slash = strchr(target, '/');
    host_len = slash ? (size_t)(slash - target) : strlen(target);
    if (host_len == 0 || host_len >= sizeof(host_spec)) {
        result("ERROR");
        return;
    }
    memcpy(host_spec, target, host_len);
    host_spec[host_len] = '\0';
    if (slash) strlcpy(path, slash, sizeof(path));
    else strlcpy(path, "/", sizeof(path));
    if (!split_host_port(host_spec, host, sizeof(host), &port, 80)) {
        result("ERROR");
        return;
    }

    hangup();
    if (!tcp_client.connect(host, port, MODEM_CONNECT_TIMEOUT_MS)) {
        connect_fail_count++;
        result("NO CARRIER");
        return;
    }
    connect_count++;
    tcp_client.print(String("GET ") + path + " HTTP/1.0\r\nHost: " + host +
                     "\r\nUser-Agent: CardPuter86\r\nConnection: close\r\n\r\n");
    command_mode = false;
    tcp_read_only = true;
    result("CONNECT");
}

static void at_and_v(void) {
    char line[56];
    snprintf(line, sizeof(line), "E%d Q0 V1 &C1 &D2", echo_on ? 1 : 0);
    result(line);
    snprintf(line, sizeof(line), "WIFI %s SSID %s",
             WiFi.status() == WL_CONNECTED ? "up" : "down",
             wifi_ssid[0] ? wifi_ssid : "-");
    result(line);
    snprintf(line, sizeof(line), "CONNECTS %lu FAILS %lu",
             (unsigned long)connect_count, (unsigned long)connect_fail_count);
    result(line);
    result("OK");
}

static bool starts_ci(const char *s, const char *prefix) {
    while (*prefix) {
        char a = *s++;
        char b = *prefix++;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return false;
    }
    return true;
}

static void parse_command(void) {
    char *cmd = cmd_line;
    char op;

    cmd_line[cmd_len] = '\0';
    while (*cmd == ' ') cmd++;
    if ((cmd[0] != 'A' && cmd[0] != 'a') || (cmd[1] != 'T' && cmd[1] != 't')) {
        result("ERROR");
        return;
    }
    cmd += 2;
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') {
        result("OK");
        return;
    }

    /* +WIFISCAN has to be tested before +WIFI, it starts with the same text */
    if (starts_ci(cmd, "+WIFISCAN")) {
        wifi_scan();
        return;
    }
    if (starts_ci(cmd, "+WIFI")) {
        parse_wifi_command(cmd + 5);
        return;
    }
    if (starts_ci(cmd, "+HTTPGET")) {
        const char *arg = cmd + 8;
        while (*arg == ' ' || *arg == '=') arg++;
        http_get(arg);
        return;
    }
    if (starts_ci(cmd, "+CIPCLOSE")) {
        hangup();
        result("OK");
        return;
    }
    if (starts_ci(cmd, "+CIFSR")) {
        result(WiFi.status() == WL_CONNECTED
               ? WiFi.localIP().toString().c_str() : "0.0.0.0");
        result("OK");
        return;
    }
    if (cmd[0] == '&' && (cmd[1] == 'V' || cmd[1] == 'v')) {
        at_and_v();
        return;
    }

    op = (cmd[0] >= 'a' && cmd[0] <= 'z') ? cmd[0] - 32 : cmd[0];
    switch (op) {
        case 'Z':
            echo_on = true;
            hangup();
            result("OK");
            return;
        case 'I':
            result("CardPuter86 Hayes WiFi Modem 0.5");
            result("ESP32-S3 TCP bridge, AT&V for status");
            result("OK");
            return;
        case 'E':
            echo_on = (cmd[1] != '0');
            result("OK");
            return;
        case 'H':
            hangup();
            result("OK");
            return;
        case 'D':
            cmd++;
            if (*cmd == 'T' || *cmd == 't' || *cmd == 'P' || *cmd == 'p') cmd++;
            while (*cmd == ' ') cmd++;
            dial_tcp(cmd);
            return;
        default:
            result("ERROR");
            return;
    }
}

static void command_byte(uint8_t value) {
    if (echo_on) rx_push(value);
    if (value == '\n') return;
    if (value == '\r') {
        parse_command();
        cmd_len = 0;
        return;
    }
    if (value == 8 || value == 127) {
        if (cmd_len > 0) cmd_len--;
        return;
    }
    if (cmd_len < sizeof(cmd_line) - 1) {
        cmd_line[cmd_len++] = (char)value;
    }
}

static void data_byte(uint8_t value) {
    if (value == '+') {
        plus_count++;
        if (plus_count >= 3) {
            command_mode = true;
            plus_count = 0;
            result("OK");
        }
        return;
    }
    while (plus_count > 0) {
        tcp_client.write('+');
        plus_count--;
    }
    if (tcp_client.connected()) {
        tcp_client.write(value);
    } else {
        command_mode = true;
        result("NO CARRIER");
    }
}

#ifdef use_lib_log_serial
// 仅调试固件：即使还没配 SSID 也把射频拉起来（start_wifi() 的前两步），
// 这样 guest 里跑 `AT+WIFISCAN` 就能扫到周围 AP —— 用来在不暴露家用 WiFi
// 密码的前提下证明「驱动 + 射频 + SPI/flash 共存」是通的。
bool cardputer_modem_debug_start_radio(void) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    Serial.printf("[wifi] radio forced on, heap=%lu max=%lu\n",
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getMaxAllocHeap());
    return true;
}

/* Debug console (0x01 'G'): one-line network report over USB serial.
   Needed because the guest-side path (WTERM + AT commands) depends on the
   serial->PS/2 keyboard injection being perfect, which is a separate problem. */
void cardputer_modem_debug_net_report(void) {
    Serial.printf("[net] status=%d connected=%d cfg=%d ssid='%s' ip=%s rssi=%d"
                  " com1=%d heap=%lu max=%lu\n",
                  (int)WiFi.status(),
                  (int)(WiFi.status() == WL_CONNECTED),
                  (int)(wifi_ssid[0] != '\0'),
                  wifi_ssid,
                  WiFi.status() == WL_CONNECTED
                      ? WiFi.localIP().toString().c_str() : "-",
                  WiFi.status() == WL_CONNECTED ? (int)WiFi.RSSI() : 0,
                  (int)cardputer_settings_com1_enabled(),
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getMaxAllocHeap());
}

/* Debug console (0x01 'H'): full end-to-end HTTP GET straight from the firmware,
   printing the first bytes to USB serial. Proves DNS + TCP + HTTP without needing
   a working guest keyboard. Blocking: up to MODEM_CONNECT_TIMEOUT_MS + a few s. */
void cardputer_modem_debug_http_get(const char *host, const char *path) {
    WiFiClient client;
    if (WiFi.status() != WL_CONNECTED) {
        if (!wifi_started) start_wifi();
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[http] no wifi (status=%d)\n", (int)WiFi.status());
        return;
    }
    Serial.printf("[http] GET %s%s heap=%lu max=%lu\n", host, path,
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getMaxAllocHeap());
    if (!client.connect(host, 80, MODEM_CONNECT_TIMEOUT_MS)) {
        Serial.printf("[http] connect failed\n");
        return;
    }
    client.printf("GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                  path, host);
    const uint32_t start = millis();
    unsigned int got = 0;
    while ((client.connected() || client.available()) &&
           millis() - start < 8000 && got < 400) {
        while (client.available() && got < 400) {
            const int c = client.read();
            if (c < 0) break;
            Serial.write((uint8_t)((c == '\r' || c == '\n') ? ' ' : c));
            got++;
        }
        delay(2);
    }
    client.stop();
    Serial.printf("\n[http] bytes=%u in %lums\n", got,
                  (unsigned long)(millis() - start));
}
#endif

void cardputer_modem_init(void) {
    ier = lcr = mcr = scr = 0;
    divisor = 12;
    cmd_len = 0;
    echo_on = true;
    command_mode = true;
    rx_clear();
    load_wifi_settings();
    if (wifi_ssid[0] != '\0') start_wifi();
}

void cardputer_modem_poll(void) {
    if (!cardputer_settings_com1_enabled()) {
        if (tcp_client.connected()) tcp_client.stop();
        return;
    }
    if (wifi_started && WiFi.status() != WL_CONNECTED &&
        millis() - last_wifi_attempt_ms > 15000) {
        last_wifi_attempt_ms = millis();
        WiFi.disconnect();
        WiFi.begin(wifi_ssid, wifi_pass);
    }
    if (!command_mode && tcp_client.connected()) {
        while (tcp_client.available() && !rx_full()) {
            rx_push((uint8_t)tcp_client.read());
        }
    } else if (!command_mode && !tcp_client.connected()) {
        command_mode = true;
        result("NO CARRIER");
    }
}

bool cardputer_modem_wifi_configured(void) {
    return wifi_ssid[0] != '\0';
}

bool cardputer_modem_wifi_connected(void) {
    return WiFi.status() == WL_CONNECTED;
}

const char *cardputer_modem_wifi_ssid(void) {
    return wifi_ssid;
}

void cardputer_modem_pause_wifi(void) {
    if (tcp_client.connected()) tcp_client.stop();
    command_mode = true;
    wifi_started = false;
    WiFi.disconnect(false, false);
}

void cardputer_modem_resume_wifi(void) {
    if (wifi_ssid[0] != '\0') start_wifi();
}

bool cardputer_modem_port(uint16_t port) {
    return port >= COM1_BASE && port <= COM1_BASE + 7;
}

void cardputer_modem_write(uint16_t port, uint8_t value) {
    const uint8_t reg = port - COM1_BASE;
    if ((lcr & 0x80) && reg <= 1) {
        if (reg == 0) divisor = (divisor & 0xFF00) | value;
        else divisor = (divisor & 0x00FF) | ((uint16_t)value << 8);
        return;
    }

    switch (reg) {
        case 0:
            if (command_mode) command_byte(value);
            else data_byte(value);
            break;
        case 1:
            ier = value;
            break;
        case 2:
            if (value & 0x02) rx_clear();
            break;
        case 3:
            lcr = value;
            break;
        case 4:
            mcr = value;
            break;
        case 7:
            scr = value;
            break;
        default:
            break;
    }
}

uint8_t cardputer_modem_read(uint16_t port) {
    const uint8_t reg = port - COM1_BASE;
    if (lcr & 0x80) {
        if (reg == 0) return divisor & 0xFF;
        if (reg == 1) return divisor >> 8;
    }

    switch (reg) {
        case 0: {
            const int value = rx_pop();
            return value < 0 ? 0 : (uint8_t)value;
        }
        case 1:
            return ier;
        case 2:
            return rx_empty() ? 0x01 : 0x04;
        case 3:
            return lcr;
        case 4:
            return mcr;
        case 5:
            return (rx_empty() ? 0 : UART_LSR_DR) | UART_LSR_THRE | UART_LSR_TEMT;
        case 6:
            return UART_MSR_CTS | UART_MSR_DSR |
                (tcp_client.connected() ? UART_MSR_DCD : 0);
        case 7:
            return scr;
        default:
            return 0xFF;
    }
}
