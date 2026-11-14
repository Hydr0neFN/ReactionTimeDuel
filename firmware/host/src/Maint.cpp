#include "Maint.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <SPIFFS.h>
#include <Update.h>
#include <MD5Builder.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "RtdNetConfig.h"

/* Before V4.0.2 the joystick image was a SPIFFS file. SPIFFS is about 60 %
 * full with the sounds and then lost a part of a 318 KB image. */
static const char* OLD_IMAGE_PATH = "/fw_joystick.bin";
static const char* OLD_IMAGE_VER_PATH = "/fw_joystick.ver";

/*
 * The joystick image is stored raw in the OTA slot that does not run, from
 * IMAGE_OFFSET on. The first sector stays erased, so the bootloader never
 * takes the slot for a host image. NVS holds version, size and MD5.
 * A host update writes the same slot: store the joystick image again after it.
 */
static const uint32_t IMAGE_OFFSET = 0x1000;
static const uint32_t FLASH_SECTOR = 0x1000;
static const char* IMAGE_NVS = "rtdimg";

static const esp_partition_t* imageSlot() {
  return esp_ota_get_next_update_partition(nullptr);
}

/* Kind prefix of RTD_FW_MARKER(); the version part is accepted from any release. */
static const char* HOST_MARKER = "RTDFW|" "host" "|";
static const char* STICK_MARKER = "RTDFW|" "joystick" "|";

static const uint32_t DISPLAY_TIMEOUT_MS = 3000;

/* JSON numbers without a temporary String (no heap churn while polling). */
static void appendNum(String& out, long value) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%ld", value);
  out += buf;
}

static const char* DEVICE_LABELS[DBG_DEVICE_COUNT] = {"STICK1", "STICK2", "STICK3",
                                                      "STICK4", "DISPLAY"};

static String versionText(uint8_t verByte, uint8_t patch) {
  if (verByte == 0) return String("--");
  char buf[12];
  snprintf(buf, sizeof(buf), "%u.%u.%u", (unsigned)((verByte >> 4) & 0x0F),
           (unsigned)(verByte & 0x0F), (unsigned)patch);
  return String(buf);
}

static bool parseVersion(const char* text, uint8_t& verByte, uint8_t& patch) {
  int major = 0, minor = 0, fix = 0;
  if (sscanf(text, "%d.%d.%d", &major, &minor, &fix) != 3) return false;
  if (major < 0 || major > 15 || minor < 0 || minor > 15) return false;
  if (fix < 0 || fix > 255) return false;
  verByte = (uint8_t)((major << 4) | minor);
  patch = (uint8_t)fix;
  return true;
}

