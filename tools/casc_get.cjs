// Read-only CASC extraction by regex. Usage: node casc_get.cjs <install-dir> <outdir> <regex> [--list]
// NOTE: bw-casclib@2.0.2 returns buffers of fileSize*8 bytes on 64-bit (only the first fileSize are valid);
// every file is truncated to the size reported by findFiles(). Resolve bw-casclib from NODE_PATH or cwd.
const c = require(require.resolve("bw-casclib", { paths: [process.cwd(), ...(process.env.NODE_PATH||"").split(":")] }));
const fs = require("fs"), path = require("path");
const [install, out, rx, flag] = process.argv.slice(2);
if (!install || !out || !rx) { console.error("usage: casc_get.cjs <install-dir> <outdir> <regex> [--list]"); process.exit(2); }
(async () => {
  const h = await c.openStorage(install);
  const re = new RegExp(rx, "i");
  const sel = (await c.findFiles(h, "*")).filter(f => re.test(f.fullName));
  if (flag === "--list") { for (const f of sel.slice(0, 40)) console.log(f.fileSize, f.fullName); console.log(sel.length, "matches"); c.closeStorage(h); return; }
  let n = 0, bytes = 0;
  for (const f of sel) {
    const b = await c.readFile(h, f.fullName);
    if (b.length !== f.fileSize && b.length !== f.fileSize * 8) { console.error("unexpected size", f.fullName, b.length, f.fileSize); continue; }
    const o = path.join(out, ...f.fullName.split("\\")); fs.mkdirSync(path.dirname(o), { recursive: true });
    fs.writeFileSync(o, b.subarray(0, f.fileSize)); n++; bytes += f.fileSize;
  }
  console.log("extracted", n, "files,", bytes, "bytes to", out);
  c.closeStorage(h);
})().catch(e => { console.error("FAIL", e.message); process.exit(1); });
