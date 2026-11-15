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
<div class=row><span>Field</span><b id=field>-</b></div>
<div class=row><span>Trees loaded</span><b id=trees>-</b></div>
<div class=row><span>Field start from tractor</span><b id=dist>-</b></div>
<div class=row><span>Trees marked</span><b id=hits>-</b></div>
<div class=row><span>Output</span><b id=out>-</b></div>
</div>

<div class=card><h2>Field (tree positions)</h2>
<p class=note>CSV with a header row: <b>X</b>/<b>Easting</b> and <b>Y</b>/<b>Northing</b> in MGA metres (other columns are ignored),
or <b>Lat</b> and <b>Lon</b> columns in degrees. One tree per line, up to 20000. Same CSV files as the ESP32 marker.</p>
<label>CSV file <input type=file id=cf accept=".csv,.txt"></label>
<label>MGA zone <select id=zone><option value="">choose</option><option value=54>MGA 54 (Sunraysia)</option><option value=55>MGA 55</option><option value=56>MGA 56</option></select></label>
<div id=cprev class=note></div>
<div class=btns><button id=cs disabled onclick=sendTrees()>Send to AiO</button></div>
<div class=msg id=cmsg></div></div>

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
var MPD=111319.49,FD=null;
function utm2ll(E,N,zone){var a=6378137,e2=0.00669437999014,k0=0.9996,e4=e2*e2,e6=e4*e2,ep2=e2/(1-e2);
 var y=N-(N>5e6?1e7:0),x=E-5e5,lon0=((zone-1)*6-180+3)*Math.PI/180,M=y/k0;
 var mu=M/(a*(1-e2/4-3*e4/64-5*e6/256)),e1=(1-Math.sqrt(1-e2))/(1+Math.sqrt(1-e2));
 var p=mu+(3*e1/2-27*e1*e1*e1/32)*Math.sin(2*mu)+(21*e1*e1/16-55*Math.pow(e1,4)/32)*Math.sin(4*mu)+(151*e1*e1*e1/96)*Math.sin(6*mu)+(1097*Math.pow(e1,4)/512)*Math.sin(8*mu);
 var s=Math.sin(p),c=Math.cos(p),t=Math.tan(p),N1=a/Math.sqrt(1-e2*s*s),T1=t*t,C1=ep2*c*c,R1=a*(1-e2)/Math.pow(1-e2*s*s,1.5),D=x/(N1*k0);
 var lat=p-(N1*t/R1)*(D*D/2-(5+3*T1+10*C1-4*C1*C1-9*ep2)*Math.pow(D,4)/24+(61+90*T1+298*C1+45*T1*T1-252*ep2-3*C1*C1)*Math.pow(D,6)/720);
 var lon=lon0+(D-(1+2*T1+C1)*D*D*D/6+(5-2*C1+28*T1-3*C1*C1+8*ep2+24*T1*T1)*Math.pow(D,5)/120)/c;
 return [lat*180/Math.PI,lon*180/Math.PI]}
function parseField(text,zone){var L=text.split(/\r?\n/),h=-1;
 for(var i=0;i<L.length;i++)if(L[i].trim()){h=i;break}
 if(h<0)throw 'The file is empty.';
 var cols=L[h].split(',').map(function(x){return x.trim().toUpperCase()}),ix=function(){for(var k=0;k<arguments.length;k++){var j=cols.indexOf(arguments[k]);if(j>=0)return j}return -1};
 var xi=ix('X','EASTING'),yi=ix('Y','NORTHING'),la=ix('LAT','LATITUDE'),lo=ix('LON','LONG','LONGITUDE'),ll=la>=0&&lo>=0;
 if(!ll&&(xi<0||yi<0))throw 'Header row needs X/Easting and Y/Northing columns (or Lat and Lon).';
 if(!ll&&!zone)throw 'Choose the MGA zone first. A wrong zone puts the trees hundreds of km away.';
 var pts=[],bad=0;
 for(var i=h+1;i<L.length;i++){if(!L[i].trim())continue;var f=L[i].split(',');
  var A=parseFloat(f[ll?la:xi]),B=parseFloat(f[ll?lo:yi]);if(!isFinite(A)||!isFinite(B)){bad++;continue}
  if(ll){pts.push([A,B]);continue}
  if(A<1e5||A>9e5||B<5e6||B>1e7)throw 'Line '+(i+1)+' ('+A+', '+B+') is not an MGA easting/northing in the southern hemisphere.';
  pts.push(utm2ll(A,B,+zone))}
 if(!pts.length)throw 'No tree rows found under the header.';
 if(pts.length>20000)throw pts.length+' trees: the AiO holds at most 20000.';
 var la0=pts[0][0],lo0=pts[0][1],ml=MPD*Math.cos(la0*Math.PI/180),n=pts.length,E=new Float32Array(n),Nn=new Float32Array(n);
 for(var i=0;i<n;i++){E[i]=(pts[i][1]-lo0)*ml;Nn[i]=(pts[i][0]-la0)*MPD}
 return {n:n,E:E,N:Nn,lat0:la0,lon0:lo0,bad:bad}}
