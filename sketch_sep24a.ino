/*********
  ESP32 web server that prints whatever you type into a text box
  on a thermal receipt printer, using one of several layouts.

  Uses the WebServer library that ships with the ESP32 Arduino core.
  Printer commands are standard ESC/POS.
*********/

#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <LittleFS.h>

// Replace with your network credentials
const char* ssid = "<SSID>";
const char* password = "<PASSWORD>";

// === Web Login ===
// The browser will prompt for these before showing the page or accepting a print.
// Pick a long password: Basic Auth is only as strong as the password itself.
const char* WEB_USERNAME = "<USER>";
const char* WEB_PASSWORD = "<PASSWORD>";

// === Wi-Fi Reconnect ===
// How long to wait for the initial connection at boot before giving up and
// letting the loop() watchdog keep retrying in the background.
const unsigned long WIFI_BOOT_TIMEOUT_MS = 20000;
// How often the watchdog retries once the connection has dropped.
const unsigned long WIFI_RETRY_INTERVAL_MS = 10000;
unsigned long lastWifiRetry = 0;
bool wasConnected = false;

// === Time Configuration ===
// POSIX timezone string. Handles daylight saving automatically. Examples:
//   US Eastern:  "EST5EDT,M3.2.0,M11.1.0"
//   US Central:  "CST6CDT,M3.2.0,M11.1.0"
//   US Mountain: "MST7MDT,M3.2.0,M11.1.0"
//   US Pacific:  "PST8PDT,M3.2.0,M11.1.0"
//   UK:          "GMT0BST,M3.5.0/1,M10.5.0"
//   UTC:         "UTC0"
const char* TZ_STRING = "MST7MDT,M3.2.0,M11.1.0";
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";

// Web server on port 80
WebServer server(80);

// === Printer Setup ===
// Serial2 is used for the printer. On the ELEGOO / DOIT style boards
// these are the pins labelled TX2 and RX2. Wiring:
//   ESP32 GPIO 17 (TX2) -> printer RX
//   ESP32 GPIO 16 (RX2) -> printer TX (optional, only needed for status reads)
//   ESP32 GND           -> printer GND (must share a common ground)
//   printer DTR         -> leave unconnected for now
// Power the printer from its own supply, not from the ESP32.
const int PRINTER_RX_PIN = 16;
const int PRINTER_TX_PIN = 17;
const long PRINTER_BAUD = 9600;   // Most cheap thermal printers default to 9600 or 19200
HardwareSerial& printer = Serial2;

// Longest message accepted from the web page
const int MAX_MESSAGE_LENGTH = 500;

// Characters per line on the printer. Standard 58mm paper is 32 columns.
// Double-width text gets half as many.
const int PRINTER_COLUMNS = 32;

// === Message Log ===
// Every print is appended to a tab-separated file on the ESP32's flash:
//   time <TAB> ip <TAB> layout <TAB> message
// When the file passes MAX_LOG_BYTES it becomes /log.old.txt (replacing the
// previous old file) and a fresh /log.txt is started, so at most about
// 2 x MAX_LOG_BYTES of flash is ever used.
// View it at /log, download it at /log.txt. Both always require the login
// above, even while requireLogin() is switched off for printing.
const char* LOG_PATH = "/log.txt";
const char* LOG_OLD_PATH = "/log.old.txt";
const size_t MAX_LOG_BYTES = 100 * 1024;
bool logReady = false;

// Set by handlePrint() and written to the log in loop(), after the HTTP
// response has gone out.
String pendingIP = "";
String pendingLayoutName = "";

// === Layouts ===
enum Layout {
  LAYOUT_NOTE,
  LAYOUT_TASKS,
  LAYOUT_ACHIEVEMENT,
  LAYOUT_REMINDER,
  LAYOUT_LABEL
};

// Message waiting to be printed. Set by the web handler, consumed in loop()
// so the browser gets its response before the (slow) printing starts.
String pendingMessage = "";
Layout pendingLayout = LAYOUT_NOTE;
bool printRequested = false;

