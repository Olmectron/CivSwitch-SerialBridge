/*
 * ESP32 serial bridge for CivSwitch
 *
 * Creates an air-gapped Wi-Fi AP and relays payloads from your PC
 * to the Switch over USB serial. Your PC stays on its normal network.
 *
 * The Switch connects to this AP. Your PC talks to the ESP32 over
 * USB serial. The ESP32 opens a TCP connection to the Switch on its
 * own network and forwards the payload + response.
 *
 * Serial protocol:
 *   PC sends:     SEND <ip> <port> <length>\n<payload bytes>
 *   PC sends:     FTP <ip> <port> <length> <remote_path>\n<file bytes>
 *   ESP32 replies: console response, then \nEND\n
 *   Other commands: STATUS, SCAN
 *
 * Works with any ESP32 board (WROOM-32, S2, S3, C3, etc).
 * Compile with Arduino ESP32 board package v3.x.
 */

#include <WiFi.h>
#include <WiFiClient.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>

// ── Config (change these if you want) ───────────────────────────────
const char* AP_SSID     = "Switch2Bridge";
const int   AP_CHANNEL  = 6;
const int   MAX_CLIENTS = 4;

IPAddress apIP(192, 168, 4, 1);
IPAddress gateway(192, 168, 4, 1);
IPAddress subnet(255, 255, 255, 0);

// Built-in LED pin. GPIO 2 on most boards.
// Set to -1 if your board doesn't have one or you don't care.
const int LED_PIN = 2;

// ── FTP relay config ────────────────────────────────────────────────
// Credentials used when uploading to the console's FTP server.
// Most Switch homebrew FTP servers (sys-ftpd, ftpd) accept anonymous
// login. Change these if your server requires real credentials.
const char* FTP_USER = "anonymous";
const char* FTP_PASS = "anonymous@";

// ── DNS + HTTP (makes the Switch accept the network) ────────────────
DNSServer dnsServer;
WebServer httpServer(80);

// ── Serial relay ────────────────────────────────────────────────────
String serialBuf = "";

// ── LED state ───────────────────────────────────────────────────────
int lastClientCount = -1;
unsigned long lastBlinkTime = 0;
int blinkPhase = 0;

// Nintendo connectivity-check handler.
// The Switch probes specific hostnames after joining a network. If the
// responses match what Nintendo's servers return, it considers itself
// online and skips the captive-portal popup entirely.
//
// Adapted from 90DNS (GPL-2.0-or-later) via
// fernando-granco/switch2-homebrew (MIT).
void handleHttp() {
  String host = httpServer.hostHeader();

  // Legacy connectivity test (conntest.nintendowifi.net)
  if (host == "conntest.nintendowifi.net") {
    httpServer.sendHeader("Cache-Control", "no-store");
    httpServer.sendHeader("X-Organization", "Nintendo");
    httpServer.send(200, "text/html",
      "<html><head><title>HTML Page</title></head>"
      "<body>This is test.html page</body></html>");
    return;
  }

  // Modern connectivity tests (ctest / api endpoints)
  if (host == "ctest.cdn.nintendo.net" ||
      host == "ctest.p01.ctest.srv.nintendo.net" ||
      host == "ctest-dl-lp1.cdn.nintendo.net" ||
      host == "ctest-ul-lp1.cdn.nintendo.net" ||
      host == "api.p01.lp1.ctest.srv.nintendo.net" ||
      host == "api-ipv6.p01.lp1.ctest.srv.nintendo.net") {
    httpServer.sendHeader("Cache-Control", "no-store");
    httpServer.sendHeader("X-Organization", "Nintendo");
    httpServer.send(200, "text/plain", "ok");
    return;
  }

  // Everything else — no internet forwarding
  httpServer.send(404, "text/plain", "");
}

