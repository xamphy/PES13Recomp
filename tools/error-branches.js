// Analyses the "// ERROR <addr>" branches in generated/ (branches that leave their function in the
// middle of another one) and proposes a function size for config/pes13.toml.
//   node tools/error-branches.js            -> report
//   node tools/error-branches.js --toml     -> only the proposed `{ address, size }` lines
// Inputs: game/xex/default.image.bin (from pes13.exe --dump-image), generated/, config/.
'use strict';
const fs = require('fs');
const path = require('path');
const root = path.join(__dirname, '..');
const IMG_BASE = 0x82000000;
const img = fs.readFileSync(path.join(root, 'game/xex/default.image.bin'));
const word = (a) => img.readUInt32BE(a - IMG_BASE);
const hex = (n) => n.toString(16).toUpperCase().padStart(8, '0');

// --- sections / pdata ---
const pe = img.readUInt32LE(0x3c);
const nsec = img.readUInt16LE(pe + 6), optsz = img.readUInt16LE(pe + 20);
const sections = {};
for (let i = 0, s = pe + 24 + optsz; i < nsec; i++, s += 40)
    sections[img.toString('ascii', s, s + 8).replace(/\0/g, '')] = { va: IMG_BASE + img.readUInt32LE(s + 12), size: img.readUInt32LE(s + 8) };
const text = sections['.text'];
const pdata = new Map(); // begin -> length in bytes
for (let a = sections['.pdata'].va; a < sections['.pdata'].va + sections['.pdata'].size; a += 8) {
    const begin = word(a), data = word(a + 4);
    if (begin) pdata.set(begin, ((data >>> 8) & 0x3FFFFF) * 4);
}

// --- bl targets (real callees) ---
const blTargets = new Set();
for (let a = text.va; a < text.va + text.size; a += 4) {
    const w = word(a);
    if ((w >>> 26) === 18 && (w & 1) && !(w & 2)) blTargets.add((a + (((w & 0x3FFFFFC) << 6) >> 6)) >>> 0);
}

// --- config sizes, generated function starts, switch tables ---
const cfgText = fs.readFileSync(path.join(root, 'config/pes13.toml'), 'utf8');
const cfg = new Map();
for (const m of cfgText.matchAll(/address\s*=\s*0x([0-9A-Fa-f]+)\s*,\s*size\s*=\s*0x([0-9A-Fa-f]+)/g)) cfg.set(parseInt(m[1], 16), parseInt(m[2], 16));
const starts = [...fs.readFileSync(path.join(root, 'generated/ppc_func_mapping.cpp'), 'utf8').matchAll(/\{ 0x([0-9A-F]{8}), sub_/g)].map((m) => parseInt(m[1], 16)).sort((a, b) => a - b);
const startSet = new Set(starts);
const switches = new Map(); // bctr base -> labels
{
    const t = fs.readFileSync(path.join(root, 'config/pes13_switch_tables_all.toml'), 'utf8');
    for (const blk of t.split('[[switch]]').slice(1)) {
        const base = parseInt(/base\s*=\s*0x([0-9A-F]+)/i.exec(blk)[1], 16);
        const labels = [...blk.matchAll(/0x([0-9A-F]+),/gi)].map((m) => parseInt(m[1], 16));
        switches.set(base, labels);
    }
}
function nextStart(a) { // first generated function start > a
    let lo = 0, hi = starts.length;
    while (lo < hi) { const mid = (lo + hi) >> 1; if (starts[mid] <= a) lo = mid + 1; else hi = mid; }
    return starts[lo];
}
function containing(a) { let lo = 0, hi = starts.length; while (lo < hi) { const mid = (lo + hi) >> 1; if (starts[mid] <= a) lo = mid + 1; else hi = mid; } return starts[lo - 1]; }

// --- ERROR list from generated/ ---
const errors = new Map(); // func -> [targets]
for (const f of fs.readdirSync(path.join(root, 'generated')).filter((f) => /^ppc_recomp\.\d+\.cpp$/.test(f))) {
    let cur = null;
    for (const line of fs.readFileSync(path.join(root, 'generated', f), 'utf8').split('\n')) {
        let m = /^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]+)\)/.exec(line);
        if (m) { cur = parseInt(m[1], 16); continue; }
        m = /\/\/ ERROR ([0-9A-F]+)/.exec(line);
        if (m && cur !== null) { if (!errors.has(cur)) errors.set(cur, []); errors.get(cur).push(parseInt(m[1], 16)); }
    }
}

// A "real" function start: something other code calls or the exception table names.
const isReal = (a) => blTargets.has(a) || pdata.has(a);

// Flow extent from `start`: walk forward, following local branches (not calls, not into real
// functions), until an unconditional terminator with nothing pending beyond it.
function extent(start, limit) {
    let maxT = start, a = start, notes = [];
    for (; a < start + limit; a += 4) {
        if (a !== start && isReal(a) && a > maxT) { notes.push(`stops at real function ${hex(a)}`); return { end: a, notes }; }
        const w = word(a), op = w >>> 26;
        let term = false;
        if (op === 18 && !(w & 1)) { // b
            const t = (a + (((w & 0x3FFFFFC) << 6) >> 6)) >>> 0;
            if (!(w & 2) && t > start && t < start + limit && !isReal(t)) maxT = Math.max(maxT, t);
            term = true;
        } else if (op === 16 && !(w & 1)) { // bc
            const t = (a + (((w & 0xFFFC) << 16) >> 16)) >>> 0;
            if (!(w & 2) && t > start && t < start + limit && !isReal(t)) maxT = Math.max(maxT, t);
            if ((w >>> 21 & 0x14) === 0x14) term = true; // branch always
        } else if (op === 19) {
            const xo = (w >>> 1) & 0x3FF, bo = (w >>> 21) & 0x1F;
            if ((xo === 16 || xo === 528) && !(w & 1) && (bo & 0x14) === 0x14) term = true; // blr / bctr
            if (xo === 528 && switches.has(a)) for (const l of switches.get(a)) if (l > start && l < start + limit) maxT = Math.max(maxT, l);
        } else if (w === 0) { notes.push(`zero word at ${hex(a)}`); return { end: a, notes }; }
        if (term && a + 4 > maxT) return { end: a + 4, notes };
    }
    notes.push('hit limit');
    return { end: a, notes };
}