/* Dark theme of DESIGN.md 7.3, no external resources. */
static const char PAGE_HTML[] PROGMEM = R"PAGE(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RTD Host - Update mode</title>
<style>
body{background:#101418;color:#FFFFFF;font-family:system-ui,Arial,sans-serif;margin:0;padding:20px}
h1{color:#FF9430;font-size:24px;margin:0 0 16px}
h2{font-size:16px;margin:0 0 10px;color:#8A9099;text-transform:uppercase;letter-spacing:.08em}
.panel{background:#1A2026;border-radius:12px;padding:16px;margin-bottom:16px}
.row{display:flex;align-items:center;gap:10px;padding:8px 0;border-bottom:1px solid #242c34}
.row:last-child{border-bottom:0}
.spread{justify-content:space-between}
.muted{color:#8A9099}
.ok{color:#2AAC00}
.bad{color:#E81818}
.dot{display:inline-block;width:12px;height:12px;border-radius:50%}
button{background:#FF9430;color:#101418;border:0;border-radius:10px;padding:12px 16px;font-weight:700;font-size:15px;cursor:pointer}
button.secondary{background:#242c34;color:#FFFFFF}
button:disabled{opacity:.4;cursor:default}
input[type=file]{color:#8A9099;margin:8px 0;display:block}
form{margin:0 0 8px}
</style></head><body>
<h1>REACTION TIME DUEL &mdash; UPDATE MODE</h1>
<p class="muted"><a href="/debug" style="color:#FF9430">Debug page</a></p>

<div class="panel">
<h2>Access point</h2>
<div class="row spread"><span class="muted">Wi-Fi</span><b id="ssid">--</b></div>
<div class="row spread"><span class="muted">Password</span><b id="pass">--</b></div>
<div class="row spread"><span class="muted">Address</span><b id="addr">--</b></div>
</div>

<div class="panel">
<h2>Devices</h2>
<div class="row spread"><span class="muted">Host</span><b id="hostv">--</b></div>
<div id="sticks"></div>
<div class="row spread"><span class="muted">Display</span><b id="disp">--</b></div>
</div>

<div class="panel">
<h2>Host firmware</h2>
<form id="fhost"><input type="file" id="fhostfile" accept=".bin"><button type="submit">Upload host</button></form>
<h2>Joystick firmware</h2>
<div class="row spread"><span class="muted">Stored image</span><b id="img">--</b></div>
<form id="fstick"><input type="file" id="fstickfile" accept=".bin"><button type="submit">Upload joystick</button></form>
<h2>Display firmware</h2>
<form id="fdisp"><input type="file" id="fdispfile" accept=".bin"><button type="submit">Upload display</button></form>
</div>

<div class="panel">
<h2>Over the air</h2>
<div class="row spread"><span class="muted">Stick update</span><b id="otas">--</b></div>
<button id="go">Update joysticks now</button>
<button id="exit" class="secondary">Exit update mode</button>
<p id="msg" class="muted"></p>
</div>

<script>
function setText(id,t){document.getElementById(id).textContent=t;}
function say(t){setText('msg',t);}
async function status(){const r=await fetch('/api/status');return await r.json();}
async function refresh(){
  try{
    const s=await status();
    setText('ssid',s.ap.ssid);setText('pass',s.ap.pass);setText('addr',s.ap.ip);
    setText('hostv',s.host.version+'  (phase '+s.host.phase+')');
    let rows='';
    for(const st of s.sticks){
      rows+='<div class="row spread"><span>'+st.id+'</span>'+
            '<span class="dot" style="background:'+st.color+'"></span>'+
            '<span>'+st.version+'</span>'+
            '<span class="'+(st.online?'ok':'bad')+'">'+(st.online?'ONLINE':'OFFLINE')+'</span></div>';
    }
    document.getElementById('sticks').innerHTML=rows;
    setText('disp',(s.display.online?'ONLINE ':'OFFLINE ')+s.display.version+(s.display.ip!=='--'?'  '+s.display.ip:''));
    setText('img',s.image.present?s.image.version:'no image');
    setText('otas','state '+s.ota.state+' / target '+s.ota.target+' / '+s.ota.progress+' %');
    document.getElementById('go').disabled=!s.image.present;
  }catch(e){}
}
async function upload(url,file){
  const fd=new FormData();fd.append('firmware',file,file.name);
  const r=await fetch(url,{method:'POST',body:fd});
  return await r.text();
}
document.getElementById('fhost').addEventListener('submit',async e=>{
  e.preventDefault();const f=document.getElementById('fhostfile').files[0];if(!f)return;
  say('Uploading host firmware...');say(await upload('/upload/host',f));
});
document.getElementById('fstick').addEventListener('submit',async e=>{
  e.preventDefault();const f=document.getElementById('fstickfile').files[0];if(!f)return;
  say('Uploading joystick firmware...');say(await upload('/upload/joystick',f));refresh();
});
document.getElementById('fdisp').addEventListener('submit',async e=>{
  e.preventDefault();const f=document.getElementById('fdispfile').files[0];if(!f)return;
  const s=await status();
  if(!s.display.ip||s.display.ip==='--'){say('Display is not connected.');return;}
  say('Uploading display firmware...');
  try{const r=await fetch('http://'+s.display.ip+'/update',{method:'POST',body:f});say('Display: '+(r.ok?'ok':'failed'));}
  catch(err){say('Display upload failed.');}
});
document.getElementById('go').addEventListener('click',async()=>{
  const r=await fetch('/ota/sticks',{method:'POST'});say(await r.text());refresh();
});
document.getElementById('exit').addEventListener('click',async()=>{
  await fetch('/maint/exit',{method:'POST'});say('Rebooting into normal mode...');
});
setInterval(refresh,1000);refresh();
</script>
</body></html>)PAGE";

/* Debug page (DESIGN.md 11.2): no external resources, same dark theme. */
static const char DEBUG_PAGE_HTML[] PROGMEM = R"DEBUG(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RTD Host - Debug</title>
<style>
body{background:#101418;color:#FFFFFF;font-family:system-ui,Arial,sans-serif;margin:0;padding:20px}
h1{color:#FF9430;font-size:24px;margin:0 0 8px}
h2{font-size:16px;margin:0 0 10px;color:#8A9099;text-transform:uppercase;letter-spacing:.08em}
a{color:#FF9430}
.panel{background:#1A2026;border-radius:12px;padding:16px;margin-bottom:16px}
.muted{color:#8A9099}
.ok{color:#2AAC00}
.bad{color:#E81818}
button{background:#FF9430;color:#101418;border:0;border-radius:10px;padding:10px 14px;font-weight:700;font-size:14px;cursor:pointer;margin:2px 8px 2px 0}
button.off{background:#242c34;color:#FFFFFF}
button.off.on{background:#FF9430;color:#101418}
pre{background:#0B0E11;border-radius:8px;padding:10px;margin:0;height:280px;overflow:auto;font-size:12px;line-height:1.4;white-space:pre-wrap}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:8px}
.kv{background:#101418;border-radius:8px;padding:8px}
.kv b{display:block;font-size:17px}
.kv span{color:#8A9099;font-size:12px}
table{width:100%;border-collapse:collapse;font-size:13px}
td,th{text-align:left;padding:6px 4px;border-bottom:1px solid #242c34;vertical-align:top}
th{color:#8A9099;font-weight:400}
</style></head><body>
<h1>REACTION TIME DUEL &mdash; DEBUG</h1>
<p class="muted"><a href="/">Update mode page</a></p>

<div class="panel"><h2>Counters</h2><div class="grid" id="counters"></div></div>

<div class="panel"><h2>Devices</h2><div id="devices"></div></div>

<div class="panel"><h2>Switches</h2>
<button id="trace" class="off">TRACE OFF</button>
<button id="wifi" class="off">DEBUG WI-FI OFF</button>
<p id="msg" class="muted"></p>
</div>

<div class="panel"><h2>Event log</h2><pre id="log"></pre></div>

<script>
let lastSeq=0,traceOn=0,wifiOn=0;
function el(t,c){const e=document.createElement(t);if(c)e.className=c;return e;}
function kv(label,value,bad){const d=el('div','kv');const b=el('b',bad?'bad':'');b.textContent=value;
  const s=el('span');s.textContent=label;d.appendChild(b);d.appendChild(s);return d;}
function setCounters(x){
  const c=document.getElementById('counters');c.textContent='';
  c.appendChild(kv('uptime',Math.floor(x.uptime_ms/1000)+' s'));
  c.appendChild(kv('loop max (10 s)',(x.loop_max_us/1000).toFixed(2)+' ms'));
  c.appendChild(kv('heap free / min',x.heap_free+' / '+x.heap_min));
  c.appendChild(kv('reset',x.reset_reason+' '+x.reset_reason_text));
  c.appendChild(kv('rx frames',x.rx_frames));
  c.appendChild(kv('rx ring drops',x.rx_ring_drops,x.rx_ring_drops>0));
  c.appendChild(kv('bad frames',x.bad_frames,x.bad_frames>0));
  c.appendChild(kv('tx frames',x.tx_frames));
  c.appendChild(kv('tx fail S1..S4 / display / all',x.tx_fail.join(' / ')));
  c.appendChild(kv('phase',x.phase_name));
  traceOn=x.trace;wifiOn=x.debug_wifi;
  const t=document.getElementById('trace');
  t.textContent=traceOn?'TRACE ON':'TRACE OFF';t.className=traceOn?'':'off';
  const w=document.getElementById('wifi');
  w.textContent=wifiOn?'DEBUG WI-FI ON':'DEBUG WI-FI OFF';w.className=wifiOn?'':'off';
}
function setDevices(x){
  const c=document.getElementById('devices');c.textContent='';
  const t=el('table');
  const hr=el('tr');
  for(const h of ['Device','Last seen','Frames','Last cmd','DIAG values']){
    const th=el('th');th.textContent=h;hr.appendChild(th);}
  t.appendChild(hr);
  for(const d of x.devices){
    const tr=el('tr');
    const n=el('td');n.textContent=d.name;tr.appendChild(n);
    const a=el('td',d.seen&&d.age_ms<3000?'ok':'bad');
    a.textContent=d.seen?d.age_ms+' ms ago':'never';tr.appendChild(a);
    const f=el('td');f.textContent=d.frames;tr.appendChild(f);
    const lc=el('td');lc.textContent=d.last_cmd_name;tr.appendChild(lc);
    const dv=el('td');
    if(!d.diag.length){dv.textContent='--';}
    else{dv.textContent=d.diag.map(v=>v.name+'='+v.value+' ('+v.age_ms+' ms)').join(', ');}
    tr.appendChild(dv);
    t.appendChild(tr);
  }
  c.appendChild(t);
}
function addLines(text){
  const log=document.getElementById('log');
  for(const line of text.split('\n')){
    if(!line)continue;
    const m=/^#(\d+)/.exec(line);
    if(m)lastSeq=parseInt(m[1],10);
    log.appendChild(document.createTextNode(line+'\n'));
  }
  while(log.childNodes.length>400)log.removeChild(log.firstChild);
  log.scrollTop=log.scrollHeight;
}
let busy=false;
async function refresh(){
  if(busy)return;
  busy=true;
  try{
    const d=await (await fetch('/api/debug')).json();
    setCounters(d);setDevices(d);
    const t=await (await fetch('/api/log?since='+lastSeq)).text();
    if(t)addLines(t);
  }catch(e){}
  busy=false;
}
async function loop(){await refresh();setTimeout(loop,1000);}
document.getElementById('trace').addEventListener('click',async()=>{
  await fetch('/api/debug/trace?on='+(traceOn?0:1),{method:'POST'});refresh();
});
document.getElementById('wifi').addEventListener('click',async()=>{
  const r=await fetch('/api/debug/wifi?on='+(wifiOn?0:1),{method:'POST'});
  try{const j=await r.json();document.getElementById('msg').textContent=j.note||'';}catch(e){}
});
loop();
</script>
</body></html>)DEBUG";

Maint::Maint(HostGame& game, Storage& storage, Debug& debug)
    : game_(game),
      storage_(storage),
      debug_(debug),
      server_(80),
      pump_(nullptr),
      active_(false),
      rebootPending_(false),
      rebootAtMs_(0),
      uploadRejected_(false),
      imageValid_(false),
      imageVer_(0),
      imagePatch_(0),
      imageSize_(0),
      imagePart_(nullptr),
      imageErasedTo_(0),
      hostUploadOk_(false),
      hostUpdateOk_(false),
      hostMarkerFound_(false),
      hostMatch_(0),
      joystickUploadBytes_(0),
      joystickReadback_(0),
      joystickUploadOk_(false),
      joystickMarkerFound_(false),
      joystickMatch_(0),
      joystickVersionLen_(0),
      joystickVersionDone_(false) {
  memset(&display, 0, sizeof(display));
  joystickVersion_[0] = '\0';
}

void Maint::begin() {
  SPIFFS.begin(false);
  /* Free the space of an image file that an older firmware left behind. */
  if (SPIFFS.exists(OLD_IMAGE_PATH)) SPIFFS.remove(OLD_IMAGE_PATH);
  if (SPIFFS.exists(OLD_IMAGE_VER_PATH)) SPIFFS.remove(OLD_IMAGE_VER_PATH);
  refreshImageInfo();
  /* DESIGN.md 11.3: debug Wi-Fi runs the AP and the web page in every phase. */
  if (storage_.debugWifi()) startServer();
}

/* MD5 of the first `size` bytes of the stored image. */
bool Maint::imageMd5(uint32_t size, String& md5, uint32_t* bytesRead) {
  const esp_partition_t* part = imageSlot();
  if (part == nullptr || size == 0 || size > part->size - IMAGE_OFFSET) return false;

  MD5Builder builder;
  builder.begin();
  uint8_t buf[512];
  uint32_t done = 0;
  uint32_t sincePump = 0;
  while (done < size) {
    uint32_t n = size - done;
    if (n > sizeof(buf)) n = sizeof(buf);
    if (esp_partition_read(part, IMAGE_OFFSET + done, buf, n) != ESP_OK) return false;
    builder.add(buf, (uint16_t)n);
    done += n;
    if (bytesRead != nullptr) *bytesRead = done;
    sincePump += n;
    if (sincePump >= 4096) {
      sincePump = 0;
      pump();
    }
  }
  builder.calculate();
  md5 = builder.toString();
  return true;
}

/* The NVS record is valid only when the slot still holds the same bytes:
 * a host update overwrites the slot. */
void Maint::refreshImageInfo() {
  imageValid_ = false;
  imageVer_ = 0;
  imagePatch_ = 0;
  imageSize_ = 0;
  imageMd5_ = "";

  Preferences prefs;
  if (!prefs.begin(IMAGE_NVS, true)) return;
  uint32_t size = prefs.getUInt("size", 0);
  uint8_t ver = prefs.getUChar("ver", 0);
  uint8_t patch = prefs.getUChar("patch", 0);
  String md5 = prefs.getString("md5", "");
  prefs.end();
  if (size == 0 || md5.length() != 32) return;

  String stored;
  if (!imageMd5(size, stored, nullptr) || stored != md5) return;

  imageVer_ = ver;
  imagePatch_ = patch;
  imageSize_ = size;
  imageMd5_ = md5;
  imageValid_ = true;
}

bool Maint::storeImageVersion(uint8_t ver, uint8_t patch, uint32_t size, const String& md5) {
  Preferences prefs;
  if (!prefs.begin(IMAGE_NVS, false)) return false;
  bool ok = prefs.putUInt("size", size) != 0;
  ok = prefs.putUChar("ver", ver) != 0 && ok;
  ok = prefs.putUChar("patch", patch) != 0 && ok;
  ok = prefs.putString("md5", md5) != 0 && ok;
  prefs.end();
  return ok;
}

void Maint::clearImageRecord() {
  Preferences prefs;
  if (prefs.begin(IMAGE_NVS, false)) {
    prefs.clear();
    prefs.end();
  }
  imageValid_ = false;
  imageVer_ = 0;
  imagePatch_ = 0;
  imageSize_ = 0;
  imageMd5_ = "";
}

/* Append a chunk of the upload to the slot; erase each sector before its first use. */
bool Maint::imageWrite(const uint8_t* data, size_t len) {
  const esp_partition_t* part = (const esp_partition_t*)imagePart_;
  if (part == nullptr) return false;
  uint32_t start = IMAGE_OFFSET + joystickUploadBytes_;
  uint32_t end = start + (uint32_t)len;
  if (end > part->size) return false;
  while (imageErasedTo_ < end) {
    if (esp_partition_erase_range(part, imageErasedTo_, FLASH_SECTOR) != ESP_OK) return false;
    imageErasedTo_ += FLASH_SECTOR;
  }
  if (len == 0) return true;
  return esp_partition_write(part, start, data, len) == ESP_OK;
}

bool Maint::stickImage(uint8_t* ver, uint8_t* patch) {
  if (!imageValid_) return false;
  *ver = imageVer_;
  *patch = imagePatch_;
  return true;
}

void Maint::enter() {
  /* Re-entering maintenance cancels an exit that has not restarted yet. */
  rebootPending_ = false;
  /* With debug Wi-Fi the AP is already up since boot: never start it twice. */
  if (active_) return;
  startServer();
}

void Maint::startServer() {
  if (active_) return;

  WiFi.mode(WIFI_AP_STA);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_ps(WIFI_PS_NONE);
  if (!WiFi.softAP(RTD_AP_SSID, RTD_AP_PASS, ESPNOW_CHANNEL, 0, 4)) {
    Serial.println("[MAINT] soft AP failed");
  }

  server_.on("/", HTTP_GET, [this]() { handleRoot(); });
  server_.on("/api/status", HTTP_GET, [this]() { handleStatus(); });
  server_.on("/debug", HTTP_GET, [this]() { handleDebugPage(); });
  server_.on("/api/debug", HTTP_GET, [this]() { handleDebugJson(); });
  server_.on("/api/log", HTTP_GET, [this]() { handleDebugLog(); });
  server_.on("/api/debug/trace", HTTP_POST, [this]() { handleDebugTrace(); });
  server_.on("/api/debug/wifi", HTTP_POST, [this]() { handleDebugWifi(); });
  server_.on("/fw/joystick.bin", HTTP_GET, [this]() { handleJoystickImage(); });
  server_.on("/upload/host", HTTP_POST, [this]() { handleHostUploadDone(); },
             [this]() { handleHostUpload(server_.upload()); });
  server_.on("/upload/joystick", HTTP_POST, [this]() { handleJoystickUploadDone(); },
             [this]() { handleJoystickUpload(server_.upload()); });
  server_.on("/ota/sticks", HTTP_POST, [this]() { handleOtaSticks(); });
  server_.on("/maint/exit", HTTP_POST, [this]() { handleExit(); });
  server_.onNotFound([this]() { server_.send(404, "text/plain", "not found"); });
  server_.begin();

  active_ = true;
  Serial.printf("[MAINT] AP %s, http://%s\n", RTD_AP_SSID, RTD_HOST_IP);
}

bool Maint::requireMaint() {
  if (game_.phase() == PH_MAINT) return true;
  server_.send(409, "text/plain", "maintenance mode only");
  return false;
}

void Maint::exit() {
  storage_.flush();
  /* Slightly deferred: HostGame::actOtaSticks aborts the phase (which exits
   * maintenance) right before it re-enters maintenance for the stick update. */
  rebootPending_ = true;
  rebootAtMs_ = millis() + 300;
}

void Maint::update() {
  if (active_) server_.handleClient();
  if (rebootPending_ && (int32_t)(millis() - rebootAtMs_) >= 0) {
    storage_.flush();
    /* Diagnostic: remember who ended maintenance (0 = a host update or the web page). */
    Preferences why;
    if (why.begin("rtddbg", false)) {
      why.putUChar("cause", game_.exitCause());
      why.putUChar("src", game_.exitSrc());
      why.end();
    }
    ESP.restart();
  }
}

void Maint::sendAction(uint8_t action, uint8_t argument) {
  GamePacket pkt;
  buildPacket(&pkt, ID_HOST, ID_HOST, CMD_ACTION,
              (uint16_t)(((uint16_t)action << 8) | argument));
  game_.onPacket((const uint8_t*)&pkt, PACKET_SIZE, micros());
}

/* ------------------------------------------------------------------- page */

void Maint::handleRoot() { server_.send_P(200, "text/html", PAGE_HTML); }

void Maint::handleStatus() {
  StatePacket st = game_.state();
  uint32_t now = millis();

  bool displayOnline =
      display.seen && (uint32_t)(now - display.lastSeenMs) <= DISPLAY_TIMEOUT_MS;

  String json;
  json.reserve(768);
  json += "{\"ap\":{\"ssid\":\"";
  json += RTD_AP_SSID;
  json += "\",\"pass\":\"";
  json += RTD_AP_PASS;
  json += "\",\"ip\":\"http://";
  json += RTD_HOST_IP;
  json += "\"},";
  json += "\"host\":{\"version\":\"";
  json += FW_VERSION_STRING;
  json += "\",\"phase\":";
  json += String((unsigned)st.phase);
  json += ",\"uptime_s\":";
  json += String((unsigned long)(now / 1000));
  json += "},\"sticks\":[";

  static const char* COLORS[RTD_MAX_PLAYERS] = {"#FFFFFF", "#0000FF", "#FF0000", "#FFFF00"};
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (i > 0) json += ',';
    json += "{\"id\":";
    json += String((unsigned)(i + 1));
    json += ",\"color\":\"";
    json += COLORS[i];
    json += "\",\"online\":";
    json += (st.sticks_online & (uint8_t)(1u << i)) ? 1 : 0;
    json += ",\"version\":\"";
    json += versionText(st.stick_fw[i], st.stick_fw_patch[i]);
    json += "\"}";
  }

  json += "],\"display\":{\"online\":";
  json += displayOnline ? 1 : 0;
  json += ",\"version\":\"";
  json += versionText(display.ver, display.patch);
  json += "\",\"ip\":\"";
  if (display.ipOctet != 0) {
    json += "192.168.4.";
    json += String((unsigned)display.ipOctet);
  } else {
    json += "--";
  }
  json += "\",\"ota\":";
  json += String((unsigned)display.otaState);
  json += "},\"image\":{\"present\":";
  json += imageValid_ ? 1 : 0;
  json += ",\"version\":\"";
  json += versionText(imageVer_, imagePatch_);
  json += "\"},\"fs\":{\"used\":";
  json += String((unsigned long)SPIFFS.usedBytes());
  json += ",\"total\":";
  json += String((unsigned long)SPIFFS.totalBytes());
  json += "},\"ota\":{\"state\":";
  json += String((unsigned)st.ota_state);
  json += ",\"target\":";
  json += String((unsigned)st.ota_target);
  json += ",\"progress\":";
  json += String((unsigned)st.ota_progress);
  json += "}}";

  server_.send(200, "application/json", json);
}

/* -------------------------------------------------------------- debug hub */

void Maint::handleDebugPage() { server_.send_P(200, "text/html", DEBUG_PAGE_HTML); }

void Maint::handleDebugJson() {
  uint32_t now = debug_.uptimeMs();

  String json;
  json.reserve(4096);
  json += "{\"uptime_ms\":";
  appendNum(json, (long)now);
  json += ",\"loop_max_us\":";
  appendNum(json, (long)debug_.loopMaxUs());
  json += ",\"heap_free\":";
  appendNum(json, (long)debug_.heapFree());
  json += ",\"heap_min\":";
  appendNum(json, (long)debug_.heapMin());
  json += ",\"reset_reason\":";
  appendNum(json, (long)debug_.resetReason());
  {
    Preferences why;
    uint8_t cause = 0, src = 0;
    if (why.begin("rtddbg", true)) {
      cause = why.getUChar("cause", 0);
      src = why.getUChar("src", 0);
      why.end();
    }
    json += ",\"last_exit_cause\":";
    appendNum(json, (long)cause);
    json += ",\"last_exit_src\":";
    appendNum(json, (long)src);
  }
  json += ",\"reset_reason_text\":\"";
  json += debug_.resetReasonText();
  json += '"';
  json += ",\"rx_frames\":";
  appendNum(json, (long)debug_.rxFrames());
  json += ",\"rx_ring_drops\":";
  appendNum(json, (long)debug_.rxRingDrops());
  json += ",\"bad_frames\":";
  appendNum(json, (long)debug_.badFrames());
  json += ",\"tx_frames\":";
  appendNum(json, (long)debug_.txFrames());
  json += ",\"tx_fail\":[";
  for (uint8_t i = 0; i < DBG_TX_DESTS; i++) {
    if (i > 0) json += ',';
    appendNum(json, (long)debug_.txFail(i));
  }
  json += "],\"trace\":";
  appendNum(json, debug_.trace() ? 1 : 0);
  json += ",\"debug_wifi\":";
  appendNum(json, storage_.debugWifi() ? 1 : 0);
  json += ",\"phase\":";
  appendNum(json, (long)game_.phase());
  json += ",\"phase_name\":\"";
  json += Debug::phaseName(game_.phase());
  json += '"';

  json += ",\"devices\":[";
  for (uint8_t i = 0; i < DBG_DEVICE_COUNT; i++) {
    const DebugDevice& dev = debug_.device(i);
    if (i > 0) json += ',';
    json += "{\"name\":\"";
    json += DEVICE_LABELS[i];
    json += "\",\"id\":";
    appendNum(json, i < RTD_MAX_PLAYERS ? (long)(i + 1) : (long)ID_DISPLAY);
    json += ",\"seen\":";
    appendNum(json, dev.seen ? 1 : 0);
    json += ",\"last_ms\":";
    appendNum(json, (long)dev.lastMs);
    json += ",\"age_ms\":";
    appendNum(json, dev.seen ? (long)(now - dev.lastMs) : -1);
    json += ",\"frames\":";
    appendNum(json, (long)dev.frames);
    json += ",\"last_cmd\":";
    appendNum(json, (long)dev.lastCmd);
    json += ",\"last_cmd_name\":\"";
    const char* cmd = Debug::cmdName(dev.lastCmd);
    json += cmd != nullptr ? cmd : "--";
    json += "\",\"diag\":[";
    bool first = true;
    for (uint8_t k = 0; k < dev.diagCount; k++) {
      if (!dev.diag[k].valid) continue;
      if (!first) json += ',';
      first = false;
      json += "{\"key\":";
      appendNum(json, (long)dev.diag[k].key);
      json += ",\"name\":\"";
      json += rtdDiagKeyName(dev.diag[k].key);
      json += "\",\"value\":";
      appendNum(json, (long)dev.diag[k].value);
      json += ",\"ms\":";
      appendNum(json, (long)dev.diag[k].ms);
      json += ",\"age_ms\":";
      appendNum(json, (long)(now - dev.diag[k].ms));
      json += '}';
    }
    json += "]}";
  }
  json += "]}";

  server_.send(200, "application/json", json);
}

void Maint::handleDebugLog() {
  uint32_t since = 0;
  if (server_.hasArg("since")) since = (uint32_t)strtoul(server_.arg("since").c_str(), nullptr, 10);

  String out;
  out.reserve(100 * DBG_EVENT_TEXT_MAX);
  DebugEvent ev;
  char line[DBG_EVENT_TEXT_MAX];
  uint16_t lines = 0;
  while (lines < 100 && debug_.nextEventAfter(since, ev)) {
    debug_.formatEvent(ev, line, sizeof(line));
    out += line;
    out += '\n';
    since = ev.seq;
    lines++;
  }
  server_.send(200, "text/plain", out);
}

void Maint::handleDebugTrace() {
  bool on = true;
  if (server_.hasArg("on")) on = server_.arg("on") != "0";
  debug_.setTrace(on);

  char buf[40];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"trace\":%d}", on ? 1 : 0);
  server_.send(200, "application/json", buf);
}

void Maint::handleDebugWifi() {
  bool on = storage_.debugWifi();
  if (server_.hasArg("on")) on = server_.arg("on") != "0";
  storage_.setDebugWifi(on);

  char buf[160];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"debug_wifi\":%d,\"note\":\"%s\"}", on ? 1 : 0,
           on ? "stored; the access point starts at the next boot"
              : "stored; the access point stays on until the next boot");
  server_.send(200, "application/json", buf);
}

/* ------------------------------------------------------------ fw images */

void Maint::handleJoystickImage() {
  /* DESIGN.md 11.3: outside maintenance mode the image is not offered. */
  const esp_partition_t* part = imageSlot();
  if (game_.phase() != PH_MAINT || !imageValid_ || part == nullptr) {
    server_.send(404, "text/plain", "no image");
    return;
  }

  server_.sendHeader("x-MD5", imageMd5_);
  server_.setContentLength((size_t)imageSize_);
  server_.send(200, "application/octet-stream", "");

  /* Small chunks so that a pump fits between them (beacon, snapshot, ring). */
  WiFiClient client = server_.client();
  uint8_t chunk[1460];
  uint32_t done = 0;
  while (done < imageSize_) {
    uint32_t n = imageSize_ - done;
    if (n > sizeof(chunk)) n = sizeof(chunk);
    if (esp_partition_read(part, IMAGE_OFFSET + done, chunk, n) != ESP_OK) break;
    if (client.write(chunk, n) != n) break;
    done += n;
    pump();
  }
}

/* Scan a chunk for a marker; once found, capture the version text up to '|'. */
void Maint::feedMarker(const uint8_t* data, size_t len, bool& found, uint8_t& match,
                       const char* needle, uint8_t needleLen, char* version,
                       uint8_t* versionLen, bool* versionDone) {
  if (found && (version == nullptr || *versionDone)) return;

  for (size_t i = 0; i < len; i++) {
    uint8_t c = data[i];
    if (!found) {
      if (c == (uint8_t)needle[match]) {
        match++;
        if (match == needleLen) found = true;
      } else {
        match = (c == (uint8_t)needle[0]) ? 1 : 0;
      }
    } else if (version != nullptr && !*versionDone) {
      if (c == '|') {
        version[*versionLen] = '\0';
        *versionDone = true;
      } else if (*versionLen < 11) {
        version[(*versionLen)++] = (char)c;
      }
    }
  }
}

void Maint::handleHostUpload(HTTPUpload& upload) {
  if (upload.status == UPLOAD_FILE_START) {
    hostMarkerFound_ = false;
    hostMatch_ = 0;
    hostUploadOk_ = false;
    /* DESIGN.md 11.3: outside maintenance mode the body is discarded, never
     * opened and never written. */
    uploadRejected_ = game_.phase() != PH_MAINT;
    if (uploadRejected_) return;
    /* An earlier upload may have left the OTA slot open. */
    if (Update.isRunning()) Update.abort();
    /* The host image goes into the slot that holds the joystick image. */
    clearImageRecord();
    hostUpdateOk_ = Update.begin(UPDATE_SIZE_UNKNOWN);
    Serial.printf("[MAINT] host upload start: %s\n", upload.filename.c_str());
  } else if (uploadRejected_) {
    return;
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    feedMarker(upload.buf, upload.currentSize, hostMarkerFound_, hostMatch_, HOST_MARKER,
               (uint8_t)strlen(HOST_MARKER), nullptr, nullptr, nullptr);
    if (hostUpdateOk_ && upload.currentSize > 0 &&
        Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      hostUpdateOk_ = false;
    }
    pump();
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    /* A client can drop right after the last chunk; then END has already run. */
    if (hostUploadOk_) return;
    /* The client dropped: close the update so the next upload starts clean. */
    if (Update.isRunning()) Update.abort();
    hostUpdateOk_ = false;
    hostUploadOk_ = false;
    hostMarkerFound_ = false;
    hostMatch_ = 0;
    Serial.println("[MAINT] host upload aborted");
  } else if (upload.status == UPLOAD_FILE_END) {
    bool ok = hostUpdateOk_ && hostMarkerFound_;
    if (ok) {
      ok = Update.end(true);
    } else {
      Update.abort();
    }
    hostUploadOk_ = ok;
    if (ok) {
      /* The service stays active: the new firmware enters update mode again at boot. */
      storage_.setResumeMaint();
      rebootPending_ = true;
      rebootAtMs_ = millis() + 1500;
    }
    Serial.printf("[MAINT] host upload %s\n", ok ? "ok" : "rejected");
  }
}

void Maint::handleHostUploadDone() {
  /* The phase check also covers a POST that carried no file part. */
  bool rejected = uploadRejected_ || game_.phase() != PH_MAINT;
  uploadRejected_ = false;
  if (rejected) {
    server_.send(409, "text/plain", "maintenance mode only");
    return;
  }
  server_.send(200, "application/json", hostUploadOk_ ? "{\"ok\":true}" : "{\"ok\":false}");
}

void Maint::handleJoystickUpload(HTTPUpload& upload) {
  if (upload.status == UPLOAD_FILE_START) {
    /* DESIGN.md 11.3: outside maintenance mode the body is discarded; the
     * stored image stays untouched. */
    uploadRejected_ = game_.phase() != PH_MAINT;
    if (uploadRejected_) return;
    /* No image is served until a complete upload has been verified. */
    clearImageRecord();
    joystickMarkerFound_ = false;
    joystickMatch_ = 0;
    joystickVersionLen_ = 0;
    joystickVersionDone_ = false;
    joystickVersion_[0] = '\0';
    joystickUploadBytes_ = 0;
    joystickReadback_ = 0;
    joystickMd5_.begin();

    /* A host upload that did not finish holds the same slot. */
    if (Update.isRunning()) Update.abort();
    imagePart_ = imageSlot();
    imageErasedTo_ = 0;
    joystickUploadOk_ = imagePart_ != nullptr;
  } else if (uploadRejected_) {
    return;
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    feedMarker(upload.buf, upload.currentSize, joystickMarkerFound_, joystickMatch_,
               STICK_MARKER, (uint8_t)strlen(STICK_MARKER), joystickVersion_,
               &joystickVersionLen_, &joystickVersionDone_);
    if (joystickUploadOk_ && !imageWrite(upload.buf, upload.currentSize)) {
      joystickUploadOk_ = false;
    }
    joystickMd5_.add(upload.buf, (uint16_t)upload.currentSize);
    joystickUploadBytes_ += upload.currentSize;
    pump();
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    /* A client can drop right after the last chunk; then END has already run. */
    if (imageValid_) return;
    /* The client dropped: keep no version, the partial image is never served. */
    clearImageRecord();
    joystickUploadOk_ = false;
    Serial.println("[MAINT] joystick upload aborted");
  } else if (upload.status == UPLOAD_FILE_END) {
    uint8_t ver = 0;
    uint8_t patch = 0;
    bool ok = joystickUploadOk_ && joystickMarkerFound_ && joystickVersionDone_ &&
              joystickUploadBytes_ > 0 && parseVersion(joystickVersion_, ver, patch);
    String stored;
    if (ok) {
      /* Read the stored image back and compare it with the received bytes. */
      joystickMd5_.calculate();
      ok = imageMd5(joystickUploadBytes_, stored, &joystickReadback_) &&
           stored == joystickMd5_.toString();
    }
    if (ok) ok = storeImageVersion(ver, patch, joystickUploadBytes_, stored);

    if (ok) {
      imageValid_ = true;
      imageVer_ = ver;
      imagePatch_ = patch;
      imageSize_ = joystickUploadBytes_;
      imageMd5_ = stored;
    } else {
      clearImageRecord();
    }
    joystickUploadOk_ = ok;
    Serial.printf("[MAINT] joystick upload %s (%lu bytes, read back %lu)\n", ok ? "ok" : "rejected",
                  (unsigned long)joystickUploadBytes_, (unsigned long)joystickReadback_);
  }
}

void Maint::handleJoystickUploadDone() {
  /* The phase check also covers a POST that carried no file part. */
  bool rejected = uploadRejected_ || game_.phase() != PH_MAINT;
  uploadRejected_ = false;
  if (rejected) {
    server_.send(409, "text/plain", "maintenance mode only");
    return;
  }
  String json = "{\"ok\":";
  json += joystickUploadOk_ ? "true" : "false";
  json += ",\"version\":\"";
  json += imageValid_ ? versionText(imageVer_, imagePatch_) : String("--");
  json += "\",\"bytes\":";
  json += String((unsigned long)joystickUploadBytes_);
  json += ",\"readback\":";
  json += String((unsigned long)joystickReadback_);
  json += ",\"fs_used\":";
  json += String((unsigned long)SPIFFS.usedBytes());
  json += ",\"fs_total\":";
  json += String((unsigned long)SPIFFS.totalBytes());
  json += "}";
  server_.send(200, "application/json", json);
}

void Maint::handleOtaSticks() {
  if (!requireMaint()) return;
  sendAction(ACT_OTA_STICKS, 0);
  server_.send(200, "application/json", "{\"ok\":true}");
}

void Maint::handleExit() {
  if (!requireMaint()) return;
  server_.send(200, "application/json", "{\"ok\":true}");
  rebootPending_ = true;
  rebootAtMs_ = millis() + 500;
}
