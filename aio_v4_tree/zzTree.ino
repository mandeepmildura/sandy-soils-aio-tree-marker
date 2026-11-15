// ================================================================
//  Tree marker module for the AiO v4.5 (Keya CAN steering)
// ================================================================
// Fires the spray valve through the onboard Cytron (pins PWM1=2, DIR=4)
// at each tree, predicted from the F9P GGA fixes this board already
// parses. Trees and settings come from a Surface app over UDP port 8890;
// replies (acks, hits, status) go back to the sender on port 8891.
//
// Safety: the Cytron is only taken over when the Surface sets the
// "own Cytron" flag (meant for Keya CAN steering, where the Cytron is
// unused). Until then this module is a dry run: it computes and reports
// hits but drives no pins. Giving the Cytron back to steering needs a
// reboot.
//
// Packets: 'T','M',<type>,payload (little-endian)
//   'S' settings: float foreAft(m,+behind antenna), float lateral(m,+right),
//       float hitRadius(m), u16 pulseMs, u16 lagMs, u8 flags
//       (bit0 enabled, bit1 own Cytron, bit2 DIR pin high)
//   'O' origin:   double lat0, double lon0           (clears trees)
//   'T' trees:    u16 start, u16 count, count x (float e, float n) metres
//                 east/north of origin: n=(lat-lat0)*TREE_M_PER_DEG,
//                 e=(lon-lon0)*TREE_M_PER_DEG*cos(lat0)
//   'F' test fire        'P' sim position: double lat, double lon
//   'Q' status request   'G' raw GPS dump (reply 'G' + last 96 bytes from Serial7)
// Replies: 'A' u8 type,u32 value | 'H' u32 idx,float miss,u32 ms |
//          'Z' status (see treeSendStatus)

#define TREE_PORT_IN   8890
#define TREE_PORT_OUT  8891
#define TREE_MAX       20000
#define TREE_M_PER_DEG 111319.49
#define TREE_VEL_N     4

EthernetUDP treeUdp;
IPAddress   treeHostIP;
bool        treeHostKnown = false;

DMAMEM float treeE[TREE_MAX];
DMAMEM float treeN[TREE_MAX];
uint32_t treeCount = 0;
double   treeLat0 = 0, treeLon0 = 0, treeMPerDegLon = TREE_M_PER_DEG;
bool     treeHasOrigin = false;

// settings
float    treeForeAft = 0, treeLateral = 0, treeHitR = 0.35f;
uint16_t treePulseMs = 60, treeLagMs = 0;
bool     treeEnabled = false, treeDirHigh = true;

// motion state
double   tVelE = 0, tVelN = 0, tFwdE = 0, tFwdN = 1;   // m/s, unit forward
double   tHistE[TREE_VEL_N], tHistN[TREE_VEL_N];
uint32_t tHistUs[TREE_VEL_N];
int      tHistCount = 0, tHistHead = 0;
uint32_t tLastFixUs = 0, tLastFixMs = 0;
uint8_t  tFixQuality = 0;
double   tLastLat = 0, tLastLon = 0;

// approach / firing
int32_t  tApproachIdx = -1;
float    tApproachMinD = 1e9f;
bool     tApproachFired = false;
volatile bool     tFireArmed = false, tPulseOn = false;
volatile uint32_t tFireAtUs = 0, tOffAtUs = 0;
volatile int32_t  tFireIdx = -1;
volatile float    tFireMiss = 0;
volatile bool     tHitPending = false;
volatile int32_t  tHitIdx = -1;
volatile float    tHitMiss = 0;
volatile uint32_t tHitMs = 0;
uint32_t tTotalHits = 0;
volatile uint32_t tFireCount = 0;               // every output pulse incl. tests (web page flash)
IntervalTimer treeTimer;
elapsedMillis treeStatusTimer;

// ── output (runs in the 100 us timer ISR) ────────────────────
static inline void treeOutput(bool on) {
  if (!treeOwnsCytron) return;                 // dry run
  digitalWriteFast(PWM1_LPWM, on ? HIGH : LOW);
}

void treeTick() {
  uint32_t now = micros();
  if (tPulseOn && (int32_t)(now - tOffAtUs) >= 0) {
    treeOutput(false);
    tPulseOn = false;
  }
  if (tFireArmed && (int32_t)(now - tFireAtUs) >= 0) {
    tFireArmed = false;
    if (!tPulseOn) {
      treeOutput(true);
      tPulseOn = true;
      tFireCount++;
      tOffAtUs = now + (uint32_t)treePulseMs * 1000u;
      tHitIdx = tFireIdx; tHitMiss = tFireMiss; tHitMs = millis();
      tHitPending = true;
    }
  }
}

