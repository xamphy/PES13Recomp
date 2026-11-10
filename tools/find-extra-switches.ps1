# Finds jump tables XenonAnalyse skipped and writes them as [[switch]] entries:
#   - absolute tables where the compiler scheduled `rlwinm` between lis/addi and jumps via r0
#   - byte/short offset tables whose range check is in the *caller* (before the bl)
# Table length comes from, in order: a nearby `cmplwi idx,N ; bgt/ble`, the largest
# such check in any caller, or (last resort) reading entries until a target leaves the
# containing function. Also reports functions whose case targets run past their
# current boundary, as candidate `functions = [...]` entries.
#
#   powershell -ExecutionPolicy Bypass -File tools\find-extra-switches.ps1 <decrypted image.bin>
# Output: config\pes13_switch_tables_extra.toml
param([Parameter(Mandatory)][string]$Image)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $MyInvocation.MyCommand.Path -Parent) -Parent

Add-Type -Language CSharp -TypeDefinition @'
using System; using System.IO; using System.Linq; using System.Collections.Generic; using System.Text; using System.Text.RegularExpressions;
public static class ExtraSwitches2 {
  const uint Base = 0x82000000, Lo = 0x821B0000, Hi = 0x83254A00;
  static byte[] Img;
  static uint W(uint va) { long o = va - Base; return (uint)(Img[o] << 24 | Img[o+1] << 16 | Img[o+2] << 8 | Img[o+3]); }
  static uint B8(uint va) { return Img[va - Base]; }
  static uint H16(uint va) { long o = va - Base; return (uint)(Img[o] << 8 | Img[o+1]); }
  static uint Op(uint w) { return w >> 26; }
  static uint Xo(uint w) { return (w >> 1) & 0x3FF; }
  static uint RT(uint w) { return (w >> 21) & 31; }   // also RS
  static uint RA(uint w) { return (w >> 16) & 31; }
  static uint RB(uint w) { return (w >> 11) & 31; }
  static bool IsNop(uint w) { return w == 0x60000000; }
  static bool IsLis(uint w) { return Op(w) == 15 && RA(w) == 0; }
  static bool IsAddi(uint w) { return Op(w) == 14; }
  static bool IsRlwinm(uint w) { return Op(w) == 21; }
  static bool IsMtctr(uint w) { return (w & 0xFC1FFFFF) == 0x7C0903A6; }
  static uint Next(ref uint a) { while (IsNop(W(a))) a += 4; uint w = W(a); a += 4; return w; }

  // cmplwi crF,rIdx,K followed (within a few insns) by bgt/ble crF -> K. Scans backwards from `from`.
  static int FindBound(uint from, uint idxReg, int window) {
    int crField = -1;
    for (int i = 1; i <= window; i++) {
      uint a = from - (uint)(i * 4); if (a < Lo) break;
      uint w = W(a);
      if (crField < 0 && Op(w) == 16) {                     // bc
        uint bo = RT(w), bi = RA(w);
        bool bgt = (bo & 0x1C) == 0x0C && (bi & 3) == 1;    // branch if gt set
        bool ble = (bo & 0x1C) == 0x04 && (bi & 3) == 1;    // branch if gt clear
        if (bgt || ble) crField = (int)(bi >> 2);
      }
      else if (crField >= 0 && Op(w) == 10 && (RT(w) >> 2) == crField && RA(w) == idxReg)
        return (int)(w & 0xFFFF);
      if (w == 0x4E800020) break;                           // blr: left the block
    }
    return -1;
  }

