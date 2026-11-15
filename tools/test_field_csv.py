"""Check the web page's CSV import (zWebPage.h) end to end, no board needed.

Builds a 40 x 150 tree block near Mildura, writes it as MGA54 CSV with an
independent forward projection, runs the page's own JavaScript (parseField)
in Node, and checks every tree lands within 1 cm of the truth in the
firmware's local frame. Also checks bad files are rejected.

  python tools/test_field_csv.py        (needs node)
"""
import json, math, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, '..', 'aio_v4_tree', 'zWebPage.h')
a, e2, k0 = 6378137.0, 0.00669437999014, 0.9996
ep2 = e2 / (1 - e2)

def mga(lat, lon, zone=54):   # Snyder forward Transverse Mercator, southern hemisphere
    p = math.radians(lat); A = (math.radians(lon) - math.radians((zone - 1) * 6 - 177)) * math.cos(p)
    N = a / math.sqrt(1 - e2 * math.sin(p) ** 2); T = math.tan(p) ** 2; C = ep2 * math.cos(p) ** 2; e4 = e2 * e2; e6 = e4 * e2
    M = a * ((1 - e2/4 - 3*e4/64 - 5*e6/256) * p - (3*e2/8 + 3*e4/32 + 45*e6/1024) * math.sin(2*p)
             + (15*e4/256 + 45*e6/1024) * math.sin(4*p) - (35*e6/3072) * math.sin(6*p))
    x = k0 * N * (A + (1 - T + C) * A**3 / 6 + (5 - 18*T + T*T + 72*C - 58*ep2) * A**5 / 120) + 500000
    y = k0 * (M + N * math.tan(p) * (A*A/2 + (5 - T + 9*C + 4*C*C) * A**4 / 24
                                     + (61 - 58*T + T*T + 600*C - 330*ep2) * A**6 / 720)) + 10000000
    return x, y

lat0, lon0, MPD = -34.2312345, 142.1123456, 111319.49
ml, b = MPD * math.cos(math.radians(lat0)), math.radians(20)
truth = []
for r in range(40):
    for t in range(150):
        dn = t*3.0*math.cos(b) - r*6.7*math.sin(b); de = t*3.0*math.sin(b) + r*6.7*math.cos(b)
        truth.append((de, dn, lat0 + dn / MPD, lon0 + de / ml))

js = re.search(r'<script>(.*)</script>', open(PAGE, encoding='utf-8').read(), re.S).group(1)
d = tempfile.mkdtemp()
csv = 'Index,X,Y,Z\n' + ''.join('%d,%.4f,%.4f,50\n' % ((i,) + mga(la, lo)) for i, (_, _, la, lo) in enumerate(truth))
open(os.path.join(d, 'field.csv'), 'w').write(csv)
open(os.path.join(d, 'page.js'), 'w', encoding='utf-8').write(js)
open(os.path.join(d, 'run.js'), 'w').write(r'''
const fs=require('fs');global.document={getElementById:()=>({})};global.fetch=()=>new Promise(()=>{});global.setTimeout=()=>0;
eval(fs.readFileSync(__dirname+'/page.js','utf8').replace(/poll\(\);\s*$/,''));
const f=parseField(fs.readFileSync(__dirname+'/field.csv','utf8'),'54'),bad=[];
for(const [t,z] of [['X,Y\n600000,6200000\n',''],['foo,bar\n1,2\n','54'],['X,Y\n12,34\n','54']]){try{parseField(t,z)}catch(e){bad.push(String(e))}}
console.log(JSON.stringify({n:f.n,E:Array.from(f.E),N:Array.from(f.N),bad:bad}));''')
out = json.loads(subprocess.check_output(['node', os.path.join(d, 'run.js')]))
assert out['n'] == len(truth), out['n']
worst = max(math.hypot(out['E'][i] - te, out['N'][i] - tn) for i, (te, tn, _, _) in enumerate(truth))
assert worst < 0.01, 'worst error %.4f m' % worst
assert len(out['bad']) == 3, out['bad']
print('OK: %d trees, worst error %.1f mm, 3 bad files rejected' % (out['n'], worst * 1000))