// Schedule a fire `inUs` from now (0 = immediately).
void treeSchedule(int32_t idx, float miss, uint32_t inUs) {
  noInterrupts();
  tFireIdx = idx; tFireMiss = miss;
  tFireAtUs = micros() + inUs;
  tFireArmed = true;
  interrupts();
}

// ── pure prediction math (same as the ESP32 firmware) ────────
static inline void treePredict(double rE, double rN, double vE, double vN,
                               double& tStar, double& dMin) {
  double v2 = vE*vE + vN*vN;
  tStar = -(rE*vE + rN*vN) / v2;
  double cE = rE + vE*tStar, cN = rN + vN*tStar;
  dMin = sqrt(cE*cE + cN*cN);
}

void treeTakeCytron() {
  if (treeOwnsCytron) return;
  treeOwnsCytron = true;                      // motorDrive() now leaves pins 2/4 alone
  pinMode(PWM1_LPWM, OUTPUT);
  digitalWriteFast(PWM1_LPWM, LOW);
  pinMode(DIR1_RL_ENABLE, OUTPUT);
  digitalWrite(DIR1_RL_ENABLE, treeDirHigh ? HIGH : LOW);
  Serial.println("TREE: Cytron taken for spray valve (reboot to return it to steering)");
}

// ── per-fix processing ───────────────────────────────────────
void treeOnPosition(double lat, double lon, uint32_t fixUs) {
  tLastLat = lat; tLastLon = lon;
  tLastFixUs = fixUs; tLastFixMs = millis();
  if (!treeHasOrigin) return;

  double e = (lon - treeLon0) * treeMPerDegLon;
  double n = (lat - treeLat0) * TREE_M_PER_DEG;

  // velocity over the last few fixes (UART arrival times are steady)
  if (tHistCount && (fixUs - tHistUs[(tHistHead + TREE_VEL_N - 1) % TREE_VEL_N]) > 600000u)
    tHistCount = 0;                            // gap: restart
  tHistE[tHistHead] = e; tHistN[tHistHead] = n; tHistUs[tHistHead] = fixUs;
  tHistHead = (tHistHead + 1) % TREE_VEL_N;
  if (tHistCount < TREE_VEL_N) tHistCount++;
  if (tHistCount >= 2) {
    int newest = (tHistHead + TREE_VEL_N - 1) % TREE_VEL_N;
    int oldest = (tHistHead + TREE_VEL_N - tHistCount) % TREE_VEL_N;
    double dt = (tHistUs[newest] - tHistUs[oldest]) / 1e6;
    if (dt > 0.02) {
      tVelE = (tHistE[newest] - tHistE[oldest]) / dt;
      tVelN = (tHistN[newest] - tHistN[oldest]) / dt;
    }
  } else { tVelE = tVelN = 0; }
  double spd = sqrt(tVelE*tVelE + tVelN*tVelN);
  if (spd > 0.2) { tFwdE = tVelE / spd; tFwdN = tVelN / spd; }   // keep last heading when slow

  // antenna -> nozzle (+foreAft = behind, +lateral = right)
  double nozE = e - treeForeAft * tFwdE + treeLateral * tFwdN;
  double nozN = n - treeForeAft * tFwdN - treeLateral * tFwdE;

  if (!treeEnabled || treeCount == 0) return;

  // nearest tree within search range
  const double range = treeHitR * 2.5 + 0.5;
  int32_t nearIdx = -1; double nearD = 1e9;
  for (uint32_t i = 0; i < treeCount; i++) {
    double de = nozE - treeE[i], dn = nozN - treeN[i];
    if (fabs(de) > range || fabs(dn) > range) continue;
    double d = sqrt(de*de + dn*dn);
    if (d < range && d < nearD) { nearD = d; nearIdx = (int32_t)i; }
  }
  if (nearIdx < 0) { tApproachIdx = -1; tApproachMinD = 1e9f; tApproachFired = false; return; }
  if (nearIdx != tApproachIdx) { tApproachIdx = nearIdx; tApproachMinD = nearD; tApproachFired = false; }
  if (tApproachFired) return;

  if (spd >= 0.2) {
    double tStar, dMin;
    treePredict(nozE - treeE[nearIdx], nozN - treeN[nearIdx], tVelE, tVelN, tStar, dMin);
    if (dMin > treeHitR) return;
    // tStar is measured from the fix time; fire at fixTime + tStar - lag
    double fireAt = tStar - treeLagMs / 1000.0;
    double sinceFix = (micros() - fixUs) / 1e6;
    double inS = fireAt - sinceFix;
    if (inS <= 0) {
      if (tStar > -0.5) { tApproachFired = true; treeSchedule(nearIdx, (float)dMin, 0); }
    } else if (inS < 0.15) {                   // due before the next 10 Hz fix
      tApproachFired = true;
      treeSchedule(nearIdx, (float)dMin, (uint32_t)(inS * 1e6));
    }
    return;
  }

  // slow / stopped: closest-approach rule
  if (nearD < tApproachMinD - 0.005) { tApproachMinD = nearD; return; }
  if (nearD > tApproachMinD + 0.01 && tApproachMinD <= treeHitR) {
    tApproachFired = true;
    treeSchedule(nearIdx, tApproachMinD, 0);
  }
}