void setup() {
  Serial.begin(115200);
  Serial.setRxBufferSize(65536);

  if (LED_PIN >= 0) {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, gateway, subnet);

  bool ok = WiFi.softAP(AP_SSID, NULL, AP_CHANNEL, false, MAX_CLIENTS);
  if (!ok) {
    Serial.println("[ERROR] Failed to start AP");
    while (true) {
      if (LED_PIN >= 0) digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      delay(100);
    }
  }

  esp_wifi_set_ps(WIFI_PS_NONE);

  // DNS catches all lookups so the Switch's connectivity check
  // resolves to us instead of timing out.
  dnsServer.start(53, "*", apIP);

  // Emulate Nintendo connectivity-check responses so the Switch
  // believes it's online and never shows a captive-portal popup.
  // Adapted from 90DNS (GPL-2.0-or-later) via fernando-granco/switch2-homebrew.
  httpServer.onNotFound(handleHttp);
  httpServer.begin();

  Serial.println();
  Serial.println("========================================");
  Serial.println("  CivSwitch Serial Bridge");
  Serial.printf("  SSID:     %s\n", AP_SSID);
  Serial.println("  Auth:     Open (no password)");
  Serial.printf("  AP IP:    %s\n", WiFi.softAPIP().toString().c_str());
  Serial.println("========================================");
  Serial.println();
  Serial.println("Connect the Switch to this AP (no popup).");
  Serial.println("Send payloads from your PC with send_serial.py.");
  Serial.println();
}

void relayPayload(const char* ip, int port, const uint8_t* payload, int len) {
  Serial.printf("[RELAY] Connecting to %s:%d (%d bytes)...\n", ip, port, len);

  WiFiClient client;
  if (!client.connect(ip, port, 8000)) {
    Serial.println("[RELAY] ERROR: connection failed");
    Serial.println("\nEND");
    return;
  }

  client.write(payload, len);
  client.flush();

  // TCP half-close — tells the loader "payload complete, run it"
  int fd = client.fd();
  if (fd >= 0) {
    ::shutdown(fd, SHUT_WR);
  }

  Serial.println("[RELAY] Sent. Reading response...");
  unsigned long deadline = millis() + 120000;
  bool gotData = false;

  while (client.connected() && millis() < deadline) {
    while (client.available()) {
      char c = client.read();
      Serial.write(c);
      gotData = true;
      deadline = millis() + 5000;
    }
    delay(10);
  }

  client.stop();
  if (!gotData) Serial.println("[RELAY] (no response)");
  Serial.println("\nEND");
}

// Reads `payloadLen` bytes from the serial port into a freshly malloc'd
// buffer. On any error it prints the error + END and returns NULL, so the
// caller just checks for NULL and bails. Caller must free() on success.
uint8_t* readSerialPayload(int payloadLen) {
  if (payloadLen <= 0 || payloadLen > 262144) {
    Serial.println("[ERROR] Invalid payload length");
    Serial.println("\nEND");
    return NULL;
  }

  uint8_t* buf = (uint8_t*)malloc(payloadLen);
  if (!buf) {
    Serial.println("[ERROR] Out of memory");
    Serial.println("\nEND");
    return NULL;
  }

  int received = 0;
  unsigned long timeout = millis() + 10000;
  while (received < payloadLen && millis() < timeout) {
    if (Serial.available()) {
      buf[received++] = Serial.read();
      timeout = millis() + 5000;
    }
  }

  if (received < payloadLen) {
    Serial.printf("[ERROR] Only got %d of %d bytes\n", received, payloadLen);
    Serial.println("\nEND");
    free(buf);
    return NULL;
  }

  return buf;
}

// ── Minimal FTP client ──────────────────────────────────────────────
// Enough to upload one file to the console's FTP server (STOR in binary
// mode over PASV). The console runs the FTP *server*; the ESP32 is the
// client, on its own Wi-Fi network.

void ftpSendCmd(WiFiClient& ctrl, const String& cmd) {
  Serial.print("[FTP>] ");
  Serial.println(cmd);
  ctrl.print(cmd);
  ctrl.print("\r\n");
  ctrl.flush();
}

