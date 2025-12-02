#include <Wire.h>
#include <VL53L0X.h>
#include <BleKeyboard.h>
#include <math.h>




// -------------------- Objects --------------------
VL53L0X lox;
BleKeyboard bleKeyboard("Gesture Speaker", "ESP32", 100);

// -------------------- Distance & Filter --------------------
int32_t distanceRaw = -1;
float   distanceFiltered = -1.0;
float   lastDistanceFiltered = -1.0;

unsigned long lastVLReadTime = 0;
const unsigned long MAIN_LOOP_DELAY = 40;  // ~25 Hz
const float FILTER_ALPHA = 0.35;           // smoothing
// -------------------- Power / Hybrid mode --------------------
enum PowerMode {
  MODE_ACTIVE,
  MODE_IDLE
};

PowerMode powerMode = MODE_ACTIVE;

unsigned long lastHandSeenTime = 0;
const unsigned long IDLE_TIMEOUT_MS = 15000;   // 15 seconds of no hand → idle

// We'll use a dynamic loop delay instead of the fixed MAIN_LOOP_DELAY
unsigned long currentLoopDelay = MAIN_LOOP_DELAY;  // start in active mode


// -------------------- Zones (in mm, adjust slightly if needed) --------------------
const uint16_t MIN_HAND_DIST  = 40;    // ignore <4 cm
const uint16_t MAX_HAND_DIST  = 900;   // ignore >90 cm

// NEAR  : volume zone (very close)
const uint16_t NEAR_MAX       = 180;   // ~4–18 cm

// MID   : play/pause zone
const uint16_t MID_MIN        = 220;   // ~22–40 cm
const uint16_t MID_MAX        = 350;

// FAR   : skip zone
const uint16_t FAR_MIN        = 420;   // ~43–90+ cm

// -------------------- Volume (swipe to set direction, then hold) --------------------
int   volumeDir = 0;                // -1 = down, +1 = up, 0 = idle
float volumeEntryDist = -1;
const uint16_t VOL_DIR_SET_DELTA = 40;      // mm movement to pick a direction
unsigned long volumeDirStartTime = 0;

unsigned long lastVolumeStepTime = 0;
const unsigned long VOLUME_STEP_INTERVAL_SLOW = 150;  // ms initially
const unsigned long VOLUME_STEP_INTERVAL_FAST = 80;   // ms after some time
const unsigned long VOLUME_SPEEDUP_MS         = 1500; // after 1.5s, go faster

// -------------------- Play/Pause (MID zone) --------------------
float holdRefDist = -1;
unsigned long holdStartTime = 0;
const uint16_t HOLD_TOL_MM = 10;           // ±10 mm band
const unsigned long PLAYPAUSE_HOLD_MS = 700;
const unsigned long PLAYPAUSE_COOLDOWN_MS = 1200;
unsigned long lastPlayPauseTime = 0;

// -------------------- Skip (FAR zone) --------------------
// -------------------- Skip (FAR zone) --------------------
// One clear swipe per "visit" to FAR zone
bool   swipeArmed = false;
float  swipeBaseDist = -1;
unsigned long swipeBaseTime = 0;
bool   wasInFarZone = false;

// Tune these if needed
const uint16_t SWIPE_DELTA_MM      = 70;   // mm change needed (~7 cm)
const unsigned long SWIPE_TIME_MAX = 600;  // ms window for swipe
const unsigned long SWIPE_COOLDOWN_MS = 900;
unsigned long lastSwipeTime = 0;

// -------------------- Helper: BLE media key --------------------
void sendMediaKey(const MediaKeyReport keycode, const char *label) {
  Serial.print("  >> ACTION: ");
  Serial.println(label);
  if (bleKeyboard.isConnected()) {
    bleKeyboard.write(keycode);
  } else {
    Serial.println("  !! BLE not connected, action not sent");
  }
}