// === Function Declarations ===
void handleRoot();
void handlePrint();
void handleNotFound();
bool requireLogin();
bool requireAdmin();
void handleLog();
void handleLogDownload();
void handleLogClear();
void initLog();
void logMessage(const String& ip, const String& layout, const String& msg);
void streamLogFile(const char* path);
String htmlEscape(const String& s);
String logTimestamp();
void connectWiFi();
void checkWiFi();
void initializePrinter();
void printMessage(const String& text, Layout layout);
void printNote(const String& text);
void printTasks(const String& text);
void printAchievement(const String& text);
void printReminder(const String& text);
void printLabel(const String& text);
void printWrapped(const String& text, int cols, const String& firstPrefix, const String& contPrefix);
void printWrappedLine(const String& line, int cols, const String& firstPrefix, const String& contPrefix);
String padCenter(const String& text, int width);
void setBold(bool on);
void setInverse(bool on);
void setAlign(int align);
void setSize(bool doubleWidth, bool doubleHeight);
void resetFormat();
void advancePaper(int lines);
void syncTime();
String getTimestamp();
Layout parseLayout(const String& name);

void setup() {
  Serial.begin(115200);

  initializePrinter();
  initLog();

  // Connect to Wi-Fi. If this times out, the watchdog in loop() keeps trying.
  connectWiFi();

  // Get the current time from the internet
  if (WiFi.status() == WL_CONNECTED) {
    syncTime();
  }

  // Routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/print", HTTP_POST, handlePrint);
  server.on("/log", HTTP_GET, handleLog);
  server.on("/log.txt", HTTP_GET, handleLogDownload);
  server.on("/log/clear", HTTP_POST, handleLogClear);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Web server started");
}

void loop() {
  checkWiFi();
  server.handleClient();

  // Print outside the request handler so the HTTP response isn't held up
  if (printRequested) {
    logMessage(pendingIP, pendingLayoutName, pendingMessage);
    printMessage(pendingMessage, pendingLayout);
    pendingMessage = "";
    pendingIP = "";
    pendingLayoutName = "";
    printRequested = false;
  }
}

