// AiO tree marker web page (status, settings, output test, firmware update).
// Plain HTML inside a C++ raw string: edit it as HTML. Served by otaLoop() in zzzOta.ino.
static const char WEB_PAGE[] = R"HTML(
<!doctype html><meta name=viewport content="width=device-width,initial-scale=1"><title>AiO tree marker</title>
<style>
body{font:16px/1.45 system-ui,sans-serif;margin:0;background:#f2f1ec;color:#1d1f1a}
main{max-width:34em;margin:0 auto;padding:12px 14px 40px;display:grid;gap:14px}
h1{font-size:1.3em;margin:4px 0}h2{font-size:1.05em;margin:0 0 8px}
.card{background:#fff;border:1px solid #d9d6cc;border-radius:8px;padding:12px 14px}
.row{display:flex;justify-content:space-between;gap:8px;padding:3px 0;border-bottom:1px solid #eee}
.row:last-child{border:0}.row b{font-variant-numeric:tabular-nums}
#flash{font:700 1.6em system-ui;text-align:center;padding:14px;border-radius:8px;background:#e5e3dc;color:#888;transition:background .15s}
#flash.on{background:#1e9e3a;color:#fff}
label{display:flex;justify-content:space-between;align-items:center;gap:10px;padding:5px 0}
input[type=number]{width:7em;font-size:1em;padding:4px}select{font-size:1em;padding:4px}
input[type=checkbox]{width:22px;height:22px}
button{font-size:1em;padding:9px 14px;border-radius:6px;border:1px solid #8a2b1d;background:#9b2d1f;color:#fff;cursor:pointer}
button.alt{background:#fff;color:#9b2d1f}.btns{display:flex;flex-wrap:wrap;gap:8px;margin-top:8px}
.note{font-size:.9em;color:#555;margin:6px 0 0}.msg{font-weight:700;margin-top:8px;min-height:1.2em}
small{color:#666}
</style>
<main>
<h1>AiO tree marker <small id=ver></small></h1>
<div id=flash>no mark yet</div>

<div class=card><h2>Status</h2>
<div class=row><span>GPS fix</span><b id=fix>-</b></div>
<div class=row><span>Speed</span><b id=spd>-</b></div>
<div class=row><span>Trees loaded</span><b id=trees>-</b></div>
<div class=row><span>Trees marked</span><b id=hits>-</b></div>
<div class=row><span>Output</span><b id=out>-</b></div>
</div>

<div class=card><h2>Settings</h2>
<form id=sf>
<label>Marker enabled <input type=checkbox id=en></label>
<label>Use Cytron output for the solenoid <input type=checkbox id=cyt></label>
<label>Pulse length (ms) <input type=number id=pulse min=10 max=5000 step=1></label>
<label>System lag (ms) <input type=number id=lag min=0 max=1000 step=1></label>
<label>Hit radius (m) <input type=number id=hr min=0.02 max=5 step=0.01></label>
<label>Nozzle behind antenna (m) <input type=number id=fa step=0.01></label>
<label>Nozzle right of antenna (m) <input type=number id=lat step=0.01></label>
<label>Output polarity <select id=dir><option value=1>A</option><option value=0>B</option></select></label>
<div class=btns><button type=submit>Save</button></div>
<p class=note>Offsets are measured from the right-hand (main) GPS antenna. Left of it is negative.
Turning the Cytron output off needs an AiO restart.</p>
<div class=msg id=smsg></div>
</form></div>

<div class=card><h2>Test the output</h2>
<p class=note>Multimeter on DC volts. Red probe on the solenoid wire that should be +, black on the other.
Press <b>Hold ON 5 s</b>. About +12 V: correct. About -12 V: press <b>Flip polarity</b> and test again.
0 V: tick "Use Cytron output", Save, and check the wiring and fuse.</p>
<div class=btns><button onclick="t('hold')">Hold ON 5 s</button><button class=alt onclick="t('pulse')">Single pulse</button><button class=alt onclick="t('flip')">Flip polarity</button></div>
<div class=msg id=tmsg></div></div>

<div class=card><h2>Firmware update</h2>
<p>Latest on GitHub: <b id=lv>checking...</b> <button id=ub onclick=latest() disabled>Update to latest</button></p>
<form method=post action=/update enctype=multipart/form-data class=note>Or pick a .hex file: <input type=file name=f accept=".hex"> <button class=alt>Upload</button></form>
<p class=note>Autosteer must be off.</p>
<progress id=p max=100 value=0 style="width:100%"></progress><div class=msg id=umsg></div></div>
</main>
<script>
var R='https://raw.githubusercontent.com/mandeepmildura/sandy-soils-aio-tree-marker/ota/',$=function(i){return document.getElementById(i)},C='',lastF=-1,ft;
var F=['pulse','lag','hr','fa','lat'];
function post(u,b){return fetch(u,{method:'POST',body:b}).then(function(r){return r.text()})}
function fill(d){$('en').checked=d.en;$('cyt').checked=d.cyt;$('dir').value=d.dir;F.forEach(function(k){$(k).value=d[k]})}
function poll(){fetch('/status').then(function(r){return r.json()}).then(function(d){
 if(!C){C=d.ver;$('ver').textContent='v'+d.ver;fill(d);ver()}
 $('fix').textContent=['none','GPS','DGPS','','RTK fixed','RTK float'][d.fix]||(d.fix==9?'simulated':d.fix);
 $('spd').textContent=(d.spd*3.6).toFixed(1)+' km/h';$('trees').textContent=d.trees;$('hits').textContent=d.hits;
 $('out').textContent=d.cyt?(d.on?'ON':'off')+' (polarity '+(d.dir?'A':'B')+')':'not in use (dry run)';
 if(lastF>=0&&d.fires>lastF){$('flash').className='on';$('flash').textContent='MARK  #'+d.fires;clearTimeout(ft);ft=setTimeout(function(){$('flash').className=''},500)}
 lastF=d.fires}).catch(function(){$('fix').textContent='AiO not answering'}).then(function(){setTimeout(poll,400)})}
$('sf').onsubmit=function(e){e.preventDefault();var b='en='+($('en').checked?1:0)+'&cyt='+($('cyt').checked?1:0)+'&dir='+$('dir').value;
 F.forEach(function(k){b+='&'+k+'='+encodeURIComponent($(k).value)});$('smsg').textContent='Saving...';
 post('/settings',b).then(function(t){$('smsg').textContent=t}).catch(function(){$('smsg').textContent='Not saved: AiO not answering'})};
function t(a){$('tmsg').textContent='...';post('/test','a='+a).then(function(x){$('tmsg').textContent=x;if(a=='flip')C=''}).catch(function(){$('tmsg').textContent='AiO not answering'})}
function send(b){var x=new XMLHttpRequest();x.upload.onprogress=function(e){$('p').value=e.loaded*100/e.total;$('umsg').textContent='Uploading '+Math.round(e.loaded/1024)+' of '+Math.round(e.total/1024)+' KB'};
 x.onload=function(){$('umsg').textContent=x.responseText};x.onerror=function(){$('umsg').textContent='Connection lost. If it said rebooting, refresh in 10 s.'};
 x.open('POST','/update');x.send(b);$('umsg').textContent='Uploading...'}
function latest(){$('umsg').textContent='Downloading from GitHub...';fetch(R+'aio_v4_tree.hex?'+Date.now()).then(function(r){if(!r.ok)throw r.status;return r.blob()}).then(send).catch(function(e){$('umsg').textContent='Download failed ('+e+'). Pick the file instead.'})}
function ver(){fetch(R+'version.txt?'+Date.now()).then(function(r){if(!r.ok)throw r.status;return r.text()}).then(function(v){v=v.trim();$('lv').textContent='v'+v+(v==C?' (you have it)':'');$('ub').disabled=false}).catch(function(){$('lv').textContent='no internet - pick a file'})}
poll();
</script>
)HTML";