// Called from GGA_Handler with the raw NMEA fields.
void treeOnGGA(const char* latS, const char* ns, const char* lonS, const char* ew, const char* fixQ) {
  uint32_t now = micros();
  treeGgaCount++;
  tFixQuality = (uint8_t)atoi(fixQ);
  if (tFixQuality == 0 || latS[0] == 0 || lonS[0] == 0) return;
  double la = atof(latS), lo = atof(lonS);
  double lat = floor(la / 100.0) + fmod(la, 100.0) / 60.0;
  double lon = floor(lo / 100.0) + fmod(lo, 100.0) / 60.0;
  if (ns[0] == 'S') lat = -lat;
  if (ew[0] == 'W') lon = -lon;
  treeOnPosition(lat, lon, now);
}

// ── UDP ──────────────────────────────────────────────────────
void treeSend(const uint8_t* buf, size_t len) {
  if (!treeHostKnown) return;
  treeUdp.beginPacket(treeHostIP, TREE_PORT_OUT);
  treeUdp.write(buf, len);
  treeUdp.endPacket();
}

void treeAck(char type, uint32_t value) {
  uint8_t b[8] = { 'T', 'M', 'A', (uint8_t)type };
  memcpy(b + 4, &value, 4);
  treeSend(b, sizeof(b));
}

void treeSendStatus() {
  uint8_t b[64] = { 'T', 'M', 'Z' };
  float spd = (float)sqrt(tVelE*tVelE + tVelN*tVelN);
  uint32_t age = tLastFixMs ? millis() - tLastFixMs : 0xFFFFFFFFu;
  uint8_t flags = (treeEnabled ? 1 : 0) | (treeOwnsCytron ? 2 : 0) | (treeHasOrigin ? 4 : 0) | (keyaDetected ? 8 : 0);
  size_t o = 3;
  b[o++] = tFixQuality; b[o++] = flags;
  memcpy(b + o, &treeCount, 4); o += 4;
  memcpy(b + o, &tTotalHits, 4); o += 4;
  memcpy(b + o, &spd, 4); o += 4;
  memcpy(b + o, &age, 4); o += 4;
  memcpy(b + o, &tLastLat, 8); o += 8;
  memcpy(b + o, &tLastLon, 8); o += 8;
  memcpy(b + o, &treeGpsBytes, 4); o += 4;    // bytes from the main F9P (Serial7)
  memcpy(b + o, &treeGps2Bytes, 4); o += 4;   // bytes from the heading F9P (Serial2)
  memcpy(b + o, &treeGgaCount, 4); o += 4;    // GGA sentences parsed
  treeSend(b, o);
}

void treeHandlePacket(const uint8_t* p, int len) {
  if (len < 3 || p[0] != 'T' || p[1] != 'M') return;
  switch (p[2]) {
    case 'S':
      if (len >= 20) {
        memcpy(&treeForeAft, p + 3, 4); memcpy(&treeLateral, p + 7, 4);
        memcpy(&treeHitR, p + 11, 4);
        memcpy(&treePulseMs, p + 15, 2); memcpy(&treeLagMs, p + 17, 2);
        uint8_t f = p[19];
        treeEnabled = f & 1; treeDirHigh = f & 4;
        if (treePulseMs < 5) treePulseMs = 5;
        if (treePulseMs > 5000) treePulseMs = 5000;
        if (treeLagMs > 1000) treeLagMs = 1000;
        if (treeHitR < 0.02f || treeHitR > 5.0f) treeHitR = 0.35f;
        if (f & 2) treeTakeCytron();
        if (treeOwnsCytron) digitalWrite(DIR1_RL_ENABLE, treeDirHigh ? HIGH : LOW);
        treeAck('S', f);
      }
      break;
    case 'O':
      if (len >= 19) {
        memcpy(&treeLat0, p + 3, 8); memcpy(&treeLon0, p + 11, 8);
        treeMPerDegLon = TREE_M_PER_DEG * cos(treeLat0 * M_PI / 180.0);
        treeCount = 0; treeHasOrigin = true;
        tApproachIdx = -1; tApproachFired = false; tHistCount = 0;
        treeAck('O', 0);
      }
      break;
    case 'T':
      if (len >= 7) {
        uint16_t start, count;
        memcpy(&start, p + 3, 2); memcpy(&count, p + 5, 2);
        if (7 + (int)count * 8 > len || (uint32_t)start + count > TREE_MAX) { treeAck('E', start); break; }
        for (uint16_t i = 0; i < count; i++) {
          memcpy(&treeE[start + i], p + 7 + i * 8, 4);
          memcpy(&treeN[start + i], p + 11 + i * 8, 4);
        }
        if ((uint32_t)start + count > treeCount) treeCount = start + count;
        treeAck('T', treeCount);
      }
      break;
    case 'F':
      treeSchedule(-1, 0, 0);
      treeAck('F', 0);
      break;
    case 'P':
      if (len >= 19) {
        double la, lo;
        memcpy(&la, p + 3, 8); memcpy(&lo, p + 11, 8);
        tFixQuality = 9;                       // 9 = simulated
        treeOnPosition(la, lo, micros());
      }
      break;
    case 'Q':
      treeSendStatus();
      break;
    case 'G': {                                 // last 96 raw bytes from the main F9P, oldest first
      uint8_t b[3 + sizeof(treeGpsRaw)] = { 'T', 'M', 'G' };
      for (size_t i = 0; i < sizeof(treeGpsRaw); i++)
        b[3 + i] = treeGpsRaw[(treeGpsRawHead + i) % sizeof(treeGpsRaw)];
      treeSend(b, sizeof(b));
      break;
    }
  }
}