// Reads one complete FTP control reply (handling multi-line replies),
// echoes each line over serial, stores the final line in `lastLine`,
// and returns the 3-digit status code (or -1 on timeout).
int ftpReadResponse(WiFiClient& ctrl, unsigned long timeoutMs, String& lastLine) {
  unsigned long deadline = millis() + timeoutMs;
  String line = "";
  bool multiline = false;
  String multiCode = "";

  while (millis() < deadline) {
    while (ctrl.available()) {
      char c = ctrl.read();
      deadline = millis() + timeoutMs;
      if (c == '\n') {
        line.trim();
        Serial.print("[FTP<] ");
        Serial.println(line);
        bool hasCode = line.length() >= 4 &&
                       line[0] >= '0' && line[0] <= '9' &&
                       line[1] >= '0' && line[1] <= '9' &&
                       line[2] >= '0' && line[2] <= '9';
        if (hasCode) {
          String codeStr = line.substring(0, 3);
          char sep = line.charAt(3);
          if (!multiline) {
            if (sep == '-') {            // start of multi-line reply
              multiline = true;
              multiCode = codeStr;
            } else {                     // single-line reply: done
              lastLine = line;
              return codeStr.toInt();
            }
          } else if (codeStr == multiCode && sep == ' ') {
            lastLine = line;             // terminating line of multi-line reply
            return codeStr.toInt();
          }
        }
        line = "";
      } else if (c != '\r') {
        line += c;
      }
    }
    delay(5);
  }
  return -1;
}

void relayFtp(const char* ip, int port, const char* remotePath,
              const uint8_t* payload, int len) {
  Serial.printf("[FTP] Connecting to %s:%d -> %s (%d bytes)...\n",
                ip, port, remotePath, len);

  WiFiClient ctrl;
  if (!ctrl.connect(ip, port, 8000)) {
    Serial.println("[FTP] ERROR: control connection failed");
    Serial.println("\nEND");
    return;
  }

  String resp;
  int code;

  code = ftpReadResponse(ctrl, 8000, resp);                 // 220 greeting
  if (code != 220) { Serial.println("[FTP] ERROR: no greeting"); goto fail; }

  ftpSendCmd(ctrl, String("USER ") + FTP_USER);
  code = ftpReadResponse(ctrl, 8000, resp);                 // 331 or 230
  if (code == 331) {
    ftpSendCmd(ctrl, String("PASS ") + FTP_PASS);
    code = ftpReadResponse(ctrl, 8000, resp);               // 230
  }
  if (code != 230) { Serial.println("[FTP] ERROR: login failed"); goto fail; }

  ftpSendCmd(ctrl, "TYPE I");                               // binary mode
  code = ftpReadResponse(ctrl, 8000, resp);                 // 200
  if (code != 200) { Serial.println("[FTP] ERROR: TYPE I rejected"); goto fail; }

  ftpSendCmd(ctrl, "PASV");                                 // passive mode
  code = ftpReadResponse(ctrl, 8000, resp);                 // 227 (h1,h2,h3,h4,p1,p2)
  if (code != 227) { Serial.println("[FTP] ERROR: PASV rejected"); goto fail; }

  {
    int open = resp.indexOf('(');
    int close = resp.indexOf(')', open);
    if (open < 0 || close < 0) {
      Serial.println("[FTP] ERROR: can't parse PASV reply");
      goto fail;
    }
    String nums = resp.substring(open + 1, close);
    int vals[6];
    int n = 0, start = 0;
    for (int i = 0; i <= nums.length() && n < 6; i++) {
      if (i == nums.length() || nums[i] == ',') {
        vals[n++] = nums.substring(start, i).toInt();
        start = i + 1;
      }
    }
    if (n != 6) {
      Serial.println("[FTP] ERROR: bad PASV reply");
      goto fail;
    }
    String dataIp = String(vals[0]) + "." + vals[1] + "." + vals[2] + "." + vals[3];
    int dataPort = vals[4] * 256 + vals[5];
    Serial.printf("[FTP] Data channel %s:%d\n", dataIp.c_str(), dataPort);

    WiFiClient data;
    if (!data.connect(dataIp.c_str(), dataPort, 8000)) {
      Serial.println("[FTP] ERROR: data connection failed");
      goto fail;
    }

    ftpSendCmd(ctrl, String("STOR ") + remotePath);
    code = ftpReadResponse(ctrl, 8000, resp);               // 150 or 125
    if (code != 150 && code != 125) {
      Serial.println("[FTP] ERROR: STOR rejected");
      data.stop();
      goto fail;
    }

    data.write(payload, len);
    data.flush();
    data.stop();                                            // closing data -> 226

    code = ftpReadResponse(ctrl, 30000, resp);              // 226 transfer complete
    if (code == 226 || code == 250) {
      Serial.printf("[FTP] OK: uploaded %d bytes to %s\n", len, remotePath);
    } else {
      Serial.println("[FTP] ERROR: transfer not confirmed");
    }
  }

  ftpSendCmd(ctrl, "QUIT");
  ftpReadResponse(ctrl, 3000, resp);
  ctrl.stop();
  Serial.println("\nEND");
  return;

fail:
  ctrl.stop();
  Serial.println("\nEND");
}