// === Web Handlers ===
void handleRoot() {
  if (!requireLogin()) return;

  String html = R"rawliteral(<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <link rel="icon" href="data:,">
  <title>ESP32 Printer</title>
  <style>
    html { font-family: Helvetica; text-align: center; background: #f4f4f4; }
    body { margin: 0; padding: 16px; }
    h1 { margin: 8px 0 16px; }
    textarea { width: 100%; max-width: 420px; box-sizing: border-box; font-size: 18px; padding: 8px; }
    .button { background-color: #4CAF50; border: none; color: white; padding: 16px 40px;
              font-size: 24px; margin: 8px; cursor: pointer; border-radius: 6px; }
    .banner { background-color: #dff0d8; color: #3c763d; padding: 12px; margin: 0 auto 16px;
              max-width: 420px; border-radius: 6px; font-size: 18px; }

    /* Layout picker */
    .layouts { display: flex; flex-wrap: wrap; justify-content: center; gap: 12px; max-width: 900px; margin: 0 auto 16px; }
    .card { display: block; width: 160px; background: white; border: 2px solid #ddd; border-radius: 8px;
            padding: 10px; cursor: pointer; text-align: left; }
    .card:has(input:checked) { border-color: #4CAF50; box-shadow: 0 0 0 2px #4CAF5044; }
    .card input { display: none; }
    .card .title { font-weight: bold; font-size: 16px; margin-bottom: 2px; }
    .card .desc { font-size: 12px; color: #666; margin-bottom: 8px; min-height: 30px; }

    /* Fake receipt preview */
    .paper { background: #fff; border-top: 1px dashed #bbb; border-bottom: 1px dashed #bbb;
             font-family: "Courier New", monospace; font-size: 9px; line-height: 1.3;
             padding: 6px 4px; color: #222; white-space: pre; overflow: hidden; }
    .paper .c { text-align: center; }
    .paper .b { font-weight: bold; }
    .paper .big { font-size: 14px; font-weight: bold; text-align: center; }
    .paper .inv { background: #222; color: #fff; text-align: center; font-weight: bold; }
    .paper .dim { color: #888; }
  </style>
</head>
<body>
  <h1>ESP32 Printer</h1>
  %BANNER%
  <form action="/print" method="post">

    <div class="layouts">

      <label class="card">
        <input type="radio" name="layout" value="note" checked>
        <div class="title">Note</div>
        <div class="desc">Plain text with a timestamp. Good for anything.</div>
        <div class="paper"><div class="dim">Thu, 24 Sep 2026 11:10 PM</div>--------------------------
Pick up the package from
the front office before
5pm.</div>
      </label>

      <label class="card">
        <input type="radio" name="layout" value="tasks">
        <div class="title">Task List</div>
        <div class="desc">One task per line. Each gets a checkbox.</div>
        <div class="paper"><div class="c b">TASKS</div><div class="c dim">Thu, 24 Sep 2026</div>--------------------------
[ ] Buy milk
[ ] Call the dentist
[ ] Fix the bike tire
[ ] Water the plants</div>
      </label>

      <label class="card">
        <input type="radio" name="layout" value="achievement">
        <div class="title">Achievement</div>
        <div class="desc">Celebrate something you finished.</div>
        <div class="paper"><div class="c">**************************</div><div class="big">ACHIEVEMENT</div><div class="big">UNLOCKED</div><div class="c">**************************</div>
<div class="c b">Finished the ESP32
printer project</div>
<div class="c dim">Thu, 24 Sep 2026</div></div>
      </label>

      <label class="card">
        <input type="radio" name="layout" value="reminder">
        <div class="title">Reminder</div>
        <div class="desc">Hard to miss. Stick it where you'll see it.</div>
        <div class="paper"><div class="inv">        REMINDER        </div>
<div class="b">Take the trash out
tonight. Pickup is
early tomorrow.</div>
<div class="dim">Thu, 24 Sep 2026 11:10 PM</div></div>
      </label>

      <label class="card">
        <input type="radio" name="layout" value="label">
        <div class="title">Label</div>
        <div class="desc">Big text, no date. For jars, boxes, and bins.</div>
        <div class="paper">
<div class="big">COFFEE BEANS</div><div class="big">DARK ROAST</div>
</div>
      </label>

    </div>

    <p><textarea name="msg" rows="4" maxlength="500" placeholder="Type something to print..." required autofocus></textarea></p>
    <p><button class="button" type="submit">Print</button></p>
  </form>
</body>
</html>)rawliteral";

  // Show a confirmation if we were redirected here after a print
  if (server.hasArg("printed")) {
    html.replace("%BANNER%", "<div class=\"banner\">Printed!</div>");
  } else {
    html.replace("%BANNER%", "");
  }

  server.send(200, "text/html", html);
}

void handlePrint() {
  if (!requireLogin()) return;

  if (!server.hasArg("msg")) {
    server.send(400, "text/plain", "Missing msg field");
    return;
  }

  String msg = server.arg("msg");
  msg.trim();
  if (msg.length() == 0) {
    server.send(400, "text/plain", "Nothing to print");
    return;
  }
  if (msg.length() > MAX_MESSAGE_LENGTH) {
    msg = msg.substring(0, MAX_MESSAGE_LENGTH);
  }

  pendingMessage = msg;
  pendingLayout = parseLayout(server.arg("layout"));
  // Store the layout as the canonical name, not whatever string was posted
  const char* layoutNames[] = { "note", "tasks", "achievement", "reminder", "label" };
  pendingLayoutName = layoutNames[pendingLayout];
  // No proxy in front, so the connection's source address is the sender.
  // (Requests from inside the LAN via NAT loopback may show the router's IP.)
  pendingIP = server.client().remoteIP().toString();
  printRequested = true;

  Serial.print("Print requested from ");
  Serial.print(pendingIP);
  Serial.println(":");
  Serial.println(msg);

  // Redirect back to the form so a browser refresh doesn't reprint.
  // The query flag tells the form page to show the "Printed!" banner.
  server.sendHeader("Location", "/?printed=1");
  server.send(303);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// Returns true if the request carries valid credentials. Otherwise it sends
// the 401 challenge (which makes the browser show its login box) and returns
// false, so the caller should just return without sending anything else.
bool requireLogin() {
  if (server.authenticate(WEB_USERNAME, WEB_PASSWORD)) {
    return true;
  }
  server.requestAuthentication(BASIC_AUTH, "ESP32 Printer", "Login required");
  return false;
}

// Same as requireLogin() but never bypassed. Guards the log pages, which
// hold every visitor's IP address and message.
bool requireAdmin() {
  if (server.authenticate(WEB_USERNAME, WEB_PASSWORD)) {
    return true;
  }
  server.requestAuthentication(BASIC_AUTH, "ESP32 Printer", "Login required");
  return false;
}

// === Log Pages ===

// Table of every logged print, oldest first. Streamed in chunks so a large
// log never has to fit in RAM at once.
void handleLog() {
  if (!requireAdmin()) return;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent(R"rawliteral(<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <link rel="icon" href="data:,">
  <title>Print Log</title>
  <style>
    body { font-family: Helvetica; background: #f4f4f4; margin: 0; padding: 16px; }
    h1 { margin: 8px 0 12px; }
    table { border-collapse: collapse; background: white; width: 100%; font-size: 14px; }
    th, td { border: 1px solid #ddd; padding: 6px 8px; text-align: left; vertical-align: top; }
    th { background: #eee; }
    td.msg { white-space: pre-wrap; word-break: break-word; font-family: "Courier New", monospace; }
    td.nowrap { white-space: nowrap; }
    .actions { margin-bottom: 12px; }
    .actions a, .actions button { margin-right: 12px; font-size: 14px; }
  </style>
</head>
<body>
  <h1>Print Log</h1>
  <div class="actions">
    <a href="/">Back to printer</a>
    <a href="/log.txt">Download .txt</a>
    <form action="/log/clear" method="post" style="display:inline"
          onsubmit="return confirm('Delete the whole log?')">
      <button type="submit">Clear log</button>
    </form>
  </div>
  <table>
    <tr><th>Time</th><th>IP</th><th>Layout</th><th>Message</th></tr>
)rawliteral");

  if (logReady) {
    streamLogFile(LOG_OLD_PATH);
    streamLogFile(LOG_PATH);
  } else {
    server.sendContent("<tr><td colspan=\"4\">Log storage unavailable</td></tr>");
  }

  server.sendContent("</table>\n</body>\n</html>\n");
  server.sendContent("");  // end of chunked response
}

// Sends each line of a log file as a table row. Every field is HTML-escaped:
// messages come from strangers, and without escaping a message containing
// <script> would run in your browser when you open this page.
void streamLogFile(const char* path) {
  if (!LittleFS.exists(path)) return;
  File f = LittleFS.open(path, "r");
  if (!f) return;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.length() == 0) continue;

    String fields[4];
    int start = 0;
    for (int i = 0; i < 4; i++) {
      int tab = (i < 3) ? line.indexOf('\t', start) : -1;
      if (tab == -1) {
        fields[i] = line.substring(start);
        start = line.length();
      } else {
        fields[i] = line.substring(start, tab);
        start = tab + 1;
      }
    }

    // Line breaks were stored as a literal \n; turn them back into real ones
    String msg = fields[3];
    msg.replace("\\n", "\n");

    String row = "<tr><td class=\"nowrap\">" + htmlEscape(fields[0]) +
                 "</td><td class=\"nowrap\">" + htmlEscape(fields[1]) +
                 "</td><td>" + htmlEscape(fields[2]) +
                 "</td><td class=\"msg\">" + htmlEscape(msg) + "</td></tr>\n";
    server.sendContent(row);
  }
  f.close();
}

// Raw tab-separated download of the whole log (old file first).
void handleLogDownload() {
  if (!requireAdmin()) return;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Content-Disposition", "attachment; filename=\"print-log.txt\"");
  server.send(200, "text/plain; charset=utf-8", "");
  server.sendContent("time\tip\tlayout\tmessage\n");

  const char* paths[] = { LOG_OLD_PATH, LOG_PATH };
  char buf[512];
  for (const char* path : paths) {
    if (!logReady || !LittleFS.exists(path)) continue;
    File f = LittleFS.open(path, "r");
    if (!f) continue;
    while (f.available()) {
      size_t n = f.readBytes(buf, sizeof(buf));
      server.sendContent(buf, n);
    }
    f.close();
  }
  server.sendContent("");
}

void handleLogClear() {
  if (!requireAdmin()) return;
  if (logReady) {
    LittleFS.remove(LOG_PATH);
    LittleFS.remove(LOG_OLD_PATH);
  }
  Serial.println("Log cleared");
  server.sendHeader("Location", "/log");
  server.send(303);
}

Layout parseLayout(const String& name) {
  if (name == "tasks") return LAYOUT_TASKS;
  if (name == "achievement") return LAYOUT_ACHIEVEMENT;
  if (name == "reminder") return LAYOUT_REMINDER;
  if (name == "label") return LAYOUT_LABEL;
  return LAYOUT_NOTE;
}

// === Log Functions ===

void initLog() {
  // true = format the filesystem if it can't be mounted (first boot)
  logReady = LittleFS.begin(true);
  if (logReady) {
    Serial.printf("Log storage ready: %u of %u bytes used\n",
                  (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
  } else {
    Serial.println("LittleFS mount failed, prints will not be logged");
  }
}

void logMessage(const String& ip, const String& layout, const String& msg) {
  if (!logReady) return;

  // Rotate once the current file is over the size cap
  if (LittleFS.exists(LOG_PATH)) {
    File f = LittleFS.open(LOG_PATH, "r");
    size_t size = f ? f.size() : 0;
    if (f) f.close();
    if (size > MAX_LOG_BYTES) {
      LittleFS.remove(LOG_OLD_PATH);
      LittleFS.rename(LOG_PATH, LOG_OLD_PATH);
    }
  }

  // Keep each entry on one line: tabs separate fields, so strip them from the
  // message, and store line breaks as a literal \n
  String clean = msg;
  clean.replace("\t", " ");
  clean.replace("\r\n", "\n");
  clean.replace("\r", "\n");
  clean.replace("\n", "\\n");

  File f = LittleFS.open(LOG_PATH, "a");
  if (!f) {
    Serial.println("Could not open log file");
    return;
  }
  f.print(logTimestamp());
  f.print('\t');
  f.print(ip);
  f.print('\t');
  f.print(layout);
  f.print('\t');
  f.print(clean);
  f.print('\n');
  f.close();
}

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 16);
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += c;        break;
    }
  }
  return out;
}

// Sortable timestamp for the log, e.g. "2026-10-09 09:30:12".
// Falls back to seconds since boot if NTP hasn't synced yet.
String logTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) {
    return "uptime+" + String(millis() / 1000) + "s";
  }
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

// === Wi-Fi Functions ===
void connectWiFi() {
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_BOOT_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");

  if (WiFi.status() == WL_CONNECTED) {
    wasConnected = true;
    Serial.println("WiFi connected.");
    Serial.print("Open http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi not connected yet, will keep retrying in the background");
  }
}

// Called every loop. Non-blocking: if the link is down it kicks off a
// reconnect at most once per retry interval and returns immediately.
void checkWiFi() {
  bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected) {
    if (!wasConnected) {
      // Just came back after a drop (or after a failed boot connect)
      Serial.print("WiFi reconnected. IP: ");
      Serial.println(WiFi.localIP());
      syncTime();
    }
    wasConnected = true;
    return;
  }

  if (wasConnected) {
    Serial.println("WiFi connection lost");
    wasConnected = false;
  }

  unsigned long now = millis();
  if (now - lastWifiRetry < WIFI_RETRY_INTERVAL_MS) {
    return;
  }
  lastWifiRetry = now;

  Serial.println("Retrying WiFi...");
  WiFi.disconnect();
  WiFi.begin(ssid, password);
}

// === Printer Functions ===
void initializePrinter() {
  printer.begin(PRINTER_BAUD, SERIAL_8N1, PRINTER_RX_PIN, PRINTER_TX_PIN);
  delay(500);

  // ESC @ : reset the printer to its default settings
  printer.write(0x1B);
  printer.write('@');
  delay(50);

  Serial.println("Printer initialized");
}

void printMessage(const String& text, Layout layout) {
  Serial.println("Printing...");

  // Browsers send textarea line breaks as \r\n; the printer only wants \n
  String cleaned = text;
  cleaned.replace("\r\n", "\n");
  cleaned.replace("\r", "\n");

  resetFormat();

  switch (layout) {
    case LAYOUT_TASKS:       printTasks(cleaned);       break;
    case LAYOUT_ACHIEVEMENT: printAchievement(cleaned); break;
    case LAYOUT_REMINDER:    printReminder(cleaned);    break;
    case LAYOUT_LABEL:       printLabel(cleaned);       break;
    default:                 printNote(cleaned);        break;
  }

  resetFormat();

  // Feed a few lines so the text clears the tear bar
  advancePaper(3);

  Serial.println("Print done");
}

// --- Layouts ---

// Timestamp, separator, plain wrapped text.
void printNote(const String& text) {
  printer.println(getTimestamp());
  printer.println("--------------------------------");
  printWrapped(text, PRINTER_COLUMNS, "", "");
}

// Bold centered header, date, then one checkbox per input line.
void printTasks(const String& text) {
  setAlign(1);
  setBold(true);
  setSize(false, true);
  printer.println("TASKS");
  setSize(false, false);
  setBold(false);
  printer.println(getTimestamp());
  setAlign(0);
  printer.println("--------------------------------");

  // Each non-empty line is a task. Continuation lines indent under the box.
  int start = 0;
  while (start <= (int)text.length()) {
    int nl = text.indexOf('\n', start);
    if (nl == -1) nl = text.length();
    String line = text.substring(start, nl);
    line.trim();
    if (line.length() > 0) {
      printWrappedLine(line, PRINTER_COLUMNS, "[ ] ", "    ");
    }
    start = nl + 1;
  }
}

// Star border, big "ACHIEVEMENT UNLOCKED", bold centered message, date.
void printAchievement(const String& text) {
  setAlign(1);
  printer.println("********************************");
  setBold(true);
  setSize(true, true);
  printer.println("NEW");
  printer.println("ACHIEVEMENT");
  setSize(false, false);
  setBold(false);
  printer.println("********************************");
  printer.println();

  setBold(true);
  printWrapped(text, PRINTER_COLUMNS, "", "");
  setBold(false);

  printer.println();
  printer.println(getTimestamp());
  setAlign(0);
}

// Full-width inverse header bar, bold message, timestamp.
void printReminder(const String& text) {
  setInverse(true);
  setBold(true);
  setSize(false, true);
  printer.println(padCenter("REMINDER", PRINTER_COLUMNS));
  setSize(false, false);
  setBold(false);
  setInverse(false);
  printer.println();

  setBold(true);
  printWrapped(text, PRINTER_COLUMNS, "", "");
  setBold(false);

  printer.println();
  printer.println(getTimestamp());
}

// Double-size bold centered text, nothing else. Wraps at half the columns
// because double-width characters take two cells each.
void printLabel(const String& text) {
  setAlign(1);
  setBold(true);
  setSize(true, true);
  printWrapped(text, PRINTER_COLUMNS / 2, "", "");
  setSize(false, false);
  setBold(false);
  setAlign(0);
}

// --- Word wrapping ---

// Splits the text on the user's own line breaks, then word-wraps each
// paragraph so lines break between words rather than mid-word.
// A blank line in the input prints as a blank line.
void printWrapped(const String& text, int cols, const String& firstPrefix, const String& contPrefix) {
  int start = 0;
  while (start <= (int)text.length()) {
    int nl = text.indexOf('\n', start);
    if (nl == -1) nl = text.length();
    printWrappedLine(text.substring(start, nl), cols, firstPrefix, contPrefix);
    start = nl + 1;
  }
}

// Word-wraps a single paragraph (no newlines inside) to `cols` characters.
// The first printed line is prefixed with firstPrefix, the rest with
// contPrefix (used for hanging indents on task lists). Words longer than
// a full line are broken at the column limit.
void printWrappedLine(const String& line, int cols, const String& firstPrefix, const String& contPrefix) {
  if (line.length() == 0) {
    printer.println();
    return;
  }

  String current = "";
  String prefix = firstPrefix;
  int pos = 0;
  int len = line.length();

  while (pos < len) {
    // Skip runs of spaces between words
    while (pos < len && line[pos] == ' ') pos++;
    if (pos >= len) break;

    // Grab the next word
    int wordEnd = line.indexOf(' ', pos);
    if (wordEnd == -1) wordEnd = len;
    String word = line.substring(pos, wordEnd);
    pos = wordEnd;

    int avail = cols - prefix.length();

    // A word that can't fit on any line gets chopped into column-sized pieces
    while ((int)word.length() > avail) {
      if (current.length() > 0) {
        printer.println(prefix + current);
        current = "";
        prefix = contPrefix;
        avail = cols - prefix.length();
      }
      printer.println(prefix + word.substring(0, avail));
      word = word.substring(avail);
      prefix = contPrefix;
      avail = cols - prefix.length();
    }

    // Does the word fit on the current line (with a space before it)?
    int needed = current.length() == 0 ? word.length() : current.length() + 1 + word.length();
    if (needed <= avail) {
      if (current.length() > 0) current += ' ';
      current += word;
    } else {
      printer.println(prefix + current);
      prefix = contPrefix;
      current = word;
    }
  }

  if (current.length() > 0) {
    printer.println(prefix + current);
  }
}

// Pads text with spaces on both sides to fill `width` characters. Used to
// make inverse headers span the full paper width.
String padCenter(const String& text, int width) {
  int total = width - text.length();
  if (total <= 0) return text;
  int left = total / 2;
  int right = total - left;
  String out = "";
  for (int i = 0; i < left; i++) out += ' ';
  out += text;
  for (int i = 0; i < right; i++) out += ' ';
  return out;
}

// --- ESC/POS formatting ---

void setBold(bool on) {
  printer.write(0x1B); printer.write('E'); printer.write(on ? 1 : 0);   // ESC E n
}

void setInverse(bool on) {
  printer.write(0x1D); printer.write('B'); printer.write(on ? 1 : 0);   // GS B n
}

// 0 = left, 1 = center, 2 = right
void setAlign(int align) {
  printer.write(0x1B); printer.write('a'); printer.write(align);        // ESC a n
}

// GS ! n : bit 4 = double width, bit 0 = double height.
// If sizing does nothing on your printer, try ESC ! n instead with
// 0x20 for double width and 0x10 for double height.
void setSize(bool doubleWidth, bool doubleHeight) {
  uint8_t n = (doubleWidth ? 0x10 : 0x00) | (doubleHeight ? 0x01 : 0x00);
  printer.write(0x1D); printer.write('!'); printer.write(n);
}

void resetFormat() {
  setBold(false);
  setInverse(false);
  setAlign(0);
  setSize(false, false);
}

void advancePaper(int lines) {
  for (int i = 0; i < lines; i++) {
    printer.write(0x0A); // LF
  }
}

// === Time Functions ===
void syncTime() {
  // Start the built-in SNTP client. It keeps the clock synced in the
  // background from here on, so this only needs to run once.
  configTzTime(TZ_STRING, NTP_SERVER_1, NTP_SERVER_2);

  Serial.print("Syncing time");
  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo, 1000) && attempts < 10) {
    Serial.print(".");
    attempts++;
  }
  Serial.println("");

  if (attempts < 10) {
    Serial.print("Time set: ");
    Serial.println(getTimestamp());
  } else {
    Serial.println("Time sync failed, printouts will show 'time unknown' until it succeeds");
  }
}

String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) {
    return "time unknown";
  }
  // e.g. "Thu, 24 Sep 2026 11:10 PM"
  char buf[32];
  strftime(buf, sizeof(buf), "%a, %d %b %Y %I:%M %p", &timeinfo);
  return String(buf);
}