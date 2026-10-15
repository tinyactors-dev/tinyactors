(()=>{
const $=s=>document.querySelector(s);
const h=(tag,cls,html)=>{const e=document.createElement(tag);if(cls)e.className=cls;if(html!=null)e.innerHTML=html;return e;};
if(matchMedia('(prefers-reduced-motion: reduce)').matches)document.body.classList.add('rm');
let paused=false;const loops=[];
const every=(ms,fn)=>{loops.push({ms,fn,t:0});};
setInterval(()=>{if(paused)return;loops.forEach(l=>{l.t+=100;if(l.t>=l.ms){l.t=0;l.fn();}});},100);

/* 1 vellum */
(()=>{const root=$('#v1');const base=h('div','base');const row=h('div','row8');const cells=[];
for(let i=0;i<8;i++){const c=h('div','cell');row.appendChild(c);cells.push(c);}base.appendChild(row);root.appendChild(base);
const sheet=h('div','sheet');const span=h('div','span');const w=h('div','hd w','<span>W</span><span class="tri"></span>'),r=h('div','hd r','<span class="tri"></span><span>R</span>');
sheet.append(span,w,r);root.appendChild(sheet);
let wi=5,ri=1;const X=i=>14+24+i*44;
const draw=()=>{cells.forEach((c,i)=>{const used=((i-ri+8)%8)<((wi-ri+8)%8);c.className='cell'+(used?' u':'');c.textContent=used?'m'+i:'';});
w.style.transform=`translateX(${X(wi)-10}px)`;r.style.transform=`translateX(${X(ri)-10}px)`;
const n=(wi-ri+8)%8;if(ri+n<=8){span.style.display='';span.style.left=(X(ri)-12)+'px';span.style.width=(n*44)+'px';}else{span.style.display='none';}};
draw();every(1100,()=>{if(Math.random()<.55)wi=(wi+1)%8;else if(ri!==wi)ri=(ri+1)%8;if((wi+1)%8===ri)ri=(ri+1)%8;draw();});})();

/* 2 registration */
(()=>{const root=$('#v2');const gens=[['gen 0','crashed','var(--state-error)',[40,30,60]],['gen 1','crashed','var(--state-error)',[40,30,70,20]],['gen 2','paying','var(--state-running)',[40,30,70,36,24]]];
gens.forEach((g,i)=>{const d=h('div','reg'+(i<2?' old':''),`<div class="nm">checkout.1</div><div class="st"><i style="background:${g[2]}"></i>${g[1]}</div><span class="gen">${g[0]}</span><div class="bars">${g[3].map(w=>`<span style="width:${w}px"></span>`).join('')}</div>`);d.style.setProperty('--i',2-i);root.appendChild(d);});})();

/* 4 hierarchy */
(()=>{const root=$('#v4');const T=[['register','0.3 ns',.3],['L1','1 ns',1],['L2','4 ns',4],['L3','15 ns',15],['RAM','100 ns',100],['disk','100 µs',1e5]];
const rows=T.map((t,i)=>{const s=1-i*.07,o=1-i*.13;const r=h('div','tier',`<span class="nm">${t[0]}</span><span class="tk"><span class="dot"></span></span><span class="lt">${t[1]}</span>`);
r.style.transform=`scale(${s})`;r.style.opacity=o;r.style.color=`color-mix(in oklch,var(--fg-1) ${100-i*12}%,var(--bg-app))`;root.appendChild(r);return r;});
let k=0;every(1600,()=>{const tgt=[1,2,4,3,1,5][k++%6];rows.forEach((r,i)=>{const d=r.querySelector('.dot');d.style.transition='none';d.style.opacity=0;d.style.left='-4px';});
const d=rows[tgt].querySelector('.dot');const dur=200+Math.log10(T[tgt][2]*10)*180;d.offsetWidth;d.style.transition=`left ${dur}ms var(--e-move),opacity 150ms`;d.style.opacity=1;d.style.left='calc(100% - 4px)';
rows.forEach((r,i)=>{r.style.opacity=i===tgt?1:Math.max(.25,1-i*.13);});
setTimeout(()=>{d.style.opacity=0;rows.forEach((r,i)=>r.style.opacity=1-i*.13);},dur+250);});})();

/* 5 depth of field */
(()=>{const root=$('#v5');const A=[['gateway','running','var(--state-running)'],['auth','waiting','var(--state-waiting)'],['checkout.1','running','var(--state-running)'],['checkout.2','done','var(--state-done)'],['ledger','running','var(--state-running)'],['mailer','stopped','var(--state-stopped)'],['indexer','waiting','var(--state-waiting)'],['billing','running','var(--state-running)']];
const els=A.map(a=>{const e=h('div','act',`<span class="nm">${a[0]}</span><span class="st"><i></i>${a[1]}</span>`);e.style.setProperty('--c',a[2]);root.appendChild(e);return e;});
const path=[0,1,2,4,7,2,3];let k=0;const go=()=>{const f=path[k++%path.length];els.forEach((e,i)=>{e.classList.toggle('foc',i===f);e.classList.toggle('blur',i!==f);});};go();every(1800,go);
root.addEventListener('mouseover',e=>{const a=e.target.closest('.act');if(!a)return;els.forEach(x=>{x.classList.toggle('foc',x===a);x.classList.toggle('blur',x!==a);});});})();

/* 6 lift */
(()=>{const root=$('#v6');const B=[{id:'gw',n:'gateway',x:40,y:40},{id:'rt',n:'router',x:250,y:110},{id:'db',n:'ledger',x:40,y:180},{id:'q',n:'outbox',x:420,y:40}];
const W=[['gw','rt'],['db','rt'],['rt','q']];const off={};
const svg=document.createElementNS('http://www.w3.org/2000/svg','svg');root.appendChild(svg);
const port=(b,side)=>{const o=off[b.id]||[0,0];return side==='r'?[b.x+132+o[0],b.y+31+o[1]]:[b.x+o[0],b.y+31+o[1]];};
const paths=W.map(()=>{const p=document.createElementNS('http://www.w3.org/2000/svg','path');svg.appendChild(p);return p;});
const by=Object.fromEntries(B.map(b=>[b.id,b]));
const pads=[];
const draw=()=>{W.forEach(([a,b],i)=>{const A=port(by[a],'r'),Z=port(by[b],'l');const mx=(A[0]+Z[0])/2;paths[i].setAttribute('d',`M${A[0]} ${A[1]} L${mx} ${A[1]} L${mx} ${Z[1]} L${Z[0]} ${Z[1]}`);});
pads.forEach(p=>{const pt=port(by[p.b],p.s);p.el.style.left=pt[0]+'px';p.el.style.top=pt[1]+'px';});};
B.forEach(b=>{const e=h('div','blk6',`<span class="nm">${b.n}</span>`);e.style.left=b.x+'px';e.style.top=b.y+'px';root.appendChild(e);
e.onmouseenter=()=>{e.classList.add('up');off[b.id]=[-4,-8];draw();};e.onmouseleave=()=>{e.classList.remove('up');off[b.id]=[0,0];draw();};
['l','r'].forEach(s=>{if(W.some(([a,z])=>(s==='r'&&a===b.id)||(s==='l'&&z===b.id))){const p=h('span','pad6');root.appendChild(p);pads.push({b:b.id,s,el:p});}});});
draw();
let k=0;every(2200,()=>{if(root.matches(':hover'))return;const els=[...root.querySelectorAll('.blk6')];els.forEach(e=>e.classList.remove('up'));B.forEach(b=>off[b.id]=[0,0]);const i=[1,0,3,2][k++%4];els[i].classList.add('up');off[B[i].id]=[-4,-8];draw();});})();

/* 7 axo */
(()=>{const root=$('#v7');const axo=h('div','axo');root.appendChild(axo);
const L=[{n:'padding',c:'pad',f:[['',8]]},{n:'payload',f:[['flags',2,3],['',6,'pad'],['mailbox',8,4]]},{n:'header',f:[['state',1,1],['',7,'pad'],['id',8,2]]}];
axo.appendChild(Object.assign(h('div','ly sh'),{}));
L.forEach((l,i)=>{const d=h('div','ly');d.style.setProperty('--i',i);d.style.gridTemplateColumns=l.c==='pad'?'1fr':l.f.map(f=>f[1]+'fr').join(' ');
if(l.c==='pad'){d.style.background='var(--hatch),var(--surface-1)';d.innerHTML='<i style="color:var(--fg-4)">8 B tail</i>';}
else{d.style.background='var(--surface-1)';d.innerHTML=l.f.map(f=>`<i style="background:${f[2]==='pad'?'var(--hatch),var(--surface-1)':`color-mix(in oklch,var(--data-${f[2]}) 38%,var(--surface-1))`}">${f[0]}</i>`).join('');}
axo.appendChild(d);});
[['header · 16 B','28%'],['payload · 16 B','46%'],['padding · 8 B','64%']].forEach(([t,y])=>{const e=h('span','lead',t);e.style.top=y;root.appendChild(e);});
let auto=true;root.onclick=()=>{auto=false;root.classList.toggle('ex');};every(3200,()=>{if(auto)root.classList.toggle('ex');});})();

/* 8 stack */
(()=>{const root=$('#v8');let id=1;const msgs=[];const types=['PAY','INC','SNAP','PING','STOP','ACK'];
const mk=()=>{const t=types[id%types.length];const e=h('div','msg',`<span class="mono"><span>${t}</span><span style="color:var(--fg-3)">#${id}</span></span><span class="bar" style="width:${30+((id*37)%60)}%"></span>`);id++;return e;};
const lay=()=>msgs.forEach((m,i)=>m.style.setProperty('--i',i));
for(let i=0;i<5;i++){const m=mk();msgs.push(m);}msgs.reverse().forEach(m=>root.prepend(m));msgs.reverse();
const order=()=>{msgs.forEach((m,i)=>m.style.zIndex=100-i);};lay();order();
let tick=0;every(1300,()=>{tick++;if(tick%2){const m=mk();m.classList.add('in');m.style.setProperty('--i',0);root.appendChild(m);msgs.unshift(m);order();m.offsetWidth;m.classList.remove('in');lay();if(msgs.length>6){const o=msgs.pop();o.classList.add('out');setTimeout(()=>o.remove(),700);}}
else if(msgs.length>3){const o=msgs.pop();o.classList.add('out');setTimeout(()=>o.remove(),700);}});})();

/* 9 board */
(()=>{const root=$('#v9');const B=[{n:'gateway',t:'C · epoll',x:30,y:30,w:120,h:56},{n:'router',t:'actor',x:200,y:90,w:110,h:56},{n:'ring',t:'SPSC · 4 KB',x:200,y:170,w:110,h:40,mem:1},{n:'ledger',t:'actor',x:330,y:20,w:96,h:56}];
const svg=document.createElementNS('http://www.w3.org/2000/svg','svg');root.appendChild(svg);
const P=['M150 58 L175 58 L175 118 L200 118','M310 118 L318 118 L318 48 L330 48','M255 146 L255 170'];
P.forEach(d=>{['','fl'].forEach(c=>{const p=document.createElementNS('http://www.w3.org/2000/svg','path');p.setAttribute('d',d);if(c)p.setAttribute('class',c);svg.appendChild(p);});});
B.forEach(b=>{const e=h('div','b9'+(b.mem?' mem':''),`<div class="s"></div><div class="w"></div><div class="e"></div><div class="t"><span class="mono">${b.n}</span><span class="sm">${b.t}</span></div>`);Object.assign(e.style,{left:b.x+'px',top:b.y+'px',width:b.w+'px',height:b.h+'px'});root.appendChild(e);});})();

/* 10 orbit */
(()=>{const stage=$('#v10'),obj=$('#obj');const S=9,H=46,D=46;
const F=[['state',1,1],['',7,0],['id',8,2],['flags',2,3],['',6,0],['mailbox',8,4],['refs',4,6],['prio',1,5],['',3,0]];
const total=F.reduce((a,f)=>a+f[1],0);let x=-total*S/2;
const face=(w,hh,tf,bg,txt)=>{const i=h('i','',txt||'');Object.assign(i.style,{width:w+'px',height:hh+'px',transform:tf,background:bg});return i;};
F.forEach(f=>{const w=f[1]*S;const bg=f[2]?`color-mix(in oklch,var(--data-${f[2]}) 42%,var(--surface-1))`:'var(--hatch),var(--surface-1)';const bg2=f[2]?`color-mix(in oklch,var(--data-${f[2]}) 62%,var(--surface-2))`:'var(--hatch),var(--surface-2)';
const c=h('div','cub');c.style.transform=`translate3d(${x}px,${-H/2}px,${-D/2}px)`;
c.append(face(w,H,`translateZ(${D}px)`,bg,w>30?f[0]:''),face(w,H,`rotateY(180deg)`,bg),face(D,H,`rotateY(-90deg)`,bg2),face(D,H,`translateX(${w}px) rotateY(90deg) translateX(-${D}px)`.replace('translateX(-','translateX(-'),bg2),face(w,D,`rotateX(90deg)`,bg2),face(w,D,`translateY(${H}px) rotateX(-90deg) translateY(-${D}px)`,bg2));
const ff=c.children;ff[2].style.transformOrigin='left center';ff[2].style.transform='rotateY(90deg)';ff[3].style.transformOrigin='left center';ff[3].style.transform=`translateX(${w}px) rotateY(90deg)`;
ff[4].style.transformOrigin='top center';ff[4].style.transform='rotateX(-90deg)';ff[5].style.transformOrigin='top center';ff[5].style.transform=`translateY(${H}px) rotateX(-90deg)`;
ff[0].style.transform=`translateZ(${D}px)`;ff[1].style.transform='none';
obj.appendChild(c);x+=w;});
let rx=-24,ry=-32,drag=null,idle=true;const set=()=>obj.style.transform=`rotateX(${rx}deg) rotateY(${ry}deg)`;set();
stage.addEventListener('pointerdown',e=>{drag=[e.clientX,e.clientY,rx,ry];idle=false;stage.setPointerCapture(e.pointerId);});
stage.addEventListener('pointermove',e=>{if(!drag)return;ry=drag[3]+(e.clientX-drag[0])*.5;rx=Math.max(-80,Math.min(80,drag[2]-(e.clientY-drag[1])*.5));set();});
stage.addEventListener('pointerup',()=>{drag=null;setTimeout(()=>idle=true,2500);});
let last=performance.now();const spin=t=>{const dt=t-last;last=t;if(idle&&!paused&&!document.body.classList.contains('rm')){ry+=dt*.012;set();}requestAnimationFrame(spin);};requestAnimationFrame(spin);})();

/* 11 parallax */
(()=>{const el=$('#v11');const upd=()=>{el.style.backgroundPosition=`0 ${-el.scrollTop*.5}px`;};el.addEventListener('scroll',upd);upd();
let dir=1,hover=false;el.onmouseenter=()=>hover=true;el.onmouseleave=()=>hover=false;
const step=()=>{if(!hover&&!paused&&!document.body.classList.contains('rm')){const max=el.scrollHeight-el.clientHeight;el.scrollTop+=dir*.5;if(el.scrollTop>=max-1)dir=-1;if(el.scrollTop<=0)dir=1;}requestAnimationFrame(step);};requestAnimationFrame(step);})();

/* 12 dive */
(()=>{const root=$('#v12');const NS='http://www.w3.org/2000/svg';
const crumb=h('div','crumb','<button id="c0">system</button><span id="c1" style="display:none">/ <span style="color:var(--fg-1)">ingest</span></span>');root.appendChild(crumb);
const l0=h('div','dv0'),l1=h('div','dv1');root.append(l0,l1);
const blk=(p,b,cls)=>{const e=h('div','b12'+(cls?' '+cls:''),`<span class="mono">${b.n}</span><span class="sm">${b.t}</span>`);Object.assign(e.style,{left:b.x+'px',top:b.y+'px',width:b.w+'px',height:b.h+'px'});p.appendChild(e);return e;};
const wires=(p,ds)=>{const s=document.createElementNS(NS,'svg');ds.forEach(d=>{const q=document.createElementNS(NS,'path');q.setAttribute('d',d);s.appendChild(q);});p.prepend(s);};
const sx=x=>`calc(50% + ${x}px)`;
const S=[{n:'clients',t:'external',x:-240,y:110,w:110,h:56},{n:'ingest',t:'container · C',x:-60,y:110,w:120,h:56},{n:'ledger',t:'container',x:130,y:60,w:110,h:56},{n:'archive',t:'store',x:130,y:170,w:110,h:56}];
const els=S.map(b=>{const e=blk(l0,{...b,x:0});e.style.left=sx(b.x);return e;});els[0].style.borderStyle='dashed';
const C=[{n:'accept',t:'epoll loop',x:-220,y:70,w:110,h:50},{n:'parse',t:'actor',x:-60,y:70,w:110,h:50},{n:'ring',t:'SPSC · 4 KB',x:-60,y:170,w:110,h:44},{n:'route',t:'actor',x:110,y:70,w:110,h:50}];
let w=root.getBoundingClientRect().width||520;
const cx=x=>(w/2)+x;
const draw=()=>{l0.querySelector('svg')?.remove();l1.querySelector('svg')?.remove();w=root.getBoundingClientRect().width||520;
wires(l0,[`M${cx(-130)} 138 L${cx(-60)} 138`,`M${cx(60)} 138 L${cx(95)} 138 L${cx(95)} 88 L${cx(130)} 88`,`M${cx(60)} 138 L${cx(95)} 138 L${cx(95)} 198 L${cx(130)} 198`]);

wires(l1,[`M${cx(-110)} 95 L${cx(-60)} 95`,`M${cx(50)} 95 L${cx(110)} 95`,`M${cx(-5)} 120 L${cx(-5)} 170`]);};
C.forEach(b=>{const e=blk(l1,{...b,x:0},'cmp');e.style.left=sx(b.x);});
draw();new ResizeObserver(draw).observe(root);
l1.prepend(h('div','zone12'));const tgt=els[1];
const dive=()=>{l0.style.transformOrigin=`${cx(0)}px 138px`;l1.style.transformOrigin=`${cx(0)}px 138px`;l0.classList.add('away');l1.classList.add('here');$('#c1').style.display='';};
const up=()=>{l0.classList.remove('away');l1.classList.remove('here');$('#c1').style.display='none';};
let auto=true;tgt.onclick=()=>{auto=false;dive();};$('#c0').onclick=()=>{auto=false;up();};
let k=0;every(3400,()=>{if(!auto)return;(k++%2?up:dive)();});})();

$('#bPause').onclick=e=>{paused=!paused;e.target.textContent=paused?'Play':'Pause';document.querySelectorAll('.board path.fl').forEach(p=>p.style.animationPlayState=paused?'paused':'running');};
$('#bTheme').onclick=e=>{const d=document.documentElement.dataset.theme==='dark';if(d)delete document.documentElement.dataset.theme;else document.documentElement.dataset.theme='dark';e.target.textContent=d?'Dark':'Light';};
})();