void processSerialCommand(String& line) {
  line.trim();
  if (line.startsWith("SEND ")) {
    int i1 = 5;
    int i2 = line.indexOf(' ', i1);
    int i3 = line.indexOf(' ', i2 + 1);
    if (i2 < 0 || i3 < 0) {
      Serial.println("[ERROR] Usage: SEND <ip> <port> <length>");
      Serial.println("\nEND");
      return;
    }
    String ip = line.substring(i1, i2);
    int port = line.substring(i2 + 1, i3).toInt();
    int payloadLen = line.substring(i3 + 1).toInt();

    uint8_t* buf = readSerialPayload(payloadLen);
    if (!buf) return;

    relayPayload(ip.c_str(), port, buf, payloadLen);
    free(buf);

  } else if (line.startsWith("FTP ")) {
    // FTP <ip> <port> <length> <remote_path>
    // Uploads <length> bytes (sent right after this line) to <remote_path>
    // on the console's FTP server via STOR, binary mode, passive.
    int i1 = 4;
    int i2 = line.indexOf(' ', i1);
    int i3 = line.indexOf(' ', i2 + 1);
    int i4 = line.indexOf(' ', i3 + 1);
    if (i2 < 0 || i3 < 0 || i4 < 0) {
      Serial.println("[ERROR] Usage: FTP <ip> <port> <length> <remote_path>");
      Serial.println("\nEND");
      return;
    }
    String ip = line.substring(i1, i2);
    int port = line.substring(i2 + 1, i3).toInt();
    int payloadLen = line.substring(i3 + 1, i4).toInt();
    String remotePath = line.substring(i4 + 1);
    remotePath.trim();

    if (remotePath.length() == 0) {
      Serial.println("[ERROR] FTP needs a remote path");
      Serial.println("\nEND");
      return;
    }

    uint8_t* buf = readSerialPayload(payloadLen);
    if (!buf) return;

    relayFtp(ip.c_str(), port, remotePath.c_str(), buf, payloadLen);
    free(buf);

  } else if (line == "STATUS") {
    Serial.printf("[STATUS] AP clients: %d\n", WiFi.softAPgetStationNum());
    Serial.println("\nEND");

  } else if (line == "SCAN") {
    wifi_sta_list_t staList;
    if (esp_wifi_ap_get_sta_list(&staList) == ESP_OK) {
      Serial.printf("[SCAN] %d clients\n", staList.num);
      for (int i = 0; i < staList.num; i++) {
        uint8_t* m = staList.sta[i].mac;
        Serial.printf("  %02X:%02X:%02X:%02X:%02X:%02X\n",
          m[0], m[1], m[2], m[3], m[4], m[5]);
      }
    }
    Serial.println("\nEND");

  } else if (line.length() > 0) {
    Serial.println("[ERROR] Unknown command. Use: SEND, FTP, STATUS, SCAN");
    Serial.println("\nEND");
  }
}

void loop() {
  dnsServer.processNextRequest();
  httpServer.handleClient();

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      processSerialCommand(serialBuf);
      serialBuf = "";
    } else {
      serialBuf += c;
    }
  }

  int clients = WiFi.softAPgetStationNum();
  if (clients != lastClientCount) {
    lastClientCount = clients;
    Serial.printf("[INFO] Connected clients: %d\n", clients);
  }

  if (LED_PIN >= 0) updateLED(clients);
  delay(10);
}

void updateLED(int clients) {
  if (clients == 0) {
    digitalWrite(LED_PIN, HIGH);
    return;
  }

  unsigned long now = millis();
  int totalPhases = clients * 2 + 4;

  if (now - lastBlinkTime > 200) {
    lastBlinkTime = now;
    blinkPhase = (blinkPhase + 1) % totalPhases;

    if (blinkPhase < clients * 2) {
      digitalWrite(LED_PIN, blinkPhase % 2 == 0 ? HIGH : LOW);
    } else {
      digitalWrite(LED_PIN, LOW);
    }
  }
}