function med(a){a=a.slice().sort(function(x,y){return x-y});return a.length?a[a.length>>1]:0}
function preview(){FD=null;$('cs').disabled=true;$('cmsg').textContent='';var f=$('cf').files[0];if(!f){$('cprev').textContent='';return}
 f.text().then(function(t){try{var d=parseField(t,$('zone').value)}catch(e){$('cprev').textContent=e;return}
  var mnE=1e9,mxE=-1e9,mnN=1e9,mxN=-1e9,g=[];for(var i=0;i<d.n;i++){mnE=Math.min(mnE,d.E[i]);mxE=Math.max(mxE,d.E[i]);mnN=Math.min(mnN,d.N[i]);mxN=Math.max(mxN,d.N[i]);if(i)g.push(Math.hypot(d.E[i]-d.E[i-1],d.N[i]-d.N[i-1]))}
  d.name=f.name.replace(/[^A-Za-z0-9 ._-]/g,'').slice(0,31);FD=d;$('cs').disabled=false;
  $('cprev').innerHTML='<b>'+d.n+' trees</b> in <b>'+d.name+'</b><br>Block '+(mxE-mnE).toFixed(0)+' m east-west by '+(mxN-mnN).toFixed(0)+' m north-south, typical tree spacing '+med(g).toFixed(2)+' m'+
   (d.bad?'<br>'+d.bad+' lines skipped (not numbers)':'')+'<br>First tree '+d.lat0.toFixed(7)+', '+d.lon0.toFixed(7)+distTxt(d.lat0,d.lon0)})}
var TR=null;function distTxt(a,b){if(!TR)return '';var d=Math.hypot((b-TR[1])*MPD*Math.cos(a*Math.PI/180),(a-TR[0])*MPD);return '<br>'+(d>5000?'<b style="color:#b00">':'<b>')+'That is '+fmtD(d)+' from the tractor.'+(d>5000?' Wrong zone or wrong file?':'')+'</b>'}
function fmtD(d){return d<1000?d.toFixed(0)+' m':(d/1000).toFixed(1)+' km'}
$('cf').onchange=preview;$('zone').onchange=preview;
function sendTrees(){if(!FD)return;var n=FD.n,b=new ArrayBuffer(56+8*n),v=new DataView(b);
 v.setUint32(0,0x31544D54,true);v.setUint32(4,n,true);v.setFloat64(8,FD.lat0,true);v.setFloat64(16,FD.lon0,true);
 for(var i=0;i<FD.name.length;i++)v.setUint8(24+i,FD.name.charCodeAt(i));
 new Float32Array(b,56,n).set(FD.E);new Float32Array(b,56+4*n,n).set(FD.N);
 $('cs').disabled=true;$('cmsg').textContent='Sending '+n+' trees...';
 fetch('/trees',{method:'POST',body:b}).then(function(r){return r.text()}).then(function(t){$('cmsg').textContent=t;$('cs').disabled=false}).catch(function(){$('cmsg').textContent='AiO not answering. Nothing changed if it never got the file.';$('cs').disabled=false})}

function post(u,b){return fetch(u,{method:'POST',body:b}).then(function(r){return r.text()})}
function fill(d){$('en').checked=d.en;$('cyt').checked=d.cyt;$('dir').value=d.dir;F.forEach(function(k){$(k).value=d[k]})}
function poll(){fetch('/status').then(function(r){return r.json()}).then(function(d){
 if(!C){C=d.ver;$('ver').textContent='v'+d.ver;fill(d);ver()}
 $('fix').textContent=['none','GPS','DGPS','','RTK fixed','RTK float'][d.fix]||(d.fix==9?'simulated':d.fix);
 $('spd').textContent=(d.spd*3.6).toFixed(1)+' km/h';$('trees').textContent=d.trees;$('field').textContent=d.field||'none';
 TR=(d.la||d.lo)?[d.la,d.lo]:null;
 if(TR&&d.trees&&(d.fla||d.flo)){var dd=Math.hypot((d.flo-d.lo)*MPD*Math.cos(d.la*Math.PI/180),(d.fla-d.la)*MPD);$('dist').textContent=fmtD(dd)+(dd>5000?' (check zone/file)':'')}else $('dist').textContent=d.trees?'no GPS yet':'-';$('hits').textContent=d.hits;
 $('out').textContent=d.cyt?(d.on?'ON':'off')+' (polarity '+(d.dir?'A':'B')+')':'not in use (dry run)';
 if(lastF>=0&&d.fires>lastF){$('flash').className='on';$('flash').textContent='MARK  #'+d.fires;clearTimeout(ft);ft=setTimeout(function(){$('flash').className=''},500)}
 lastF=d.fires}).catch(function(){$('fix').textContent='AiO not answering'}).then(function(){setTimeout(poll,700)})}
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
