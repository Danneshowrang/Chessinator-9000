/*
 * Foldable Chess Sensing Board — Crowd Vote Firmware
 * ESP32-S3-WROOM-1
 *
 * Flow:
 *  1. Connect to venue WiFi
 *  2. Host a web page (reachable via QR code) listing candidate moves
 *  3. Collect votes for a timed window
 *  4. POST the winning move to a companion relay server (laptop running
 *     Stockfish / Lichess API bridge) for validation + next-move suggestion
 *  5. Flash WS2812B LEDs on the winning move's from/to squares
 *  6. Blank hall sensors during the move window, then rescan to verify
 *     the human actually made the move
 *
 * Libraries needed (PlatformIO lib_deps):
 *   ESP Async WebServer
 *   AsyncTCP
 *   ArduinoJson
 *   Adafruit NeoPixel
 *
 * Hardware:
 *   4x CD74HC4067 mux, 16 hall sensors each (64 total)
 *   64x WS2812B LEDs, one per square, single data pin
 *   4 shared select lines (S0-S3), 4 separate COM->ADC lines
 */

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>

// ---------- Pin map (match schematic labels) ----------
constexpr uint8_t PIN_MUX_S0   = 4;
constexpr uint8_t PIN_MUX_S1   = 5;
constexpr uint8_t PIN_MUX_S2   = 6;
constexpr uint8_t PIN_MUX_S3   = 7;

constexpr uint8_t PIN_MUX1_COM = 1;   // IO1 - ADC
constexpr uint8_t PIN_MUX2_COM = 2;   // IO2 - ADC
constexpr uint8_t PIN_MUX3_COM = 38;  // IO38 - ADC (via J67/FFC)
constexpr uint8_t PIN_MUX4_COM = 39;  // IO39 - ADC (via J67/FFC)

constexpr uint8_t PIN_LED_DATA = 12;

constexpr uint8_t NUM_LEDS = 64;
constexpr uint8_t NUM_MUX  = 4;
constexpr uint8_t NUM_CH   = 16;

// ---------- WiFi / server config ----------
const char* WIFI_SSID = "YOUR_VENUE_SSID";
const char* WIFI_PASS = "YOUR_VENUE_PASS";

// Laptop relay server running Stockfish / Lichess bridge
const char* RELAY_HOST = "192.168.1.50";
const uint16_t RELAY_PORT = 8000;

AsyncWebServer server(80);
Adafruit_NeoPixel strip(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);

// ---------- Vote state ----------
struct VoteTally {
  String move;
  uint32_t count;
};

std::vector<VoteTally> currentCandidates;
std::vector<String> receivedVotes;
bool votingOpen = false;
uint32_t voteWindowMs = 20000;   // 20s voting window
uint32_t voteStartTime = 0;

// ---------- Hall sensor raw readings ----------
uint16_t hallRaw[NUM_MUX][NUM_CH];

const uint8_t muxCOMPins[NUM_MUX] = {
  PIN_MUX1_COM, PIN_MUX2_COM, PIN_MUX3_COM, PIN_MUX4_COM
};

// ---------- Forward declarations ----------
void setMuxChannel(uint8_t ch);
void scanAllHallSensors();
String buildVotePageHTML();
void startVotingRound(std::vector<String> candidateMoves);
void tallyAndResolveVote();
void sendMoveToRelay(const String& move);
void flashMoveOnBoard(const String& fromSquare, const String& toSquare);
uint8_t squareToLedIndex(const String& square);