// -------------------- Volume logic (NEAR zone) --------------------
void handleVolume(unsigned long now, bool inNearZone) {
  if (!inNearZone) {
    // leaving volume zone → reset
    volumeDir = 0;
    volumeEntryDist = -1;
    return;
  }

  // Small gap after other gestures
  if (now - lastPlayPauseTime < 300 || now - lastSwipeTime < 400) return;

  if (volumeEntryDist < 0 && distanceFiltered > 0) {
    // first time entering zone
    volumeEntryDist = distanceFiltered;
    volumeDir = 0;
    volumeDirStartTime = now;
  }

  // 1) If no direction set, watch for a swipe
  if (volumeDir == 0) {
    float delta = distanceFiltered - volumeEntryDist; // +ve = away (UP), -ve = closer (DOWN)
    if (delta > VOL_DIR_SET_DELTA) {
      volumeDir = +1;  // swipe away → volume up mode
      volumeDirStartTime = now;
      lastVolumeStepTime = now;
      Serial.print("  | VolMode:UP");
    } else if (delta < -VOL_DIR_SET_DELTA) {
      volumeDir = -1;  // swipe closer → volume down mode
      volumeDirStartTime = now;
      lastVolumeStepTime = now;
      Serial.print("  | VolMode:DOWN");
    } else {
      Serial.print("  | VolMode:NONE");
      return;
    }
  }

  // 2) Once direction chosen, continuous steps while holding
  unsigned long interval = VOLUME_STEP_INTERVAL_SLOW;
  if (now - volumeDirStartTime > VOLUME_SPEEDUP_MS) {
    interval = VOLUME_STEP_INTERVAL_FAST;   // speed up after a while
  }

  if (now - lastVolumeStepTime > interval) {
    if (volumeDir > 0) {
      sendMediaKey(KEY_MEDIA_VOLUME_UP, "VOLUME UP");
    } else if (volumeDir < 0) {
      sendMediaKey(KEY_MEDIA_VOLUME_DOWN, "VOLUME DOWN");
    }
    lastVolumeStepTime = now;
  }
}

// -------------------- Play/Pause logic (MID zone) --------------------
void handlePlayPause(unsigned long now, bool inMidZone) {
  if (!inMidZone) {
    holdRefDist = -1;
    holdStartTime = 0;
    return;
  }

  // Don't mix with very recent swipe
  if (now - lastSwipeTime < SWIPE_COOLDOWN_MS) return;

  if (holdRefDist < 0 && distanceFiltered > 0) {
    holdRefDist = distanceFiltered;
    holdStartTime = now;
  }

  float delta = fabs(distanceFiltered - holdRefDist);
  Serial.print("  | HoldDelta: ");
  Serial.print(delta, 1);

  if (delta <= HOLD_TOL_MM) {
    if (now - holdStartTime > PLAYPAUSE_HOLD_MS &&
        now - lastPlayPauseTime > PLAYPAUSE_COOLDOWN_MS) {
      sendMediaKey(KEY_MEDIA_PLAY_PAUSE, "PLAY/PAUSE");
      lastPlayPauseTime = now;
      holdRefDist = -1;
      holdStartTime = 0;
      Serial.print("  --> PLAY/PAUSE");
    }
  } else {
    // moved → restart timer
    holdRefDist = distanceFiltered;
    holdStartTime = now;
  }
}

// -------------------- Improved Skip logic (FAR zone with entry stabilization) --------------------
void handleSkip(unsigned long now, bool inFarZone) {
  static unsigned long farEntryTime = 0;
  static bool stableInFar = false;   // ensures zone is stable before swipe detection

  if (!inFarZone) {
    // reset state on exit
    wasInFarZone = false;
    swipeArmed = false;
    stableInFar = false;
    swipeBaseDist = -1;
    return;
  }

  // On first entry into FAR
  if (!wasInFarZone) {
    wasInFarZone = true;
    farEntryTime = now;
    stableInFar = false;
    swipeArmed = false;
    swipeBaseDist = distanceFiltered;
    swipeBaseTime = now;
    return;
  }

  // Require 250ms stability in FAR before swipe detection
  if (!stableInFar) {
    if (now - farEntryTime > 250) {
      stableInFar = true;
      swipeArmed = true;             // Swipe allowed only AFTER stable entry
      swipeBaseDist = distanceFiltered;
      swipeBaseTime = now;
    }
    return;
  }

  // After stable, detect swipe normally
  if (swipeArmed) {
    unsigned long dt = now - swipeBaseTime;
    float delta = distanceFiltered - swipeBaseDist;

    Serial.print("  | SwipeDelta: ");
    Serial.print(delta, 1);

    if (dt <= SWIPE_TIME_MAX &&
        fabs(delta) >= SWIPE_DELTA_MM &&
        now - lastSwipeTime > SWIPE_COOLDOWN_MS) {

      if (delta < 0) {
        sendMediaKey(KEY_MEDIA_NEXT_TRACK, "NEXT TRACK");
        Serial.print("  --> NEXT");
      } else {
        sendMediaKey(KEY_MEDIA_PREVIOUS_TRACK, "PREVIOUS TRACK");
        Serial.print("  --> PREVIOUS");
      }

      lastSwipeTime = now;
      swipeArmed = false;  // must exit zone and re-enter for next swipe
      return;
    }

    // If movement slow, reset baseline for next sample
    if (dt > SWIPE_TIME_MAX) {
      swipeBaseDist = distanceFiltered;
      swipeBaseTime = now;
    }
  }
}



