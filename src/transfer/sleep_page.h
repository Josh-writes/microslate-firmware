#pragma once
// The "add a sleep picture" page, served from flash in Transfer's picture mode. The browser does all the image work:
// it scales and crops the photo to this device's screen, turns it black and white (Atkinson dithering, with a
// brightness control), shows exactly what the MicroSlate will show, and sends a finished 1-bit BMP.
// The device only checks and stores it. No outside resources.

static const char kSleepPage[] = R"PAGE(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MicroSlate sleep picture</title>
<style>
:root{--bg:#fff;--fg:#111;--mut:#666;--line:#ccc;--btn:#111;--btnfg:#fff}
@media (prefers-color-scheme:dark){:root{--bg:#141414;--fg:#eee;--mut:#999;--line:#444;--btn:#eee;--btnfg:#111}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:17px/1.45 -apple-system,system-ui,Segoe UI,Roboto,sans-serif}
main{max-width:680px;margin:0 auto;padding:16px}
h1{font-size:24px;margin:8px 0 4px}
.mut{color:var(--mut);font-size:14px}
.row{display:flex;gap:10px;flex-wrap:wrap;margin:14px 0}
button,label.b{flex:1 1 140px;min-height:52px;border:2px solid var(--btn);border-radius:10px;background:var(--btn);color:var(--btnfg);
 font:600 18px system-ui,sans-serif;display:flex;align-items:center;justify-content:center;cursor:pointer;padding:0 14px}
button.o{background:transparent;color:var(--fg)}
button.on{background:var(--btn);color:var(--btnfg)}
button:disabled{opacity:.4}
#cv{width:100%;height:auto;display:block;border:1px solid var(--line);background:#fff;touch-action:none;cursor:grab;image-rendering:pixelated}
#msg{min-height:24px;font-weight:600;margin-top:10px}
input[type=range]{width:100%}
input[type=file]{display:none}
.hide{display:none}
</style></head><body><main>
<h1>Sleep picture</h1>
<p class="mut" id="sub">Loading...</p>
<div class="row"><label class="b" id="pick">Choose picture<input type="file" id="file" accept="image/*"></label></div>
<div id="edit" class="hide">
 <canvas id="cv"></canvas>
 <p class="mut">Drag the picture to choose what shows. This is exactly how the MicroSlate will show it.</p>
 <div class="row"><button class="o" id="fill">Fill screen</button><button class="o" id="fit">Whole picture</button></div>
 <label class="mut" for="br">Brightness</label><input type="range" id="br" min="-80" max="80" value="0">
 <label class="mut" for="ct">Contrast</label><input type="range" id="ct" min="-50" max="80" value="0">
 <div class="row"><button id="send">Send to MicroSlate</button></div>
</div>
<div id="msg"></div>
</main>
<script>
const base=location.pathname.endsWith('/')?location.pathname:location.pathname+'/';
const $=id=>document.getElementById(id);
const cv=$('cv'),ctx=cv.getContext('2d'),msg=$('msg');
let W=800,H=480,img=null,fill=true,ox=0,oy=0,out=null,busy=false;

fetch(base+'list',{cache:'no-store'}).then(r=>{if(!r.ok)throw 0;return r.json();}).then(d=>{
 if(d.mode!='picture')throw 0;
 W=d.w;H=d.h;cv.width=W;cv.height=H;
 $('sub').textContent='Pick a photo. It becomes a black and white picture for the sleep screen ('+(W>H?'landscape':'portrait')+').';
}).catch(()=>{$('sub').textContent='The MicroSlate is not ready for pictures. Keep its Add a picture screen open, then reload this page.';$('pick').classList.add('hide');});

$('file').onchange=e=>{
 const f=e.target.files[0];if(!f)return;
 const u=URL.createObjectURL(f),im=new Image();
 im.onload=()=>{img=im;ox=0;oy=0;$('edit').classList.remove('hide');msg.textContent='';$('pick').firstChild.textContent='Choose another picture';draw();};
 im.onerror=()=>{msg.textContent='That file could not be opened as a picture.';};
 im.src=u;
};
function mode(f){fill=f;ox=0;oy=0;$('fill').classList.toggle('on',f);$('fit').classList.toggle('on',!f);draw();}
$('fill').onclick=()=>mode(true);$('fit').onclick=()=>mode(false);mode(true);
$('br').oninput=$('ct').oninput=()=>draw();

// Scale and place the photo, keeping the drag offset inside what can be shown.
function place(){
 const s=fill?Math.max(W/img.width,H/img.height):Math.min(W/img.width,H/img.height);
 const w=img.width*s,h=img.height*s;
 const mx=Math.max(0,(w-W)/2),my=Math.max(0,(h-H)/2);
 ox=Math.max(-mx,Math.min(mx,ox));oy=Math.max(-my,Math.min(my,oy));
 return{x:(W-w)/2+ox,y:(H-h)/2+oy,w,h};
}
let pending=false;
function draw(){if(!img||pending)return;pending=true;requestAnimationFrame(()=>{pending=false;render();});}
function render(){
 const p=place();
 ctx.fillStyle='#fff';ctx.fillRect(0,0,W,H);
 ctx.drawImage(img,p.x,p.y,p.w,p.h);
 const d=ctx.getImageData(0,0,W,H),px=d.data,g=new Float32Array(W*H);
 const b=+$('br').value*1.6,c=(+$('ct').value+100)/100;
 for(let i=0,j=0;i<g.length;i++,j+=4){
  const a=px[j+3]/255,l=(0.299*px[j]+0.587*px[j+1]+0.114*px[j+2])*a+255*(1-a);
  g[i]=(l-128)*c+128+b;
 }
 // Atkinson dithering: crisp on e-ink, keeps light areas clean.
 out=new Uint8Array(W*H);
 for(let y=0;y<H;y++)for(let x=0;x<W;x++){
  const i=y*W+x,o=g[i],n=o<128?0:255,e=(o-n)/8;out[i]=n?1:0;
  if(x+1<W)g[i+1]+=e;if(x+2<W)g[i+2]+=e;
  if(y+1<H){if(x>0)g[i+W-1]+=e;g[i+W]+=e;if(x+1<W)g[i+W+1]+=e;}
  if(y+2<H)g[i+2*W]+=e;
 }
 for(let i=0,j=0;i<out.length;i++,j+=4){const v=out[i]?255:0;px[j]=px[j+1]=px[j+2]=v;px[j+3]=255;}
 ctx.putImageData(d,0,0);
}

// Drag to move the crop (finger or mouse).
let drag=null;
cv.onpointerdown=e=>{if(!img)return;drag={x:e.clientX,y:e.clientY};cv.setPointerCapture(e.pointerId);};
cv.onpointermove=e=>{if(!drag)return;const k=W/cv.getBoundingClientRect().width;ox+=(e.clientX-drag.x)*k;oy+=(e.clientY-drag.y)*k;drag={x:e.clientX,y:e.clientY};draw();};
cv.onpointerup=cv.onpointercancel=()=>{drag=null;};

// A 1-bit BMP: palette 0 black, 1 white, rows bottom-up and padded to 4 bytes.
function bmp(){
 const rb=Math.ceil(W/32)*4,size=62+rb*H,buf=new ArrayBuffer(size),v=new DataView(buf),u=new Uint8Array(buf);
 u[0]=66;u[1]=77;v.setUint32(2,size,true);v.setUint32(10,62,true);v.setUint32(14,40,true);
 v.setInt32(18,W,true);v.setInt32(22,H,true);v.setUint16(26,1,true);v.setUint16(28,1,true);
 v.setUint32(34,rb*H,true);v.setUint32(46,2,true);v.setUint32(50,2,true);
 u[58]=u[59]=u[60]=255;
 for(let y=0;y<H;y++){const row=62+(H-1-y)*rb;for(let x=0;x<W;x++)if(out[y*W+x])u[row+(x>>3)]|=0x80>>(x&7);}
 return new Blob([buf],{type:'image/bmp'});
}
$('send').onclick=()=>{
 if(!out||busy)return;busy=true;$('send').disabled=true;msg.textContent='Sending...';
 const fd=new FormData();fd.append('picture',bmp(),'picture.bmp');
 fetch(base+'picture',{method:'POST',body:fd}).then(r=>r.json().then(j=>({ok:r.ok,j}))).then(({ok,j})=>{
  msg.textContent=ok?('Saved as '+j.label+' on the MicroSlate. Choose another to send more.'):(j.error||'Not saved.');
 }).catch(()=>{msg.textContent='Could not reach the MicroSlate. Keep its screen open and try again.';})
 .finally(()=>{busy=false;$('send').disabled=false;});
};
</script></body></html>)PAGE";
