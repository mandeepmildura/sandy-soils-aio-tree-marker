// ================================================================
//  Ethernet OTA: browse to http://<board IP>/ , pick a .hex, Upload
// ================================================================
// The .hex is POSTed raw to /update and parsed line by line straight
// into spare flash (FlashTxx.c, vendored from FlasherX). Every record is
// checksum-checked; the image must start at the flash base, fit in the
// buffer and contain "fw_teensy41" (FlashTxx.c puts that string in every
// build that has this OTA module, so a firmware without OTA is refused
// and you can't lock yourself out of OTA). Only then is it copied over
// the running firmware and the board reboots. Any failure erases the
// buffer and the old firmware keeps running.
//
// Refused while autosteer is engaged. The upload blocks loop() for its
// duration (a few seconds); the tree timer is stopped and the valve
// output forced off first.
// ponytail: blocking upload, a non-blocking state machine only if OTA
// ever has to run while steering.

extern "C" {
#include "FlashTxx.h"
}

// Tree-marker build version. Bump it, then tag aio-v<same> to get a CI release.
#define AIO_TREE_VERSION "1.1.1"

EthernetServer otaServer(80);

// One-click update: the browser (which has internet; the Teensy doesn't)
// reads version.txt + aio_v4_tree.hex from the repo's "ota" branch on
// raw.githubusercontent.com (CORS-open, unlike release downloads; CI pushes
// that branch on every v* tag) and POSTs the hex to /update like a picked file.
#define OTA_RAW "https://raw.githubusercontent.com/mandeepmildura/sandy-soils-aio-tree-marker/ota/"

static const char OTA_PAGE[] =
  "<!doctype html><meta name=viewport content='width=device-width'>"
  "<title>AiO OTA</title><body style='font-family:sans-serif;max-width:32em;margin:2em auto'>"
  "<h3>AiO v4 tree marker: firmware update</h3>"
  "<p>Running: <b>v" AIO_TREE_VERSION "</b> (built " __DATE__ " " __TIME__ ")</p>"
  "<p>Latest on GitHub: <b id=lv>checking...</b> <button id=ub onclick=latest() disabled>Update to latest</button></p>"
  "<p>Or pick a <b>.hex</b> file (<a href='https://github.com/mandeepmildura/sandy-soils-aio-tree-marker/releases' target=_blank>releases</a>):<br>"
  "<input type=file id=f accept='.hex'> <button onclick=pick()>Upload</button></p>"
  "<p style='color:#a40'>Autosteer must be off.</p>"
  "<p><progress id=p max=100 value=0 style='width:100%'></progress></p><p id=s></p>"
  "<script>var R='" OTA_RAW "',C='" AIO_TREE_VERSION "',$=function(i){return document.getElementById(i)};"
  "function send(b){var x=new XMLHttpRequest();"
  "x.upload.onprogress=function(e){$('p').value=e.loaded*100/e.total;"
  "$('s').textContent='Uploading '+Math.round(e.loaded/1024)+' of '+Math.round(e.total/1024)+' KB'};"
  "x.onload=function(){$('s').textContent=x.responseText};"
  "x.onerror=function(){$('s').textContent='Connection lost. If it said rebooting, refresh in 10 s.'};"
  "x.open('POST','/update');x.send(b);$('s').textContent='Uploading...'}"
  "function pick(){var f=$('f').files[0];if(f)send(f)}"
  "function latest(){$('s').textContent='Downloading from GitHub...';"
  "fetch(R+'aio_v4_tree.hex?'+Date.now()).then(function(r){if(!r.ok)throw r.status;return r.blob()})"
  ".then(send).catch(function(e){$('s').textContent='Download failed ('+e+'). Pick the file instead.'})}"
  "fetch(R+'version.txt?'+Date.now()).then(function(r){if(!r.ok)throw r.status;return r.text()})"
  ".then(function(v){v=v.trim();$('lv').textContent='v'+v+(v==C?' (you have it)':'');$('ub').disabled=false})"
  ".catch(function(){$('lv').textContent='no internet - pick a file below'})"
  "</script>";

void otaSetup() {
  otaServer.begin();
  Serial.println("OTA: http server on port 80, tree marker v" AIO_TREE_VERSION);
}

// next byte from the client, -1 on 5 s stall or disconnect
static int otaGetc(EthernetClient& c) {
  uint32_t t0 = millis();
  while (!c.available()) {
    if (!c.connected() || millis() - t0 > 5000) return -1;
  }
  return c.read();
}

// one line without CR/LF; false on timeout or overflow
static bool otaReadLine(EthernetClient& c, char* buf, int max, long* used) {
  int n = 0;
  for (;;) {
    int ch = otaGetc(c);
    if (ch < 0) return false;
    if (used) (*used)++;
    if (ch == '\n') break;
    if (ch == '\r') continue;
    if (n >= max - 1) return false;
    buf[n++] = (char)ch;
  }
  buf[n] = 0;
  return true;
}

static int otaHex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static int otaByte(const char* p) {
  int h = otaHex(p[0]), l = otaHex(p[1]);
  return (h < 0 || l < 0) ? -1 : (h << 4) | l;
}

// Parse ":LLAAAATT<data>CC". Returns data length, -1 if malformed/bad checksum.
static int otaParseRecord(const char* line, uint8_t* data, uint32_t* addr, uint8_t* type) {
  if (line[0] != ':') return -1;
  int len = otaByte(line + 1);
  if (len < 0 || (int)strlen(line) < 11 + 2 * len) return -1;
  int a1 = otaByte(line + 3), a2 = otaByte(line + 5), t = otaByte(line + 7);
  if (a1 < 0 || a2 < 0 || t < 0) return -1;
  uint8_t sum = len + a1 + a2 + t;
  for (int i = 0; i < len; i++) {
    int b = otaByte(line + 9 + 2 * i);
    if (b < 0) return -1;
    data[i] = b; sum += b;
  }
  int ck = otaByte(line + 9 + 2 * len);
  if (ck < 0 || (uint8_t)(sum + ck) != 0) return -1;
  *addr = (a1 << 8) | a2; *type = t;
  return len;
}