// -------------------- Main gesture update --------------------
void updateDistanceAndGestures(unsigned long now) {
  // 1) Read distance
  uint16_t d = lox.readRangeSingleMillimeters();
  if (lox.timeoutOccurred()) {
    Serial.print("[TOF TIMEOUT]");
    return;  // keep previous distanceFiltered
  }

  if (d == 0 || d > 2000) {
    distanceRaw = -1;
  } else {
    distanceRaw = d;
  }

  // 2) Filter
  lastDistanceFiltered = distanceFiltered;
  if (distanceRaw < 0) {
    distanceFiltered = -1;
  } else if (distanceFiltered < 0) {
    distanceFiltered = distanceRaw;
  } else {
    distanceFiltered = distanceFiltered + FILTER_ALPHA * (distanceRaw - distanceFiltered);
  }

  Serial.print("VL53 Raw: ");
  Serial.print(distanceRaw);
  Serial.print(" mm  | Filtered: ");
  Serial.print(distanceFiltered, 1);

  // 3) Hand present?
  bool handPresent = (distanceFiltered > 0 &&
                      distanceFiltered >= MIN_HAND_DIST &&
                      distanceFiltered <= MAX_HAND_DIST);

  Serial.print("  | HandPresent: ");
  Serial.print(handPresent ? "YES" : "NO");
    // Track last time a hand was seen (for idle/active switching)
  if (handPresent) {
    lastHandSeenTime = now;

    // If we were idle and hand appears, go back to ACTIVE
    if (powerMode == MODE_IDLE) {
      powerMode = MODE_ACTIVE;
      currentLoopDelay = MAIN_LOOP_DELAY;  // back to fast polling
      Serial.print("  | PowerMode -> ACTIVE");
    }
  }


  bool inNearZone = false;
  bool inMidZone  = false;
  bool inFarZone  = false;

  if (handPresent) {
    if (distanceFiltered <= NEAR_MAX) {
      inNearZone = true;
    } else if (distanceFiltered >= MID_MIN && distanceFiltered <= MID_MAX) {
      inMidZone = true;
    } else if (distanceFiltered >= FAR_MIN) {
      inFarZone = true;
    }
  }

  Serial.print("  | Zone:");
  if (inNearZone)      Serial.print("NEAR");
  else if (inMidZone)  Serial.print("MID");
  else if (inFarZone)  Serial.print("FAR");
  else                 Serial.print("NONE");

  // 4) Run handlers
  handleVolume(now, inNearZone);
  handlePlayPause(now, inMidZone);
  handleSkip(now, inFarZone);

  Serial.println();
    // -------------------- Power mode management --------------------
  if (!handPresent && powerMode == MODE_ACTIVE) {
    // If no hand for a long time, go to IDLE
    if (now - lastHandSeenTime > IDLE_TIMEOUT_MS) {
      powerMode = MODE_IDLE;
      currentLoopDelay = 350;   // ms, much slower polling in idle
      Serial.print("  | PowerMode -> IDLE");
    }
  }

}

// -------------------- Setup --------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("=== Gesture Speaker (Volume swipe+hold / Play-Pause / Skip) ===");

  Wire.begin(21, 22); // SDA, SCL
  delay(200);

  Serial.println("I2C scan:");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print("  Device at 0x");
      Serial.println(addr, HEX);
      delay(2);
    }
  }

  Serial.println("Initializing VL53L0X...");
  bool ok = false;
  for (int i = 0; i < 5 && !ok; i++) {
    if (lox.init()) {
      ok = true;
      Serial.print("VL53L0X: Init OK (try ");
      Serial.print(i + 1);
      Serial.println(")");
    } else {
      Serial.print("VL53L0X: Init FAILED (try ");
      Serial.print(i + 1);
      Serial.println("), retrying...");
      delay(200);
    }
  }

  if (!ok) {
    Serial.println("VL53L0X: Init FAILED after retries, continuing anyway.");
    // we don't block here; loop() will still run and show TIMEOUT etc.
  }

  lox.setTimeout(500);
  Serial.println("VL53L0X: Timeout set");

  bleKeyboard.begin();
  Serial.println("BLE Keyboard started. Pair this device as 'Gesture Speaker'.");

  lastVLReadTime = millis();
}


// -------------------- Loop --------------------
void loop() {
  unsigned long now = millis();

  if (now - lastVLReadTime >=  currentLoopDelay) {
    updateDistanceAndGestures(now);
    lastVLReadTime = now;
  }

  delay(5);
}