  // At a call site: the constant loaded into rIdx before the bl (li rIdx,K), if any.
  static int FindConstantArg(uint callSite, uint idxReg) {
    for (int i = 1; i <= 16; i++) {
      uint a = callSite - (uint)(i * 4); if (a < Lo) break;
      uint w = W(a);
      if (Op(w) == 14 && RA(w) == 0 && RT(w) == idxReg) return (short)(w & 0xFFFF);   // li rIdx,K
      // rIdx written by something other than li -> not a constant
      if (Op(w) == 14 && RT(w) == idxReg) return -1;                                   // addi rIdx,rX,K
      if (Op(w) == 31 && Xo(w) == 444 && RA(w) == idxReg) return -1;                   // mr/or rIdx,...
      if (Op(w) >= 32 && Op(w) <= 43 && (Op(w) & 1) == 0 && Op(w) != 36 && Op(w) != 38 && RT(w) == idxReg) return -1; // lwz/lbz/lhz/lha rIdx
      if (Op(w) == 31 && (Xo(w) == 23 || Xo(w) == 87 || Xo(w) == 279) && RT(w) == idxReg) return -1; // indexed loads
      if ((Op(w) == 21 || Op(w) == 24 || Op(w) == 28) && RA(w) == idxReg) return -1;   // rlwinm/ori/andi. into rIdx
      if ((w & 0xFC000001) == 0x48000001 || w == 0x4E800020) break;                    // previous call / return
    }
    return -1;
  }

