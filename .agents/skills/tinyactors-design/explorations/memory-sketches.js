(()=>{
const $=s=>document.querySelector(s);
const h=(tag,cls,html)=>{const e=document.createElement(tag);if(cls)e.className=cls;if(html!=null)e.innerHTML=html;return e;};
const D={step:420,fill:480,pub:240,ring:900,drain:360,wrap:700,fade:240};
let K=1,paused=false,timer;
const later=(ms,fn)=>setTimeout(fn,ms*K);
const reflow=e=>void e.offsetWidth;
if(matchMedia('(prefers-reduced-motion: reduce)').matches)document.body.classList.add('rm');
const W='var(--accent-fg)',R='var(--fg-1)';

class Tape{
constructor(el,{n,sw=44,lanes=[],idx,wrap=false,wrapLabel='wrap',cls=''}){
this.n=n;this.sw=sw;this.gap=4;this.heads={};this.pos={};
const t=h('div','tape '+cls);t.style.setProperty('--n',n);t.style.setProperty('--sw',sw+'px');
const mk=l=>{l.el=h('div','lane'+(l.pos==='bot'?' bot':'')+(l.sm?' sm':''));return l.el;};
lanes.filter(l=>l.pos==='top').forEach(l=>t.appendChild(mk(l)));
if(idx){const r=h('div','idx');idx.forEach(s=>r.appendChild(h('span','',s)));t.appendChild(r);}
const row=h('div','slots');this.slots=[];
for(let i=0;i<n;i++){const s=h('div','slot','<div class="fill"></div><div class="ring"></div><div class="v"></div>');row.appendChild(s);this.slots.push(s);}
t.appendChild(row);
lanes.filter(l=>l.pos!=='top').forEach(l=>t.appendChild(mk(l)));
if(wrap){this.wl=h('div','wrapline','<span>'+wrapLabel+'</span>');t.appendChild(this.wl);}
this.lanes=Object.fromEntries(lanes.map(l=>[l.key,l.el]));
el.appendChild(t);this.el=t;}
head(id,lane,label,color){const d=h('div','head','<b></b><span>'+label+'</span><span class="hs"></span>');d.style.setProperty('--hc',color);this.lanes[lane].appendChild(d);this.heads[id]=d;this.pos[id]=0;return d;}
sub(id,t){this.heads[id].querySelector('.hs').textContent=t;}
move(id,i,jump){const d=this.heads[id],prev=this.pos[id],X=n=>`translateX(${n*(this.sw+this.gap)}px)`;this.pos[id]=i;
if(i<prev||jump){const wrap=i<prev&&!jump&&!!this.wl;if(wrap){d.style.transform=X(prev+0.6);this.flash(d.style.getPropertyValue('--hc'));}d.style.opacity=0;
later(D.fade,()=>{d.classList.add('nt');d.style.transform=X(i-(wrap?0.6:0));reflow(d);d.classList.remove('nt');d.style.opacity=1;d.style.transform=X(i);});}
else d.style.transform=X(i);}
fadeClear(i){this.set(i,{drain:true,cur:false});later(D.drain,()=>this.clear(i));}
flash(c){const w=this.wl;w.style.setProperty('--hc',c);w.classList.remove('pulse');reflow(w);w.classList.add('pulse');}
set(i,o){const s=this.slots[i];
if('val' in o){const v=s.querySelector('.v');v.innerHTML=o.val==null?'':o.val+(o.sub?`<small>${o.sub}</small>`:'');if(o.val!=null&&o.on){v.classList.remove('vin');reflow(v);v.classList.add('vin');}}
['on','pending','pub','drain','blocked','cur'].forEach(k=>{if(k in o)s.classList.toggle(k,!!o[k]);});
if(o.pips){let p=s.querySelector('.pips');if(!p){p=h('div','pips');s.appendChild(p);}p.innerHTML=o.pips.map(x=>`<i class="${x?'x':''}"></i>`).join('');}}
clear(i){const s=this.slots[i];s.className='slot nt';s.querySelector('.v').innerHTML='';const p=s.querySelector('.pips');if(p)p.remove();reflow(s);s.classList.remove('nt');}
}

const tickers=[];
function start(){clearInterval(timer);if(!paused)timer=setInterval(()=>tickers.forEach(f=>f()),900*K);}

/* 1a ByteOrder */
(()=>{const root=$('#bo');const addrs=['0x1000','0x1001','0x1002','0x1003'];
const mkSet=(name,order)=>{const w=h('div');w.appendChild(h('div','sub',name));
const m=new Tape(w,{n:4,sw:56,idx:addrs,lanes:[{key:'t',pos:'top'}]});m.head('ld','t','load',W);
order.forEach((b,i)=>m.set(i,{val:b,on:true}));
w.appendChild(h('div','arrow','↓ register'));
const r=new Tape(w,{n:4,sw:56,idx:['31–24','23–16','15–8','7–0'],cls:'reg'});
root.appendChild(w);return{m,r,order};};
const sets=[Object.assign(mkSet('Big-endian · network order',['0A','0B','0C','0D']),{map:i=>i}),Object.assign(mkSet('Little-endian · x86, ARM default',['0D','0C','0B','0A']),{map:i=>3-i})];
let s=0;tickers.push(()=>{sets.forEach(o=>{
if(s<4){o.m.move('ld',s);o.m.slots.forEach((el,i)=>o.m.set(i,{cur:i===s}));const ri=o.map(s);o.r.set(ri,{val:o.order[s],on:true});}
else if(s===7){o.m.slots.forEach((el,i)=>o.m.set(i,{cur:false}));for(let i=0;i<4;i++)o.r.fadeClear(i);o.m.move('ld',0,true);}});
s=(s+1)%8;});})();

/* 1b WireLayout */
(()=>{const root=$('#pb');const bytes=['08','AC','02','12','03','50','41','59'];
const t=new Tape(root,{n:8,sw:46,idx:bytes.map((b,i)=>String(i)),lanes:[{key:'t',pos:'top'}]});t.head('d','t','decode',W);
bytes.forEach((b,i)=>t.set(i,{val:b}));
const g=h('div','groups');[[1,'tag 1'],[2,'id = 300'],[1,'tag 2'],[1,'len 3'],[3,'type = "PAY"']].forEach(([n,l])=>{const s=h('span','',`<em>${l}</em>`);s.style.gridColumn='span '+n;g.appendChild(s);});
t.el.appendChild(g);const dp=h('div','decode');root.appendChild(dp);
const ex=['<b>08</b> tag · 0x08 = (1 &lt;&lt; 3) | 0 → field 1, wire type 0 (varint)','<b>AC</b> = 1·0101100 · continuation bit set, keep 0101100 (44)','<b>02</b> = 0·0000010 · last byte → (2 &lt;&lt; 7) | 44 = <b>300</b> → id','<b>12</b> tag · 0x12 = (2 &lt;&lt; 3) | 2 → field 2, wire type 2 (length-delimited)','<b>03</b> length · the next 3 bytes are the payload','<b>50</b> → "P"','<b>41</b> → "A"','<b>59</b> → "Y" · type = <b>"PAY"</b>'];
let s=0;dp.innerHTML='<span style="color:var(--fg-3)">8 bytes · press play</span>';
tickers.push(()=>{if(s<8){t.move('d',s);bytes.forEach((b,i)=>t.set(i,{cur:i===s,on:i<s}));dp.innerHTML=ex[s];}
else if(s===10){bytes.forEach((b,i)=>t.set(i,{on:false,cur:false}));t.move('d',0,true);dp.innerHTML='<span style="color:var(--fg-3)">restart</span>';}
s=(s+1)%11;});})();

/* shared SPSC ring: 1c, 2a, 2b, 2c */
const CAP=8,ring={w:0,r:0,pub:0,t:0,blocked:false,vals:{}};const subs=[];let mid=100;
const emit=e=>subs.forEach(f=>f(e));
tickers.push(()=>{ring.t++;const ph=ring.t%28;const wantW=ph<12||ph%5===0;const wantR=ring.t%2===0;
if(wantW&&ring.w-ring.r>=CAP){ring.blocked=true;emit({type:'block'});}
else{ring.blocked=false;if(wantW){const seq=ring.w++;ring.vals[seq]='m'+(mid++);emit({type:'write',seq});later(D.fill,()=>{ring.pub=Math.max(ring.pub,seq+1);emit({type:'pub',seq});});}}
if(wantR&&ring.r<ring.pub){const seq=ring.r++;emit({type:'read',seq});}
emit({type:'tick'});});

(()=>{const root=$('#spsc');const t=new Tape(root,{n:8,idx:[...Array(8)].map((_,i)=>String(i)),lanes:[{key:'w',pos:'top'},{key:'r',pos:'bot'}],wrap:true,wrapLabel:'wrap · seq & 7'});
t.head('w','w','W',W);t.head('r','r','R',R);const meta=h('div','meta');root.appendChild(meta);
subs.push(e=>{const sl=e.seq&7;
if(e.type==='write'){t.set(sl,{val:ring.vals[e.seq],on:true,pending:true,blocked:false});const nx=ring.w&7;later(D.fill*0.6,()=>t.move('w',nx));}
if(e.type==='pub'){t.set(sl,{pending:false,pub:true});later(D.step,()=>t.set(sl,{pub:false}));}
if(e.type==='read'){t.set(sl,{drain:true});later(D.drain,()=>t.clear(sl));const nr=ring.r&7;later(D.drain*0.5,()=>t.move('r',nr));}
if(e.type==='tick'){t.heads.w.classList.toggle('blocked',ring.blocked);t.sub('w',ring.blocked?'blocked':'');t.slots.forEach((s,i)=>t.set(i,{blocked:ring.blocked&&i===(ring.w&7)}));
meta.innerHTML=ring.blocked?'<span class="warn">writer blocked</span>':'';}});})();

(()=>{const root=$('#dial');const cx=120,pt=(r,a)=>{const t=(a-90)*Math.PI/180;return[cx+r*Math.cos(t),cx+r*Math.sin(t)];};
let svg=`<svg class="dial" viewBox="-12 -12 264 264">`;
for(let i=0;i<8;i++){const a0=i*45+1.5,a1=(i+1)*45-1.5,[x0,y0]=pt(100,a0),[x1,y1]=pt(100,a1),[x2,y2]=pt(62,a1),[x3,y3]=pt(62,a0);
svg+=`<path class="sec" data-i="${i}" d="M${x0} ${y0}A100 100 0 0 1 ${x1} ${y1}L${x2} ${y2}A62 62 0 0 0 ${x3} ${y3}Z"/>`;
const[tx,ty]=pt(112,i*45+22.5),[vx,vy]=pt(81,i*45+22.5);svg+=`<text x="${tx}" y="${ty+3}" text-anchor="middle">${i}</text><text class="sv" data-v="${i}" x="${vx}" y="${vy+3}" text-anchor="middle"></text>`;}
svg+=`<g class="hd" id="dW"><path d="M120 0 L114 -10 L126 -10Z" fill="var(--accent-fg)" transform="translate(0 20)"/></g><g class="hd" id="dR"><path d="M120 62 L114 72 L126 72Z" fill="var(--fg-1)" transform="translate(0 -4)"/></g>`;
svg+=`<text class="ctr" id="dC" x="120" y="122" text-anchor="middle">0/8</text><text class="ctr2" x="120" y="138" text-anchor="middle">used</text></svg>`;
const wrap=h('div','dwrap',svg+'<div class="meta" style="flex-direction:column;gap:8px"><span style="color:var(--accent-fg)">▼ W outside · next write</span><span>▲ R inside · next read</span><span id="dS"></span></div>');root.appendChild(wrap);
const secs=[...root.querySelectorAll('.sec')],vals=[...root.querySelectorAll('.sv')];
subs.push(e=>{if(e.type!=='tick'&&e.type!=='pub')return;
secs.forEach((p,i)=>{p.classList.remove('u','nx');});vals.forEach(v=>v.textContent='');
for(let q=ring.r;q<ring.w;q++){secs[q&7].classList.add('u');vals[q&7].textContent=ring.vals[q];}
if(!ring.blocked)secs[ring.w&7].classList.add('nx');
$('#dW').style.transform=`rotate(${ring.w*45+22.5}deg)`;$('#dR').style.transform=`rotate(${ring.r*45+22.5}deg)`;
$('#dC').textContent=(ring.w-ring.r)+'/8';$('#dS').innerHTML=ring.blocked?'<span class="warn" style="color:var(--state-waiting)">blocked · W caught R</span>':'';});})();

(()=>{const root=$('#seq');const v=h('div','seqv');const s=h('div','seqs');const cells=h('div');const map=new Map();s.appendChild(cells);
const win=h('div','win');win.style.width=(8*48-4)+'px';s.appendChild(win);
const hw=h('div','sh w','<span>W</span><b></b>'),hr=h('div','sh r','<b></b><span>R</span>');s.appendChild(hw);s.appendChild(hr);v.appendChild(s);root.appendChild(v);

subs.push(e=>{if(e.type!=='tick'&&e.type!=='pub')return;const lo=Math.max(0,ring.r-4),hi=ring.r+14;for(const[q,el]of map){if(q<lo||q>=hi){el.remove();map.delete(q);}}
for(let q=lo;q<hi;q++){let el=map.get(q);if(!el){el=h('div','sc g',`${q}<small>s${q&7}</small>`);el.style.left=q*48+'px';cells.appendChild(el);map.set(q,el);}el.className='sc '+(q<ring.r?'o':q<ring.pub?'u':q<ring.w?'u p':'g');}s.style.transform=`translateX(${-(ring.r-2)*48+8}px)`;win.style.transform=`translateX(${ring.r*48}px)`;
hw.style.transform=`translateX(${ring.w*48}px)`;hr.style.transform=`translateX(${ring.r*48}px)`;hw.classList.toggle('blocked',ring.blocked);});})();

(()=>{const root=$('#inl');const box=h('div','rows');root.appendChild(box);
const rows=[{n:'billing/ledger',live:true},{n:'auth/session',c:2,w:5},{n:'mail/outbox',c:7,w:3},{n:'search/indexer',c:0,w:0}];
rows.forEach(r=>{r.el=h('div','row',`<span class="nm">${r.n}</span><span class="mm">${'<i></i>'.repeat(8)}</span><span class="cnt"></span>`);box.appendChild(r.el);});
const draw=(r,w,c)=>{[...r.el.querySelectorAll('.mm i')].forEach((el,i)=>{const used=((i-(w-c))%8+8)%8<c;el.className=(used?'u':'')+(c<8&&i===(w&7)?' nx':'');});const ce=r.el.querySelector('.cnt');ce.textContent=c+'/8';ce.classList.toggle('full',c>=8);};
rows.forEach(r=>{if(!r.live)draw(r,r.w,r.c);});
subs.push(e=>{if(e.type!=='tick')return;draw(rows[0],ring.w,ring.w-ring.r);
if(ring.t%3===0)rows.slice(1).forEach((r,j)=>{const d=[[1,-1,0,1],[1,1,0,-1],[0,1,-1,0]][j][Math.floor(ring.t/3)%4];if(d>0&&r.c<8){r.c++;r.w++;}if(d<0&&r.c>0)r.c--;draw(r,r.w,r.c);});});})();

/* 1d BroadcastRing */
(()=>{const root=$('#bc');const t=new Tape(root,{n:8,idx:[...Array(8)].map((_,i)=>String(i)),lanes:[{key:'w',pos:'top'},{key:'r0',pos:'bot',sm:true},{key:'r1',pos:'bot',sm:true},{key:'r2',pos:'bot',sm:true}],wrap:true,wrapLabel:'wrap'});
t.head('w','w','W',W);const names=['R0 fast','R1','R2 slow'];names.forEach((n,i)=>t.head('r'+i,'r'+i,n,R));
const meta=h('div','meta');root.appendChild(meta);
const st={w:0,rs:[0,0,0],t:0,freed:0,blocked:false};let id=1;const every=[1,2,3];
tickers.push(()=>{st.t++;const ph=st.t%24;const wantW=ph<14&&st.t%1===0;const min=Math.min(...st.rs);
if(wantW&&st.w-st.freed>=8)st.blocked=true;
else{st.blocked=false;if(wantW){const seq=st.w++,sl=seq&7;t.set(sl,{val:'e'+(id++),on:true,pending:true,pips:[0,0,0]});later(D.fill,()=>{t.set(sl,{pending:false,pub:true});later(D.step,()=>t.set(sl,{pub:false}));});{const nx=st.w&7;later(D.fill*0.6,()=>t.move('w',nx));}}}
st.rs.forEach((r,i)=>{if(st.t%every[i]===0&&r<st.w-1+(st.blocked?1:0)&&r<st.w){const seq=r;st.rs[i]++;t.move('r'+i,st.rs[i]&7);
const sl=seq&7;t.set(sl,{pips:st.rs.map(x=>x>seq)});}});
const nmin=Math.min(...st.rs);while(st.freed<nmin){const sl=st.freed&7;t.set(sl,{drain:true});later(D.drain,()=>t.clear(sl));st.freed++;}
const gate=st.rs.indexOf(nmin);t.heads.w.classList.toggle('blocked',st.blocked);t.sub('w',st.blocked?'blocked':'');
names.forEach((n,i)=>{const g=st.blocked&&i===gate;t.heads['r'+i].classList.toggle('blocked',g);t.sub('r'+i,g?'gating':'');});
t.slots.forEach((s,i)=>t.set(i,{blocked:st.blocked&&i===(st.w&7)}));
meta.innerHTML=st.blocked?`<span class="warn">writer waits on R${gate}</span>`:'';});})();

/* 1e CacheLineAccess */
(()=>{const root=$('#fs');
const core=(n,role)=>`<div class="core">core ${n}<small>${role}</small><em class="i">I</em></div>`;
const scen=(cls,title,lines,label)=>{const s=h('div','fs',`<div class="fs-h ${cls}">${title}</div><div class="fs-row">${core(0,'writer · tail')}<div class="fs-mid">${lines.map((l,i)=>`<div class="cl"><span>L${i}</span>${l.map(c=>c?`<i data-f="${c}">${c}</i>`:'<i class="pad"></i>').join('')}</div>`).join('')}<div class="trace ${cls==='good'?'ok':''}"><span class="dot"></span></div></div>${core(1,'reader · head')}</div><div class="meta"></div>`);root.appendChild(s);return s;};
const bad=scen('bad','✕ same line · struct { uint64_t tail; uint64_t head; }',[['tail','head',0,0,0,0,0,0]],'invalidate + transfer');
const good=scen('good','✓ own lines · alignas(64) uint64_t tail; alignas(64) uint64_t head;',[['tail',0,0,0,0,0,0,0],['head',0,0,0,0,0,0,0]],'no traffic');
let n=0,xfer=0,writes=0;
const ems=s=>s.querySelectorAll('.core em');
const setEm=(e,m)=>{e.className=m?'m':'i';e.textContent=m?'M':'I';};
tickers.push(()=>{const c=n%2,f=c===0?'tail':'head';writes++;
[bad,good].forEach(s=>{const cell=s.querySelector(`i[data-f="${f}"]`);cell.classList.add('hot');later(D.wrap,()=>cell.classList.remove('hot'));});
const be=ems(bad);setEm(be[c],1);setEm(be[1-c],0);xfer++;
const dot=bad.querySelector('.dot');dot.classList.add('nt');dot.style.left=c===0?'calc(100% - 8px)':'0';dot.style.opacity=1;reflow(dot);dot.classList.remove('nt');dot.style.left=c===0?'0':'calc(100% - 8px)';later(D.wrap,()=>dot.style.opacity=0);
const ge=ems(good);setEm(ge[0],1);setEm(ge[1],1);
bad.querySelector('.meta').innerHTML=`<span>writes <b>${writes}</b></span><span style="color:var(--state-error)">line transfers <b style="color:inherit">${xfer}</b></span>`;
good.querySelector('.meta').innerHTML=`<span>writes <b>${writes}</b></span><span>line transfers <b>0</b></span><span>cost: 128 B instead of 16 B</span>`;n++;});})();

/* 2d listing, 2e reorder, 2f hex */
const F=[{n:'state',t:'uint8_t',o:0,s:1,c:1},{p:1,o:1,s:7},{n:'id',t:'uint64_t',o:8,s:8,c:2},{n:'flags',t:'uint16_t',o:16,s:2,c:3},{p:1,o:18,s:6},{n:'mailbox',t:'void *',o:24,s:8,c:4},{n:'refs',t:'uint32_t',o:32,s:4,c:6},{n:'prio',t:'uint8_t',o:36,s:1,c:5},{p:1,o:37,s:3}];
const tint=c=>`color-mix(in oklch, var(--data-${c}) 38%, var(--surface-1))`;
(()=>{const root=$('#ph');const box=h('div','rows');box.appendChild(h('div','ph hd','<span>off</span><span>size</span><span>0 · 8 · 16 · 24 · 32 · 40</span><span>field</span><span>type</span>'));
F.forEach(f=>box.appendChild(h('div','ph'+(f.p?' pad':''),`<span>${f.o}</span><span>${f.s}</span><span class="bar"><i style="left:${f.o/40*100}%;width:${f.s/40*100}%"></i></span><span class="n">${f.p?'pad':f.n}</span><span class="t">${f.p?'—':f.t}</span>`)));
root.appendChild(box);root.appendChild(h('div','foot','sizeof <b>40</b> · padding <b>16 B</b> (40%) · 1 per 64 B cache line'));})();
(()=>{const root=$('#ro');const PX=11;const box=h('div','ro');
const strip=(title,fs,total)=>{const w=h('div');w.appendChild(h('div','sub',title));const s=h('div','strip');s.style.width=total*PX+'px';
fs.forEach(f=>{const i=h('i',f.p?'pad':'',f.s>=4?`${f.p?'':f.n}<small>${f.s} B</small>`:'');i.style.width=f.s*PX+'px';i.title=(f.p?'pad':f.n)+' · '+f.s+' B';if(!f.p)i.style.background=tint(f.c);i.style.padding=f.s<4?'0':'';s.appendChild(i);});
w.appendChild(s);const r=h('div','rul');r.style.width=total*PX+'px';r.style.backgroundSize='';r.style.background=`repeating-linear-gradient(to right,var(--fg-3) 0 1px,transparent 1px ${8*PX}px)`;w.appendChild(r);return w;};
box.appendChild(strip('Declared order · 40 B',F,40));
const S=[F[2],F[5],F[6],F[3],F[0],F[7]];box.appendChild(strip('Sorted by alignment · 24 B',S,24));
box.appendChild(h('div','delta','−16 B per actor · 2 per 64 B cache line, was 1'));
box.appendChild(h('div','foot','ticks every 8 B · 1-byte fields: state, prio (hover for names)'));root.appendChild(box);})();
(()=>{const root=$('#hx');const bytes=[];const val={state:['02'],id:['2A','00','00','00','00','00','00','00'],flags:['11','00'],mailbox:['80','4E','00','1C','3A','7F','00','00'],refs:['03','00','00','00'],prio:['01']};
F.forEach(f=>{for(let i=0;i<f.s;i++)bytes.push(f.p?{p:1}:{b:val[f.n][i],c:f.c});});
const g=h('div','hx');for(let r=0;r<3;r++){g.appendChild(h('span','o','0x'+(r*16).toString(16).padStart(2,'0')));
for(let c=0;c<16;c++){const b=bytes[r*16+c];if(!b){g.appendChild(h('span'));continue;}const i=h('i',b.p?'pad':'',b.p?'··':b.b);if(!b.p)i.style.background=tint(b.c);g.appendChild(i);}}
root.appendChild(g);const lg=h('div','lg');
[['state',1,'2'],['id',2,'42 → 2A 00 … 00'],['flags',3,'0x0011 → 11 00'],['mailbox',4,'0x7f3a1c004e80 → 80 4E …'],['refs',6,'3'],['prio',5,'1']].forEach(([n,c,v])=>lg.appendChild(h('span','',`<i style="background:${tint(c)}"></i><span><b>${n}</b> = ${v}</span>`)));
root.appendChild(lg);})();

/* Section 3 primitives */
(()=>{const root=$('#prims');
const card=(id,title,tok)=>{const f=h('section','frame');f.id=id;f.dataset.screenLabel=id+' '+title;f.innerHTML=`<div class="fh"><span class="id">${id}</span><h3>${title}</h3></div><div class="tok">${tok}</div>`;root.appendChild(f);return f;};
const idx6=['0','1','2','3','4','5'];
{const f=card('3a','step','<span><b>step</b> 420ms</span><span><b>in-out</b></span>');const t=new Tape(f,{n:6,sw:40,idx:idx6,lanes:[{key:'t',pos:'top'}]});t.head('h','t','R',R);['a','b','c','d','e','f'].forEach((v,i)=>t.set(i,{val:v,on:true}));
let i=0;tickers.push(()=>{i=(i+1)%7;if(i===6)return;t.move('h',i,i===0);});}
{const f=card('3b','fill → publish','<span><b>fill</b> 480ms decelerate</span><span><b>publish</b> 240ms in · 900ms out</span>');const t=new Tape(f,{n:6,sw:40,idx:idx6,lanes:[{key:'t',pos:'top'}]});t.head('h','t','W',W);
let i=0;tickers.push(()=>{if(i===6){for(let j=0;j<6;j++)t.fadeClear(j);t.move('h',0,true);i=0;return;}const s=i;t.set(s,{val:'m'+s,on:true,pending:true});later(D.fill,()=>{t.set(s,{pending:false,pub:true});later(D.step,()=>t.set(s,{pub:false}));});i++;const ni=i;later(D.fill*0.6,()=>{if(ni<6)t.move('h',ni);});});}
{const f=card('3c','drain','<span><b>drain</b> 360ms accelerate</span><span>8px toward head</span>');const t=new Tape(f,{n:6,sw:40,idx:idx6,lanes:[{key:'t',pos:'bot'}]});t.head('h','t','R',R);const fill=()=>{for(let j=0;j<6;j++){t.clear(j);t.set(j,{val:'m'+j,on:true});}};fill();
let i=0;tickers.push(()=>{if(i===6){fill();t.move('h',0,true);i=0;return;}const s=i;t.set(s,{drain:true});later(D.drain,()=>t.clear(s));i++;const ni=i;if(ni<6)later(D.drain*0.5,()=>t.move('h',ni));});}
{const f=card('3d','block','<span><b>fade</b> 240ms, then hold</span><span><b>--state-waiting</b></span>');const t=new Tape(f,{n:6,sw:40,idx:idx6,lanes:[{key:'w',pos:'top'},{key:'r',pos:'bot'}]});t.head('w','w','W',W);t.head('r','r','R',R);
for(let j=0;j<6;j++)t.set(j,{val:'m'+j,on:true});let p=0,c=0,id=6;const blk=()=>{t.heads.w.classList.add('blocked');t.sub('w','blocked');t.set(p,{blocked:true});};blk();
tickers.push(()=>{c++;if(c%4===3){const s=p;t.set(s,{drain:true,blocked:false});later(D.drain,()=>t.clear(s));t.move('r',(p+1)%6);t.heads.w.classList.remove('blocked');t.sub('w','');}
else if(c%4===0){t.set(p,{val:'m'+(id++),on:true});p=(p+1)%6;t.move('w',p,p===0);blk();}});
t.move('w',0);t.move('r',0);}
{const f=card('3e','wrap','<span><b>exit · enter</b> 240ms</span><span><b>pulse</b> 700ms</span>');const t=new Tape(f,{n:6,sw:40,idx:idx6,lanes:[{key:'t',pos:'top'}],wrap:true,wrapLabel:'seq % 6'});t.head('h','t','W',W);
let i=0;tickers.push(()=>{i=(i+1)%6;t.move('h',i);});}
{const f=card('3f','transfer','<span><b>transfer</b> 700ms in-out</span><span><b>--state-error</b></span>');
const m=h('div','mini','<div class="core">core 0<em class="m">M</em></div><div class="trace"><span class="dot"></span><span class="x">line moves</span></div><div class="core">core 1<em class="i">I</em></div>');f.appendChild(m);
f.appendChild(h('div','foot','Only for coherence traffic. Never for ordinary data flow.'));
const ems=m.querySelectorAll('em'),dot=m.querySelector('.dot');let c=0;
tickers.push(()=>{if(c%2===0){const to=(c/2)%2===0?1:0;ems[to].className='m';ems[to].textContent='M';ems[1-to].className='i';ems[1-to].textContent='I';
dot.classList.add('nt');dot.style.left=to===1?'0':'calc(100% - 8px)';dot.style.opacity=1;reflow(dot);dot.classList.remove('nt');dot.style.left=to===1?'calc(100% - 8px)':'0';later(D.wrap,()=>dot.style.opacity=0);}c++;});}
})();

$('#bPause').onclick=e=>{paused=!paused;e.target.textContent=paused?'Play':'Pause';start();};
$('#bSpeed').onclick=e=>{K=K===1?4:1;document.documentElement.style.setProperty('--k',K);e.target.textContent='Speed '+(K===1?'1×':'¼×');start();};
$('#bTheme').onclick=e=>{const d=document.documentElement.dataset.theme==='dark';if(d)delete document.documentElement.dataset.theme;else document.documentElement.dataset.theme='dark';e.target.textContent=d?'Dark':'Light';};
tickers.forEach(f=>f());start();
})();