static void otaReply(EthernetClient& c, int code, const char* type, const char* body) {
  c.printf("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
           code, code == 200 ? "OK" : "Error", type, (unsigned)strlen(body));
  c.write((const uint8_t*)body, strlen(body));
  c.flush();
}

static void otaStopTree() {
  treeTimer.end();
  tFireArmed = false; tPulseOn = false;
  if (treeOwnsCytron) digitalWriteFast(PWM1_LPWM, LOW);
}

// Stream the hex into the flash buffer. Returns nullptr on success
// (with *imageSize set), else an error message.
static const char* otaReceive(EthernetClient& c, long contentLen, uint32_t bufAddr,
                              uint32_t bufSize, uint32_t* imageSize) {
  static char line[600];
  uint8_t data[256] __attribute__((aligned(4)));
  uint32_t base = 0, maxAddr = 0, minAddr = 0xFFFFFFFF;
  long used = 0;
  uint32_t lines = 0;
  for (;;) {
    if (used >= contentLen) return "upload ended before the hex EOF record";
    if (!otaReadLine(c, line, sizeof(line), &used)) return "upload stalled or line too long";
    if (!line[0]) continue;
    uint32_t addr; uint8_t type;
    int n = otaParseRecord(line, data, &addr, &type);
    if (n < 0) return "bad hex line (checksum or format) - is this a .hex file?";
    lines++;
    if (type == 1) break;                                     // EOF
    if (type == 4 && n == 2) { base = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16); continue; }
    if (type == 2 && n == 2) { base = (((uint32_t)data[0] << 8) | data[1]) << 4; continue; }
    if (type == 5 || type == 3) continue;                     // start address, not needed
    if (type != 0) return "unknown hex record type";
    uint32_t a = base + addr;
    if (a < FLASH_BASE_ADDR || a + n > FLASH_BASE_ADDR + bufSize)
      return "image does not fit in the flash buffer (wrong board?)";
    if (flash_write_block(bufAddr + (a - FLASH_BASE_ADDR), (char*)data, n))
      return "flash write failed";
    if (a < minAddr) minAddr = a;
    if (a + n > maxAddr) maxAddr = a + n;
  }
  if (minAddr != FLASH_BASE_ADDR) return "image does not start at the flash base";
  *imageSize = maxAddr - FLASH_BASE_ADDR;
  if (!check_flash_id(bufAddr, *imageSize))
    return "image is not a Teensy 4.1 build with OTA (missing fw_teensy41) - refused";
  Serial.printf("OTA: %lu hex lines, %lu bytes OK\r\n", lines, *imageSize);
  return nullptr;
}

static void otaUpdate(EthernetClient& c, long contentLen) {
  if (watchdogTimer < WATCHDOG_THRESHOLD) {
    otaReply(c, 409, "text/plain", "Refused: autosteer is engaged. Turn it off and retry.");
    return;
  }
  if (contentLen <= 0) { otaReply(c, 411, "text/plain", "No Content-Length."); return; }

  uint32_t bufAddr, bufSize;
  if (firmware_buffer_init(&bufAddr, &bufSize) != FLASH_BUFFER_TYPE) {
    otaReply(c, 500, "text/plain", "No flash buffer available.");
    return;
  }
  Serial.printf("OTA: receiving %ld bytes, buffer %08lX size %lu\r\n", contentLen, bufAddr, bufSize);
  otaStopTree();

  uint32_t imageSize = 0;
  const char* err = otaReceive(c, contentLen, bufAddr, bufSize, &imageSize);
  if (err) {
    Serial.print("OTA: failed: "); Serial.println(err);
    firmware_buffer_free(bufAddr, bufSize);                   // erase the half-written buffer
    otaReply(c, 400, "text/plain", err);
    treeTimer.begin(treeTick, 100);                           // back to normal
    return;
  }

  otaReply(c, 200, "text/plain", "OK - verified. Flashing and rebooting; refresh in 10 s.");
  delay(200);                                                 // let the reply leave
  c.stop();
  Serial.println("OTA: flash_move + reboot");
  Serial.flush();
  // The core's flash_wait() re-enables interrupts after every flash write,
  // so mask every IRQ source at the NVIC and stop SysTick: no ISR (some
  // live in flash, e.g. FNET's) may run while the program is rewritten.
  __disable_irq();
  for (int i = 0; i < 8; i++) (&NVIC_ICER0)[i] = 0xFFFFFFFF;
  SYST_CSR = 0;
  flash_move(FLASH_BASE_ADDR, bufAddr, imageSize);            // does not return
}

void otaLoop() {
  EthernetClient c = otaServer.available();
  if (!c) return;
  char line[200];
  long contentLen = -1;
  if (!otaReadLine(c, line, sizeof(line), nullptr)) { c.stop(); return; }
  bool post = strncmp(line, "POST /update", 12) == 0;
  char hdr[512];
  while (otaReadLine(c, hdr, sizeof(hdr), nullptr) && hdr[0]) {
    if (strncasecmp(hdr, "Content-Length:", 15) == 0) contentLen = atol(hdr + 15);
  }
  if (post) otaUpdate(c, contentLen);
  else      otaReply(c, 200, "text/html", OTA_PAGE);
  c.stop();
}
