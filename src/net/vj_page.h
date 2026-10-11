// The phone VJ deck: one page, served by the sync server at /vj?t=<pairing token> (the QR code in DasVis's right-click
// menu opens it). It sends commands to /vj/cmd and shows what's on screen. Plain HTML, no libraries.
#pragma once

namespace wb::sync {

inline constexpr const char* kVjPage = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<meta name="theme-color" content="#0b0a10">
<title>DasVis deck</title>
<style>
:root{--bg:#0b0a10;--card:#17151f;--line:#2a2733;--text:#f1eef7;--dim:#9a94a8;--accent:#c9a2ff;--hot:#ff7a90}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.35 system-ui,sans-serif;padding:14px 14px 28px;user-select:none;overflow-x:hidden}
h1{font-size:13px;letter-spacing:.14em;text-transform:uppercase;color:var(--dim);margin:2px 0 10px;font-weight:600}
#now{font-size:20px;font-weight:700;margin-bottom:12px;min-height:26px}
#now small{display:block;font-size:13px;color:var(--dim);font-weight:500}
#pad{position:relative;width:100%;aspect-ratio:1.4;border-radius:18px;background:radial-gradient(circle at 50% 50%,#2a2140,#14111c);
  border:1px solid var(--line);touch-action:none;overflow:hidden}
#pad:after{content:"camera";position:absolute;left:12px;top:10px;font-size:12px;color:var(--dim)}
#dot{position:absolute;width:34px;height:34px;margin:-17px 0 0 -17px;border-radius:50%;background:var(--accent);
  box-shadow:0 0 24px var(--accent);left:50%;top:50%;pointer-events:none}
.row{display:flex;gap:8px;margin-top:12px}
button{flex:1 1 0;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;border:1px solid var(--line);background:var(--card);color:var(--text);border-radius:14px;padding:14px 8px;
  font:600 15px system-ui,sans-serif}
button.on{background:var(--accent);color:#120e1a;border-color:var(--accent)}
#drop{background:var(--hot);color:#1a0c10;border:0;font-size:18px;padding:18px}
#scenes{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px;margin-top:12px}
#scenes button{padding:12px 6px;font-weight:500}
#msg{color:var(--dim);font-size:13px;margin-top:12px;min-height:18px}
</style></head><body>
<h1>DasVis deck</h1>
<div id="now">…</div>
<div id="pad"><div id="dot"></div></div>
<div class="row"><button id="drop">Drop now</button></div>
<div class="row"><button data-c="shock">Shockwave</button><button data-c="like">❤ Like</button><button data-c="skip">Skip</button></div>
<div class="row" id="int"><button data-i="0">Chill</button><button data-i="1">Normal</button><button data-i="2">Insane</button></div>
<div class="row"><button data-c="prev">‹ Prev</button><button data-c="lock" id="lock">Lock</button><button data-c="next">Next ›</button></div>
<div id="scenes"></div>
<div id="msg"></div>
<script>
const t=new URLSearchParams(location.search).get('t')||'';
const $=s=>document.querySelector(s);
let busy=false,pending=null;
async function send(cmd){
  if(busy){pending=cmd;return}
  busy=true;
  try{
    const r=await fetch('/vj/cmd',{method:'POST',headers:{'x-wreckbox-token':t,'content-type':'application/json'},body:JSON.stringify(cmd)});
    if(r.ok)show(await r.json());else $('#msg').textContent=r.status==403?'Not paired: scan the code again':'Error '+r.status;
  }catch(e){$('#msg').textContent='Can\'t reach WreckBox';}
  busy=false;
  if(pending){const p=pending;pending=null;send(p)}
}
function show(s){
  if(!s.open){$('#now').innerHTML='DasVis isn\'t open<small>Open the full-screen visualizer on the computer</small>';return}
  $('#now').innerHTML=s.scene+'<small>'+(s.status||'')+'</small>';
  $('#lock').classList.toggle('on',!!s.locked);
  document.querySelectorAll('#int button').forEach(b=>b.classList.toggle('on',+b.dataset.i===s.intensity));
  const box=$('#scenes');
  if(box.childElementCount!==s.scenes.length){
    box.innerHTML='';
    s.scenes.forEach((n,i)=>{const b=document.createElement('button');b.textContent=n;b.onclick=()=>send({cmd:'scene',value:i});box.appendChild(b)});
  }
  [...box.children].forEach((b,i)=>b.classList.toggle('on',b.textContent===s.scene));
  $('#msg').textContent='';
}
document.querySelectorAll('[data-c]').forEach(b=>b.onclick=()=>send({cmd:b.dataset.c}));
document.querySelectorAll('[data-i]').forEach(b=>b.onclick=()=>send({cmd:'intensity',value:+b.dataset.i}));
$('#drop').onclick=()=>send({cmd:'drop'});
// The XY pad: where the finger is turns and zooms the picture; it eases back to the middle when let go.
const pad=$('#pad'),dot=$('#dot');let x=0,y=0,held=false;
function move(e){const r=pad.getBoundingClientRect();x=Math.max(-1,Math.min(1,(e.clientX-r.left)/r.width*2-1));
  y=Math.max(-1,Math.min(1,(e.clientY-r.top)/r.height*2-1));place();send({cmd:'camera',x:x,y:-y})}
function place(){dot.style.left=(x+1)*50+'%';dot.style.top=(y+1)*50+'%'}
pad.onpointerdown=e=>{held=true;pad.setPointerCapture(e.pointerId);move(e)};
pad.onpointermove=e=>{if(held)move(e)};
pad.onpointerup=pad.onpointercancel=()=>{held=false;x=0;y=0;place();send({cmd:'camera',x:0,y:0})};
// Shake the phone: a shockwave (where the browser allows motion events on this page).
let last=0;
addEventListener('devicemotion',e=>{const a=e.accelerationIncludingGravity;if(!a)return;
  const g=Math.hypot(a.x||0,a.y||0,a.z||0);const now=Date.now();if(g>25&&now-last>700){last=now;send({cmd:'shock'})}});
send({cmd:'state'});setInterval(()=>{if(!held)send({cmd:'state'})},1500);
</script></body></html>)HTML";

}  // namespace wb::sync
