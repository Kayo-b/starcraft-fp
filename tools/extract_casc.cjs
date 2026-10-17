// Read-only CASC extraction. Truncates each buffer to the CASC-reported size
// (bw-casclib 2.0.2 over-allocates buffers by sizeof(void*) on 64-bit).
const c=require("bw-casclib"),fs=require("fs"),path=require("path"),cr=require("crypto");
const P="/home/kxyx/Games/battlenet/drive_c/Program Files (x86)/StarCraft";
const OUT=path.join(__dirname,"extracted");
const titanList=[...fs.readFileSync("../titan-reactor/src/openbw/extra/filepaths.ts","utf8").matchAll(/"((?:[^"\\]|\\\\)+)"/g)].map(m=>m[1].replace(/\\\\/g,"\\"));
const wanted=new Set(titanList.map(s=>s.toLowerCase()));
for(const n of ["arr\\sfxdata.dat","arr\\sfxdata.tbl","triggers\\melee.trg","scripts\\iscript.bin"]) wanted.add(n);
const extra=[/^tileset\\[^\\]+\.(cv5|vf4|vr4|vx4ex|wpe)$/i,/^tileset\\[^\\]+\\dark\.pcx$/i];
(async()=>{
 const h=await c.openStorage(P);
 const all=await c.findFiles(h,"*");
 const sel=all.filter(x=>wanted.has(x.fullName.toLowerCase())||extra.some(r=>r.test(x.fullName)));
 const found=new Set(sel.map(x=>x.fullName.toLowerCase()));
 const missing=[...wanted].filter(n=>!found.has(n));
 const man=[],bad=[];
 for(const f of sel){
  const b=await c.readFile(h,f.fullName);
  if(b.length!==f.fileSize && b.length!==f.fileSize*8){bad.push(`${f.fullName}: ${b.length} vs ${f.fileSize}`);continue}
  const d=b.subarray(0,f.fileSize);
  const o=path.join(OUT,...f.fullName.split("\\"));
  fs.mkdirSync(path.dirname(o),{recursive:true});fs.writeFileSync(o,d);
  man.push({name:f.fullName,size:d.length,sha256:cr.createHash("sha256").update(d).digest("hex")});
 }
 fs.writeFileSync(path.join(OUT,"manifest.json"),JSON.stringify(man,null,1));
 console.log("casc entries",all.length,"requested",wanted.size,"extracted",man.length,"unexpected-size",bad.length,"not-in-casc",missing.length);
 console.log("bad",bad.slice(0,5));console.log("missing sample",missing.slice(0,15));
 console.log("total bytes",man.reduce((a,m)=>a+m.size,0));
 c.closeStorage(h);
})().catch(e=>console.error("FAIL",e.message));