void setup() {
  Serial.begin(115200);

  pinMode(PIN_MUX_S0, OUTPUT);
  pinMode(PIN_MUX_S1, OUTPUT);
  pinMode(PIN_MUX_S2, OUTPUT);
  pinMode(PIN_MUX_S3, OUTPUT);

  strip.begin();
  strip.show();

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected. IP: ");
  Serial.println(WiFi.localIP());
  Serial.println("Point QR code / browsers at http://" + WiFi.localIP().toString());

  // ---- Web routes ----
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "text/html", buildVotePageHTML());
  });

  server.on("/vote", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!votingOpen) {
      request->send(409, "text/plain", "Voting closed");
      return;
    }
    if (request->hasParam("move", true)) {
      String move = request->getParam("move", true)->value();
      receivedVotes.push_back(move);
      request->send(200, "text/plain", "Vote recorded: " + move);
    } else {
      request->send(400, "text/plain", "Missing move param");
    }
  });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["voting_open"] = votingOpen;
    doc["votes_so_far"] = receivedVotes.size();
    if (votingOpen) {
      uint32_t elapsed = millis() - voteStartTime;
      doc["ms_remaining"] = (elapsed < voteWindowMs) ? (voteWindowMs - elapsed) : 0;
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.begin();

  // Kick off the very first voting round with a placeholder candidate list.
  // In production, candidateMoves should come from the relay server's
  // Stockfish output (top N candidate moves for the current position).
  std::vector<String> demoMoves = {"e2e4", "d2d4", "g1f3", "c2c4"};
  startVotingRound(demoMoves);
}

void loop() {
  if (votingOpen && (millis() - voteStartTime >= voteWindowMs)) {
    tallyAndResolveVote();
  }
  delay(50);
}

// ---------------------------------------------------------------
// Mux control
// ---------------------------------------------------------------
void setMuxChannel(uint8_t ch) {
  digitalWrite(PIN_MUX_S0, ch & 0x01);
  digitalWrite(PIN_MUX_S1, (ch >> 1) & 0x01);
  digitalWrite(PIN_MUX_S2, (ch >> 2) & 0x01);
  digitalWrite(PIN_MUX_S3, (ch >> 3) & 0x01);
  delayMicroseconds(5); // settle time
}

void scanAllHallSensors() {
  for (uint8_t ch = 0; ch < NUM_CH; ch++) {
    setMuxChannel(ch);
    for (uint8_t m = 0; m < NUM_MUX; m++) {
      hallRaw[m][ch] = analogRead(muxCOMPins[m]);
    }
  }
}

// ---------------------------------------------------------------
// Voting
// ---------------------------------------------------------------
void startVotingRound(std::vector<String> candidateMoves) {
  currentCandidates.clear();
  for (auto& m : candidateMoves) {
    currentCandidates.push_back({m, 0});
  }
  receivedVotes.clear();
  votingOpen = true;
  voteStartTime = millis();
  Serial.println("Voting round started with " + String(candidateMoves.size()) + " candidates");
}

void tallyAndResolveVote() {
  votingOpen = false;

  for (auto& v : receivedVotes) {
    for (auto& c : currentCandidates) {
      if (c.move == v) c.count++;
    }
  }

  String winner = "";
  uint32_t best = 0;
  for (auto& c : currentCandidates) {
    if (c.count > best) {
      best = c.count;
      winner = c.move;
    }
  }

  if (winner == "" && !currentCandidates.empty()) {
    winner = currentCandidates[0].move; // fallback if no votes came in
  }

  Serial.println("Winning move: " + winner + " (" + String(best) + " votes)");

  sendMoveToRelay(winner);
}

// ---------------------------------------------------------------
// Relay communication (laptop running Stockfish bridge)
// ---------------------------------------------------------------
void sendMoveToRelay(const String& move) {
  WiFiClient client;
  if (!client.connect(RELAY_HOST, RELAY_PORT)) {
    Serial.println("Relay connection failed");
    return;
  }

  JsonDocument doc;
  doc["move"] = move;
  String body;
  serializeJson(doc, body);

  client.println("POST /move HTTP/1.1");
  client.println("Host: " + String(RELAY_HOST));
  client.println("Content-Type: application/json");
  client.println("Content-Length: " + String(body.length()));
  client.println();
  client.println(body);

  // Wait briefly for a response containing from/to squares + next candidates
  uint32_t start = millis();
  String response;
  while (client.connected() && millis() - start < 5000) {
    while (client.available()) {
      response += (char)client.read();
    }
  }
  client.stop();

  // crude parse: expects a trailing JSON body from the relay, e.g.
  // {"from":"e2","to":"e4","next_candidates":["e7e5","c7c5","e7e6"]}
  int jsonStart = response.indexOf('{');
  if (jsonStart >= 0) {
    JsonDocument respDoc;
    DeserializationError err = deserializeJson(respDoc, response.substring(jsonStart));
    if (!err) {
      String from = respDoc["from"] | "";
      String to   = respDoc["to"]   | "";
      if (from.length() && to.length()) {
        flashMoveOnBoard(from, to);
      }

      std::vector<String> nextMoves;
      if (respDoc["next_candidates"].is<JsonArray>()) {
        for (JsonVariant v : respDoc["next_candidates"].as<JsonArray>()) {
          nextMoves.push_back(v.as<String>());
        }
      }
      if (!nextMoves.empty()) {
        startVotingRound(nextMoves);
      }
      return;
    }
  }
  Serial.println("Failed to parse relay response");
}

