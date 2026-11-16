#pragma once
// The Transfer page, served from flash. One page for both ways in: it asks the device what is on offer (/list) and
// shows either one note (Copy content, Download .txt or .md) or every note (pick some or all, Download).
// No outside resources: no fonts, no scripts from the internet, works with no internet at all.
// Several notes download as one .zip made in the browser (stored, not compressed; .txt is small).

static const char kTransferPage[] = R"PAGE(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MicroSlate</title>
<style>
:root{--bg:#fff;--fg:#111;--mut:#666;--line:#ccc;--btn:#111;--btnfg:#fff}
@media (prefers-color-scheme:dark){:root{--bg:#141414;--fg:#eee;--mut:#999;--line:#444;--btn:#eee;--btnfg:#111}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:17px/1.45 -apple-system,system-ui,Segoe UI,Roboto,sans-serif}
main{max-width:680px;margin:0 auto;padding:16px}
h1{font-size:24px;margin:8px 0 4px}
.mut{color:var(--mut);font-size:14px}
.row{display:flex;gap:10px;flex-wrap:wrap;margin:16px 0}
button,a.b{flex:1 1 180px;min-height:52px;border:2px solid var(--btn);border-radius:10px;background:var(--btn);color:var(--btnfg);
 font:600 18px system-ui,sans-serif;text-decoration:none;display:flex;align-items:center;justify-content:center;cursor:pointer;padding:0 14px}
button.o,a.o{background:transparent;color:var(--fg)}
button:disabled{opacity:.4}
textarea{width:100%;min-height:45vh;border:1px solid var(--line);border-radius:8px;padding:10px;background:var(--bg);color:var(--fg);
 font:16px/1.5 Georgia,serif;resize:vertical}
#msg{min-height:22px;font-weight:600}
ul{list-style:none;margin:0;padding:0;border-top:1px solid var(--line)}
li{display:flex;gap:8px;align-items:center;border-bottom:1px solid var(--line)}
li label{flex:1;display:flex;gap:12px;align-items:center;padding:12px 4px;cursor:pointer}
button.c{flex:none;min-height:44px;font-size:16px;padding:0 16px;min-width:96px}
li input,.all input{width:22px;height:22px;flex:none}
li .t{flex:1;overflow-wrap:anywhere}
.all{display:flex;gap:12px;align-items:center;padding:10px 4px;font-weight:600}
</style></head><body><main id="app"><p class="mut">Loading...</p></main>
<script>
const base=location.pathname.endsWith('/')?location.pathname:location.pathname+'/';
const app=document.getElementById('app');
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const size=n=>n<1024?n+' bytes':(n/1024).toFixed(n<10240?1:0)+' KB';
function fail(t){app.innerHTML='<h1>MicroSlate</h1><p>'+esc(t)+'</p><p class="mut">Keep the Transfer screen open on the MicroSlate, then reload this page.</p>';}
fetch(base+'list',{cache:'no-store'}).then(r=>{if(!r.ok)throw 0;return r.json();}).then(d=>d.mode=='one'?one(d):all(d))
 .catch(()=>fail('The MicroSlate is not sharing right now.'));

function one(d){
 const n=d.notes[0];
 if(!n){fail('The note is not there any more.');return;}
 app.innerHTML='<h1>'+esc(n.title)+'</h1><div class="mut">'+esc(n.name)+' &middot; '+size(n.size)+'</div>'+
  '<div class="row"><button id="cp">Copy content</button><a class="b o" href="'+base+'n/0?dl=1">Download '+(/\.md$/i.test(n.name)?'.md':'.txt')+'</a></div>'+
  '<div id="msg"></div><textarea id="txt" readonly></textarea>';
 const ta=document.getElementById('txt'),msg=document.getElementById('msg'),cp=document.getElementById('cp');
 cp.disabled=true;
 fetch(base+'n/0',{cache:'no-store'}).then(r=>{if(!r.ok)throw 0;return r.text();}).then(t=>{ta.value=t;cp.disabled=false;})
  .catch(()=>{msg.textContent='Could not load the note. Reload the page.';});
 cp.onclick=()=>{
  const done=ok=>{msg.textContent=ok?'Copied. Paste it anywhere.':'Select the text below and copy it.';};
  if(navigator.clipboard&&window.isSecureContext){navigator.clipboard.writeText(ta.value).then(()=>done(true),()=>done(legacy(ta)));}
  else done(legacy(ta));
 };
}
// Plain http pages cannot use the modern clipboard call, so copy the way older pages do: select, then copy.
function legacy(ta){
 ta.removeAttribute('readonly');ta.focus();ta.select();ta.setSelectionRange(0,ta.value.length);
 let ok=false;try{ok=document.execCommand('copy');}catch(e){}
 ta.setAttribute('readonly','');return ok;
}

// Copy text that is not on the page: a hidden text box, selected and copied during the tap.
function copyNow(t){
 if(navigator.clipboard&&window.isSecureContext){navigator.clipboard.writeText(t);return true;}
 const ta=document.createElement('textarea');ta.value=t;
 ta.style.cssText='position:fixed;top:0;left:0;width:1px;height:1px;opacity:0;font-size:16px';
 document.body.appendChild(ta);const ok=legacy(ta);ta.remove();return ok;
}

function all(d){
 const notes=d.notes;
 if(!notes.length){fail('There are no notes on the MicroSlate yet.');return;}
 app.innerHTML='<h1>Notes on the MicroSlate</h1><div class="mut">'+notes.length+' note'+(notes.length==1?'':'s')+'</div>'+
  '<label class="all"><input type="checkbox" id="sa" checked> Select all</label><ul>'+
  notes.map(n=>'<li><label><input type="checkbox" class="pick" value="'+n.i+'" checked><span class="t">'+esc(n.title)+
   '<br><span class="mut">'+esc(n.name)+' &middot; '+size(n.size)+'</span></span></label>'+
   '<button class="c o" data-i="'+n.i+'">Copy</button></li>').join('')+
  '</ul><div class="row"><button id="dl"></button></div><div id="msg"></div>';
 const picks=[...document.querySelectorAll('.pick')],sa=document.getElementById('sa'),dl=document.getElementById('dl'),
  msg=document.getElementById('msg');
 const chosen=()=>picks.filter(p=>p.checked).map(p=>notes.find(n=>n.i==p.value));
 const update=()=>{const c=chosen().length;sa.checked=c==picks.length;sa.indeterminate=c>0&&c<picks.length;
  dl.disabled=!c;dl.textContent=c<2?'Download':'Download '+c+' notes';};
 sa.onchange=()=>{picks.forEach(p=>p.checked=sa.checked);update();};
 picks.forEach(p=>p.onchange=update);update();
 // Copy: a phone only copies during the tap itself, so the text must already be here. Fetch every note quietly as
 // the page opens (up to about 3 MB); a note not here yet is fetched on the first tap and copied on the second.
 const texts={};
 (async()=>{let total=0;for(const n of notes){if(total+n.size>3e6)break;
  try{const r=await fetch(base+'n/'+n.i+'?p=1',{cache:'no-store'});if(r.ok){texts[n.i]=await r.text();total+=n.size;}}catch(e){}}})();
 document.querySelectorAll('button.c').forEach(b=>b.onclick=()=>{
  const i=b.dataset.i,t=texts[i];
  const say=(s,keep)=>{b.textContent=s;clearTimeout(b.tm);if(!keep)b.tm=setTimeout(()=>b.textContent='Copy',2500);};
  if(t===undefined){say('Loading...',true);
   fetch(base+'n/'+i+'?p=1',{cache:'no-store'}).then(r=>{if(!r.ok)throw 0;return r.text();})
    .then(x=>{texts[i]=x;say('Tap again',true);}).catch(()=>say('Not loaded'));return;}
  say(copyNow(t)?'Copied':'Copy failed');
 });
 dl.onclick=async()=>{
  const c=chosen();
  if(c.length==1){location.href=base+'n/'+c[0].i+'?dl=1';return;}
  dl.disabled=true;const files=[];
  try{
   for(let k=0;k<c.length;k++){
    msg.textContent='Getting '+(k+1)+' of '+c.length+'...';
    const r=await fetch(base+'n/'+c[k].i,{cache:'no-store'});if(!r.ok)throw 0;
    files.push({name:c[k].name,data:new Uint8Array(await r.arrayBuffer())});
   }
   const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([zip(files)],{type:'application/zip'}));
   a.download='MicroSlate notes '+new Date().toISOString().slice(0,10)+'.zip';document.body.appendChild(a);a.click();a.remove();
   msg.textContent=c.length+' notes downloaded.';
  }catch(e){msg.textContent='The download stopped. Keep the Transfer screen open and try again.';}
  update();
 };
}

// A stored (uncompressed) .zip of the files, names in UTF-8.
function zip(files){
 const T=new Uint32Array(256);for(let n=0;n<256;n++){let c=n;for(let k=0;k<8;k++)c=c&1?0xEDB88320^(c>>>1):c>>>1;T[n]=c>>>0;}
 const crc=b=>{let c=~0;for(let i=0;i<b.length;i++)c=T[(c^b[i])&255]^(c>>>8);return ~c>>>0;};
 const d=new Date(),tm=(d.getHours()<<11)|(d.getMinutes()<<5)|(d.getSeconds()>>1),
  dt=((d.getFullYear()-1980)<<9)|((d.getMonth()+1)<<5)|d.getDate();
 const enc=new TextEncoder(),parts=[],cen=[];let off=0;
 const used=new Set();
 for(const f of files){
  let nm=f.name;for(let k=2;used.has(nm);k++)nm=f.name.replace(/(\.txt|\.md)?$/i,' ('+k+')$1');used.add(nm);
  const name=enc.encode(nm),c=crc(f.data),L=new DataView(new ArrayBuffer(30)),C=new DataView(new ArrayBuffer(46));
  L.setUint32(0,0x04034b50,true);L.setUint16(4,20,true);L.setUint16(6,0x0800,true);L.setUint16(10,tm,true);L.setUint16(12,dt,true);
  L.setUint32(14,c,true);L.setUint32(18,f.data.length,true);L.setUint32(22,f.data.length,true);L.setUint16(26,name.length,true);
  C.setUint32(0,0x02014b50,true);C.setUint16(4,20,true);C.setUint16(6,20,true);C.setUint16(8,0x0800,true);C.setUint16(12,tm,true);
  C.setUint16(14,dt,true);C.setUint32(16,c,true);C.setUint32(20,f.data.length,true);C.setUint32(24,f.data.length,true);
  C.setUint16(28,name.length,true);C.setUint32(42,off,true);
  parts.push(new Uint8Array(L.buffer),name,f.data);cen.push(new Uint8Array(C.buffer),name);
  off+=30+name.length+f.data.length;
 }
 let cl=0;cen.forEach(p=>cl+=p.length);
 const E=new DataView(new ArrayBuffer(22));E.setUint32(0,0x06054b50,true);E.setUint16(8,files.length,true);
 E.setUint16(10,files.length,true);E.setUint32(12,cl,true);E.setUint32(16,off,true);
 return new Blob([...parts,...cen,new Uint8Array(E.buffer)]);
}
</script></body></html>
)PAGE";