  public static string Run(string imagePath, string tablesPath, string mapPath) {
    Img = File.ReadAllBytes(imagePath);
    var known = new HashSet<uint>();
    foreach (Match m in Regex.Matches(File.ReadAllText(tablesPath), @"base = 0x([0-9A-F]+)")) known.Add(Convert.ToUInt32(m.Groups[1].Value, 16));
    var funcs = new List<uint>();
    foreach (Match m in Regex.Matches(File.ReadAllText(mapPath), @"\{ 0x([0-9A-F]+),")) funcs.Add(Convert.ToUInt32(m.Groups[1].Value, 16));
    funcs.Sort();

    // bl call sites per target (for caller-side bounds)
    var callers = new Dictionary<uint, List<uint>>();
    for (uint a = Lo; a < Hi; a += 4) {
      uint w = W(a);
      if ((w & 0xFC000003) != 0x48000001) continue;
      int off = (int)(w & 0x03FFFFFC); if ((off & 0x02000000) != 0) off -= 0x04000000;
      uint t = (uint)(a + off);
      List<uint> l; if (!callers.TryGetValue(t, out l)) callers[t] = l = new List<uint>(); l.Add(a);
    }

    var sb = new StringBuilder(); var report = new StringBuilder(); var fnFix = new StringBuilder();
    sb.AppendLine("# Generated by tools/find-extra-switches.ps1 - jump tables XenonAnalyse skipped.");
    int found = 0;

    for (uint a = Lo; a < Hi - 0x40; a += 4) {
      if (known.Contains(a)) continue;
      uint p = a;
      uint w = Next(ref p); if (!IsLis(w) || RT(w) != 12) continue;
      uint tableHi = (w & 0xFFFF) << 16;
      uint idxReg = 99, preShiftSrc = 99;
      w = Next(ref p);
      if (IsRlwinm(w)) { preShiftSrc = RT(w); w = Next(ref p); }          // rlwinm r0,rIdx,n scheduled before addi
      if (!IsAddi(w) || RT(w) != 12 || RA(w) != 12) continue;
      uint table = (uint)(tableHi + (short)(w & 0xFFFF));

      string kind; uint shift = 0;
      w = Next(ref p);
      if (Op(w) == 31 && Xo(w) == 87 && RA(w) == 12) {                    // lbzx r0,r12,rIdx
        idxReg = RB(w); kind = "byte";
        uint q = p; uint w2 = Next(ref q);
        if (IsRlwinm(w2)) { shift = (w2 >> 11) & 31; kind = "computed"; p = q; }
      } else if (IsRlwinm(w)) {                                            // rlwinm r0,rIdx,n ; l?zx r0,r12,r0
        idxReg = RT(w);
        w = Next(ref p);
        if (Op(w) == 31 && Xo(w) == 23) { kind = "absolute"; }
        else if (Op(w) == 31 && Xo(w) == 279) { kind = "short"; }
        else continue;
      } else if (Op(w) == 31 && (Xo(w) == 23 || Xo(w) == 279) && preShiftSrc != 99) {
        idxReg = preShiftSrc;
        if (Xo(w) == 23) { kind = "absolute"; } else { kind = "short"; }
      } else continue;

      uint labelBase = 0;
      w = Next(ref p);
      if (kind != "absolute") {
        if (!IsLis(w) || RT(w) != 12) continue;
        uint hiB = (w & 0xFFFF) << 16;
        w = Next(ref p); if (!IsAddi(w) || RT(w) != 12) continue;
        labelBase = (uint)(hiB + (short)(w & 0xFFFF));
        w = Next(ref p); if (!(Op(w) == 31 && Xo(w) == 266)) continue; // add
        w = Next(ref p);
      }
      if (!IsMtctr(w)) continue;
      if (Next(ref p) != 0x4E800420) continue;                           // bctr

      int fi = funcs.BinarySearch(a); if (fi < 0) fi = ~fi - 1;
      uint fStart = funcs[fi], fEnd = fi + 1 < funcs.Count ? funcs[fi + 1] : Hi;

      // Table length
      string how; int bound = FindBound(a, idxReg, 12);
      if (bound >= 0) how = "local bound";
      else {
        // Every caller must constrain the index (range check or constant argument).
        List<uint> cs; bool allKnown = false;
        if (callers.TryGetValue(fStart, out cs)) {
          allKnown = true;
          foreach (var c in cs) {
            int b = FindBound(c, idxReg, 24);
            if (b < 0) b = FindConstantArg(c, idxReg);
            if (b < 0) allKnown = false;
            bound = Math.Max(bound, b);
          }
        }
        how = bound < 0 ? "estimated" : allKnown ? "caller bound" : "partial caller bound";
      }

      Func<uint, uint> target = i => kind == "absolute" ? W(table + i * 4)
                                  : kind == "short" ? labelBase + H16(table + i * 2)
                                  : labelBase + (B8(table + i) << (int)shift);
      var labels = new List<uint>();
      if (bound >= 0) for (uint i = 0; i <= (uint)bound; i++) labels.Add(target(i));
      else for (uint i = 0; i < 256; i++) { uint t = target(i); if (t < fStart || t >= fEnd || (t & 3) != 0) break; labels.Add(t); }
      if (labels.Count == 0) { report.AppendLine(string.Format("  SKIPPED {0:X8} ({1}) in sub_{2:X8}: no bound and no in-function targets", a, kind, fStart)); continue; }

      found++;
      uint maxT = labels.Max();
      report.AppendLine(string.Format("  {0:X8} {1,-8} sub_{2:X8} r{3,-2} {4,3} entries ({5}){6}", a, kind, fStart, idxReg, labels.Count, how,
        maxT >= fEnd ? string.Format("  ** targets reach 0x{0:X8}, past function end 0x{1:X8}", maxT, fEnd) : ""));
      if (maxT >= fEnd) {
        uint e = maxT; while (e < Hi) { uint x = W(e); if (x == 0x4E800020 || (Op(x) == 18 && (x & 1) == 0)) break; e += 4; }
        fnFix.AppendLine(string.Format("    {{ address = 0x{0:X8}, size = 0x{1:X} }},  # cases of switch 0x{2:X8} run to 0x{3:X8}", fStart, e + 4 - fStart, a, maxT));
      }

      sb.AppendLine("[[switch]]");
      sb.AppendLine(string.Format("base = 0x{0:X}  # {1} table at 0x{2:X8} in sub_{3:X8}, {4} entries ({5})", a, kind, table, fStart, labels.Count, how));
      sb.AppendLine("r = " + idxReg);
      sb.AppendLine("default = 0x0");
      sb.AppendLine("labels = [");
      foreach (var l in labels) sb.AppendLine(string.Format("    0x{0:X},", l));
      sb.AppendLine("]");
      sb.AppendLine();
    }
    return found + "\n" + report.ToString() + "\u0001" + fnFix.ToString() + "\u0001" + sb.ToString();
  }
}
'@

$result = [ExtraSwitches2]::Run((Resolve-Path $Image), "$root\config\pes13_switch_tables.toml", "$root\generated\ppc_func_mapping.cpp")
$head, $fnFix, $toml = $result -split [char]1
$count, $report = $head -split "`n", 2
Set-Content -Path "$root\config\pes13_switch_tables_extra.toml" -Value $toml -Encoding ascii
Write-Host "extra jump tables: $count -> config\pes13_switch_tables_extra.toml"
Write-Host $report
if ($fnFix.Trim()) { Write-Host "functions needing a larger explicit size:"; Write-Host $fnFix }