// ---------------------------------------------------------------
// LED display + move verification
// ---------------------------------------------------------------
uint8_t squareToLedIndex(const String& square) {
  // "e4" -> file e (0-7), rank 4 (0-7) -> index = rank*8 + file
  if (square.length() < 2) return 255;
  char file = square[0];
  char rank = square[1];
  if (file < 'a' || file > 'h') return 255;
  if (rank < '1' || rank > '8') return 255;
  uint8_t f = file - 'a';
  uint8_t r = rank - '1';
  return r * 8 + f;
}

void flashMoveOnBoard(const String& fromSquare, const String& toSquare) {
  uint8_t fromIdx = squareToLedIndex(fromSquare);
  uint8_t toIdx   = squareToLedIndex(toSquare);

  if (fromIdx == 255 || toIdx == 255) {
    Serial.println("Invalid square in flashMoveOnBoard");
    return;
  }

  // Blank hall sensors are not physically possible (they're passive),
  // but we pause scanning/interpreting them during the flash+move window
  // so a lifted piece mid-move doesn't register as a false event.
  bool sensingPaused = true;

  for (int i = 0; i < 6; i++) {
    strip.setPixelColor(fromIdx, strip.Color(0, 255, 0));
    strip.setPixelColor(toIdx,   strip.Color(0, 100, 255));
    strip.show();
    delay(300);
    strip.clear();
    strip.show();
    delay(200);
  }

  // Give the human time to physically move the piece, then resume
  // sensing and verify placement.
  delay(4000);
  sensingPaused = false;

  scanAllHallSensors();
  // TODO: compare hallRaw against expected post-move magnet pattern
  // and re-flash toSquare in red if verification fails.
  Serial.println("Move window complete, sensors rescanned.");
}

// ---------------------------------------------------------------
// Web page
// ---------------------------------------------------------------
String buildVotePageHTML() {
  String html = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Vote the Next Move</title>
  <style>
    body { font-family: sans-serif; text-align: center; background:#1a1a1a; color:#eee; }
    button { font-size: 1.5em; margin: 10px; padding: 14px 24px; border-radius: 10px; border:none; background:#3a7; color:#fff; }
    button:active { background:#284; }
    #status { margin-top: 20px; font-size: 1.1em; }
  </style>
</head>
<body>
  <h1>Vote for the Next Move</h1>
  <div id="candidates"></div>
  <div id="status">Loading...</div>

<script>
async function loadCandidates() {
  // In this simplified demo, candidates are hardcoded to match firmware.
  // Production version should fetch them from /status or a dedicated endpoint.
  const moves = ["e2e4", "d2d4", "g1f3", "c2c4"];
  const container = document.getElementById('candidates');
  container.innerHTML = '';
  moves.forEach(m => {
    const btn = document.createElement('button');
    btn.innerText = m;
    btn.onclick = () => vote(m);
    container.appendChild(btn);
  });
}

async function vote(move) {
  const res = await fetch('/vote', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: 'move=' + encodeURIComponent(move)
  });
  document.getElementById('status').innerText = await res.text();
}

async function pollStatus() {
  try {
    const res = await fetch('/status');
    const data = await res.json();
    if (data.voting_open) {
      document.getElementById('status').innerText =
        'Votes so far: ' + data.votes_so_far + ' | Time left: ' + Math.ceil(data.ms_remaining/1000) + 's';
    } else {
      document.getElementById('status').innerText = 'Voting closed, resolving winner...';
    }
  } catch(e) {}
}

loadCandidates();
setInterval(pollStatus, 1000);
</script>
</body>
</html>
)HTML";
  return html;
}