const toml = process.argv.includes('--toml');
const rows = [];
for (const [f, targets] of [...errors].sort((x, y) => x[0] - y[0])) {
    const curEnd = cfg.has(f) ? f + cfg.get(f) : pdata.has(f) ? f + pdata.get(f) : nextStart(f);
    const src = cfg.has(f) ? 'config' : pdata.has(f) ? 'pdata' : 'analyse';
    const uniq = [...new Set(targets)].sort((a, b) => a - b);
    const ext = extent(f, 0x4000);
    const covered = uniq.every((t) => t >= f && t < ext.end);
    const into = uniq.map((t) => { const c = containing(t); return `${hex(t)}${t < f ? '(before!)' : ''} in sub_${hex(c)}${isReal(c) ? '[real' + (pdata.has(c) ? ',pdata' : '') + (blTargets.has(c) ? ',bl' : '') + ']' : ''}`; });
    const swallowed = starts.filter((s) => s > f && s < ext.end && isReal(s)).map(hex);
    rows.push({ f, src, size: curEnd - f, newSize: ext.end - f, covered, into, notes: ext.notes, swallowed, n: targets.length });
}
if (toml) {
    for (const r of rows) if (r.covered && r.newSize > r.size) console.log(`    { address = 0x${hex(r.f)}, size = 0x${r.newSize.toString(16).toUpperCase()} },  # was 0x${r.size.toString(16).toUpperCase()} (${r.src}), ${r.n} ERROR branches`);
} else {
    for (const r of rows) {
        console.log(`sub_${hex(r.f)} ${r.src} size 0x${r.size.toString(16)} -> 0x${r.newSize.toString(16)} ${r.covered ? 'OK' : 'NOT COVERED'} (${r.n} errors)${r.notes.length ? ' [' + r.notes.join('; ') + ']' : ''}`);
        for (const i of r.into) console.log('    ' + i);
        if (r.swallowed.length) console.log('    swallows real: ' + r.swallowed.join(' '));
    }
    console.log(`${rows.length} functions, ${rows.filter((r) => r.covered).length} covered by the flow extent`);
}

// --- classification (appended) ---
if (process.argv.includes('--classify')) {
    const pd = [...pdata].sort((a, b) => a[0] - b[0]);
    const inPdata = (a) => { for (const [s, l] of pd) if (a >= s && a < s + l) return [s, l]; return null; };
    // references to F as a 32-bit word in data (function pointers, scope tables)
    const dataRefs = (a) => { let n = 0; for (let o = 0; o + 4 <= img.length; o += 4) if (img.readUInt32BE(o) === a) n++; return n; };
    for (const [f, targets] of [...errors].sort((x, y) => x[0] - y[0])) {
        const p = inPdata(f);
        const uniq = [...new Set(targets)];
        const tp = uniq.map((t) => { const q = inPdata(t); return q ? hex(q[0]) + '+' + (q[1]).toString(16) : 'none'; });
        console.log(`sub_${hex(f)} real=${isReal(f) ? (pdata.has(f) ? 'pdata' : '') + (blTargets.has(f) ? 'bl' : '') : 'no'} dataRefs=${dataRefs(f)} inside=${p ? hex(p[0]) + '+' + p[1].toString(16) : 'none'} cfg=${cfg.has(f)} n=${targets.length} targets-in=${tp.join(',')}`);
    }
}

// --- disassembly of a range from the generated comments: --dis <start> <end> ---
{
    const i = process.argv.indexOf('--dis');
    if (i > 0) {
        const s = parseInt(process.argv[i + 1], 16), e = parseInt(process.argv[i + 2], 16);
        const mn = new Map();
        for (const f of fs.readdirSync(path.join(root, 'generated')).filter((f) => /^ppc_recomp\.\d+\.cpp$/.test(f))) {
            let a = null;
            for (const line of fs.readFileSync(path.join(root, 'generated', f), 'utf8').split('\n')) {
                const m = /^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]+)\)/.exec(line);
                if (m) { a = parseInt(m[1], 16); if (a > e || a < s - 0x4000) a = null; continue; }
                if (a === null) continue;
                const c = /^\t\/\/ (?!ERROR)(.*)$/.exec(line);
                if (c) { if (!mn.has(a)) mn.set(a, c[1]); a += 4; }
            }
        }
        for (let a = s; a < e; a += 4) {
            const tags = [startSet.has(a) ? 'FUNC' : '', pdata.has(a) ? 'PDATA+' + pdata.get(a).toString(16) : '', blTargets.has(a) ? 'BL' : '', cfg.has(a) ? 'CFG+' + cfg.get(a).toString(16) : '', switches.has(a) ? 'SWITCH' : ''].filter(Boolean).join(' ');
            console.log(`${hex(a)}  ${hex(word(a))}  ${(mn.get(a) || '?').padEnd(34)} ${tags}`);
        }
    }
}