// -- settings saved in EEPROM (web Settings page) ------------
// AgOpenGPS uses EEPROM 0..~70 (ident, steerSettings, steerConfig, network);
// the tree marker keeps its own block well clear of that.
#define TREE_EE_ADDR  1024
#define TREE_EE_MAGIC 0x54524531u                // "TRE1"
struct TreeCfg { uint32_t magic; float foreAft, lateral, hitR; uint16_t pulseMs, lagMs; uint8_t flags; };

void treeSaveCfg(bool wantCytron) {
  TreeCfg c = { TREE_EE_MAGIC, treeForeAft, treeLateral, treeHitR, treePulseMs, treeLagMs,
                (uint8_t)((treeEnabled ? 1 : 0) | (wantCytron ? 2 : 0) | (treeDirHigh ? 4 : 0)) };
  EEPROM.put(TREE_EE_ADDR, c);
}

void treeLoadCfg() {
  TreeCfg c;
  EEPROM.get(TREE_EE_ADDR, c);
  if (c.magic != TREE_EE_MAGIC) { Serial.println("TREE: no saved settings, using defaults"); return; }
  treeForeAft = c.foreAft; treeLateral = c.lateral; treeHitR = c.hitR;
  treePulseMs = c.pulseMs; treeLagMs = c.lagMs;
  treeEnabled = c.flags & 1; treeDirHigh = c.flags & 4;
  if (c.flags & 2) treeTakeCytron();
  Serial.printf("TREE: settings loaded (pulse %u ms, lag %u ms, radius %.2f m, Cytron %s)\r\n",
                treePulseMs, treeLagMs, treeHitR, (c.flags & 2) ? "on" : "off");
}

// Multimeter test: output on for ms (the 100 us timer switches it off).
// Only when the Cytron is owned by the tree marker.
bool treeTestHold(uint32_t ms) {
  if (!treeOwnsCytron) return false;
  noInterrupts();
  tFireArmed = false;
  digitalWriteFast(PWM1_LPWM, HIGH);
  tPulseOn = true; tFireCount++;
  tOffAtUs = micros() + ms * 1000u;
  interrupts();
  return true;
}

void treeSetDir(bool high) {
  treeDirHigh = high;
  if (treeOwnsCytron) digitalWrite(DIR1_RL_ENABLE, high ? HIGH : LOW);
}

// ── setup / loop hooks ───────────────────────────────────────
void treeSetup() {
  treeLoadCfg();
  if (treeUdp.begin(TREE_PORT_IN)) {
    Serial.print("TREE: listening on UDP "); Serial.println(TREE_PORT_IN);
  }
  treeTimer.begin(treeTick, 100);              // 100 us fire/pulse resolution
}

void treeLoop() {
  static uint8_t buf[1400];
  int len = treeUdp.parsePacket();
  if (len > 0) {
    treeHostIP = treeUdp.remoteIP(); treeHostKnown = true;
    if (len > (int)sizeof(buf)) len = sizeof(buf);
    treeUdp.read(buf, len);
    treeHandlePacket(buf, len);
  }
  if (tHitPending) {
    noInterrupts();
    int32_t idx = tHitIdx; float miss = tHitMiss; uint32_t ms = tHitMs;
    tHitPending = false;
    interrupts();
    if (idx >= 0) tTotalHits++;
    uint8_t b[15] = { 'T', 'M', 'H' };
    memcpy(b + 3, &idx, 4); memcpy(b + 7, &miss, 4); memcpy(b + 11, &ms, 4);
    treeSend(b, sizeof(b));
  }
  if (treeStatusTimer > 1000) { treeStatusTimer = 0; treeSendStatus(); }
}
