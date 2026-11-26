<#
.SYNOPSIS
  Host-process and process-tree footprint snapshot for a Boson (or any WebView2) app: threads, handles, loaded
  DLLs, heap count, private working set, commit, and idle context switches per second.

.DESCRIPTION
  Starts -Exe with a throw-away WebView2 user data folder, waits -Settle seconds, snapshots, optionally performs a
  -Scenario step ("burst" posts BosonBench.StartLoad to boson_bench_app / boson_kitchen_sink and waits for the load test; "minimized" minimizes the main window and waits 3 s), idles -Idle
  seconds, snapshots again and writes JSON to -Out. Everything is read with documented Win32 / NT APIs
  (GetProcessMemoryInfo, NtQuerySystemInformation per-thread context switches, Toolhelp heap lists): no ETW session,
  no admin rights, safe to run beside other work. host_* fields are the app process alone; tree_* sum the whole
  process tree (msedgewebview2.exe included). Used for docs/PERFORMANCE.md ("Memory footprint, threads and idle cost").

.EXAMPLE
  powershell -File bench\memsnap.ps1 -Exe build\rel\boson_kitchen_sink.exe -Name ks -Scenario idle -Idle 30 -Out out\ks.json
#>

param([string]$Exe,[string]$Name,[int]$Idle=30,[int]$Settle=4,[string]$Scenario="idle",[string]$Out,[string[]]$AppArgs=@())
$src = @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Diagnostics; using System.Linq;
public static class Snap {
  [StructLayout(LayoutKind.Sequential)] public struct PMC2 { public uint cb; public uint PageFaultCount; public UIntPtr PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage, PrivateWorkingSetSize; public ulong SharedCommitUsage; }
  [DllImport("psapi.dll")] static extern bool GetProcessMemoryInfo(IntPtr h, ref PMC2 p, uint cb);
  [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint a, bool i, int pid);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll")] static extern bool GetProcessHandleCount(IntPtr h, out uint c);
  [DllImport("kernel32.dll")] static extern IntPtr CreateToolhelp32Snapshot(uint f, uint pid);
  [DllImport("kernel32.dll")] static extern bool Heap32ListFirst(IntPtr s, ref HEAPLIST32 h);
  [DllImport("kernel32.dll")] static extern bool Heap32ListNext(IntPtr s, ref HEAPLIST32 h);
  [DllImport("kernel32.dll")] static extern bool QueryProcessCycleTime(IntPtr h, out ulong c);
  [DllImport("kernel32.dll")] static extern bool GetProcessTimes(IntPtr h, out long c, out long e, out long k, out long u);
  [StructLayout(LayoutKind.Sequential)] public struct HEAPLIST32 { public UIntPtr dwSize; public uint th32ProcessID; public UIntPtr th32HeapID; public uint dwFlags; }
  [DllImport("ntdll.dll")] static extern int NtQuerySystemInformation(int c, IntPtr b, int len, out int ret);
  public class Info { public int Pid; public string Name; public long PrivMB_KB, WsKB, PrivWsKB, CommitKB; public uint Handles; public int Threads, Modules, Heaps; public long CtxSw; public long CpuMs; public ulong Cycles; public List<string> ThreadStarts = new List<string>(); public Dictionary<int,int> ThreadCtx=new Dictionary<int,int>(); public Dictionary<int,long> ThreadStart=new Dictionary<int,long>(); public long ExeBase; }
  public static Info Of(int pid, bool deep) {
    var i = new Info{Pid=pid};
    IntPtr h = OpenProcess(0x1000|0x0010, false, pid); if (h==IntPtr.Zero) return null;
    try {
      var m = new PMC2(); m.cb=(uint)Marshal.SizeOf(typeof(PMC2));
      if (GetProcessMemoryInfo(h, ref m, m.cb)) { i.WsKB=(long)((ulong)m.WorkingSetSize/1024); i.PrivWsKB=(long)((ulong)m.PrivateWorkingSetSize/1024); i.CommitKB=(long)((ulong)m.PrivateUsage/1024);} 
      uint hc; GetProcessHandleCount(h, out hc); i.Handles=hc;
      long c,e,k,u; if (GetProcessTimes(h,out c,out e,out k,out u)) i.CpuMs=(k+u)/10000;
      ulong cy; if (QueryProcessCycleTime(h, out cy)) i.Cycles=cy;
    } finally { CloseHandle(h); }
    try { var p = Process.GetProcessById(pid); i.Name=p.ProcessName; i.Modules=deep? p.Modules.Count:0; } catch {}
    if (true) { var s = CreateToolhelp32Snapshot(0x1, (uint)pid); if (s!=IntPtr.Zero && s!=new IntPtr(-1)) { var hl=new HEAPLIST32(); hl.dwSize=(UIntPtr)Marshal.SizeOf(typeof(HEAPLIST32)); int n=0; if (Heap32ListFirst(s, ref hl)) { do { n++; } while (Heap32ListNext(s, ref hl)); } i.Heaps=n; CloseHandle(s);} }
    // threads via NtQuerySystemInformation(SystemProcessInformation=5)
    int len=1<<20, ret; IntPtr buf=Marshal.AllocHGlobal(len);
    try {
      while (NtQuerySystemInformation(57, buf, len, out ret)==unchecked((int)0xC0000004)) { Marshal.FreeHGlobal(buf); len=ret+65536; buf=Marshal.AllocHGlobal(len); }
      long off=0;
      while (true) {
        IntPtr p = IntPtr.Add(buf,(int)off);
        uint next = (uint)Marshal.ReadInt32(p,0); uint nthreads=(uint)Marshal.ReadInt32(p,4);
        long upid = Marshal.ReadInt64(p, 0x50);
        if (upid==pid) {
          i.Threads=(int)nthreads;
          // SYSTEM_THREAD_INFORMATION array at 0x100 on x64: size 0x50
          long baseOff = 0x100; int tsz=0x88; long ctx=0;
          for (int t=0;t<nthreads;t++) { IntPtr tp=IntPtr.Add(p,(int)(baseOff+t*tsz)); ctx += Marshal.ReadInt32(tp, 0x40); i.ThreadCtx[(int)Marshal.ReadInt64(tp,0x30)] = Marshal.ReadInt32(tp,0x40); i.ThreadStart[(int)Marshal.ReadInt64(tp,0x30)] = Marshal.ReadInt64(tp,0x60);  }
          i.CtxSw=ctx; break;
        }
        if (next==0) break; off+=next;
      }
    } finally { Marshal.FreeHGlobal(buf); }
    return i;
  }
  public static Dictionary<int,int> Parents() {
    var d=new Dictionary<int,int>();
    IntPtr s=CreateToolhelp32Snapshot(0x2,0); var pe=new PE(); pe.dwSize=(uint)Marshal.SizeOf(typeof(PE));
    if (Process32First(s, ref pe)) { do { d[(int)pe.th32ProcessID]=(int)pe.th32ParentProcessID; } while (Process32Next(s, ref pe)); }
    CloseHandle(s); return d;
  }
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct PE { public uint dwSize, cntUsage, th32ProcessID; public UIntPtr th32DefaultHeapID; public uint th32ModuleID, cntThreads, th32ParentProcessID; public int pcPriClassBase; public uint dwFlags; [MarshalAs(UnmanagedType.ByValTStr, SizeConst=260)] public string szExeFile; }
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern bool Process32First(IntPtr s, ref PE p);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern bool Process32Next(IntPtr s, ref PE p);
  public static List<int> Tree(int root) {
    var par=Parents(); var res=new List<int>{root}; bool grew=true;
    while (grew) { grew=false; foreach (var kv in par) if (!res.Contains(kv.Key) && res.Contains(kv.Value)) { res.Add(kv.Key); grew=true; } }
    return res;
  }
}
'@
Add-Type -TypeDefinition $src -Language CSharp
function Tree-Sum($root) {
  $ids=[Snap]::Tree($root); $infos=@(); foreach($id in $ids){ $x=[Snap]::Of($id,$false); if($x){$infos+=$x} }
  [pscustomobject]@{ Procs=$infos.Count; PrivWsMB=[math]::Round((($infos|Measure-Object PrivWsKB -Sum).Sum)/1024,1); CommitMB=[math]::Round((($infos|Measure-Object CommitKB -Sum).Sum)/1024,1); Handles=($infos|Measure-Object Handles -Sum).Sum; Threads=($infos|Measure-Object Threads -Sum).Sum; CtxSw=($infos|Measure-Object CtxSw -Sum).Sum; CpuMs=($infos|Measure-Object CpuMs -Sum).Sum }
}
$env:BOSON_BENCH_DIR = Split-Path $Out
$res=[ordered]@{}
$udf = Join-Path $env:TEMP ("bosonmem_" + [guid]::NewGuid().ToString("N").Substring(0,8))
$sw=[Diagnostics.Stopwatch]::StartNew()
$psi = New-Object Diagnostics.ProcessStartInfo $Exe; $psi.UseShellExecute=$false; foreach($a in $AppArgs){ $psi.ArgumentList.Add($a) }
$psi.EnvironmentVariables["WEBVIEW2_USER_DATA_FOLDER"]=$udf
$p=[Diagnostics.Process]::Start($psi); $rootPid=$p.Id
Start-Sleep -Seconds $Settle
$res.name=$Name; $res.scenario=$Scenario
$hostInfo=[Snap]::Of($rootPid,$true)
$res.startup_host=$hostInfo | Select-Object * -ExcludeProperty ThreadCtx,ThreadStart
$res.startup_tree=Tree-Sum $rootPid
if ($Scenario -eq "burst") {
  # boson_bench_app / boson_kitchen_sink protocol: BosonBench.StartLoad runs the page's IPC load test (static and
  # dynamic calls, 10 MB transfers); the app writes load_done into BOSON_BENCH_DIR when it finishes.
  Add-Type -Name WB -Namespace NB -MemberDefinition '[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l); [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern uint RegisterWindowMessage(string s);'
  Remove-Item (Join-Path $env:BOSON_BENCH_DIR "load_done") -ErrorAction SilentlyContinue
  $p.Refresh(); $msg=[NB.WB]::RegisterWindowMessage("BosonBench.StartLoad"); [NB.WB]::PostMessage($p.MainWindowHandle,$msg,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
  $dl=(Get-Date).AddSeconds(90); while(-not (Test-Path (Join-Path $env:BOSON_BENCH_DIR "load_done")) -and (Get-Date) -lt $dl){ Start-Sleep -Milliseconds 200 }
  $res.load_done=(Test-Path (Join-Path $env:BOSON_BENCH_DIR "load_done")); Start-Sleep -Seconds 3
}
if ($Scenario -eq "minimized") {
  Add-Type -Name W -Namespace N -MemberDefinition '[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int c);'
  $p.Refresh(); [N.W]::ShowWindow($p.MainWindowHandle, 6) | Out-Null; Start-Sleep -Seconds 3
}
$a=[Snap]::Of($rootPid,$false); $ta=Tree-Sum $rootPid; $t0=Get-Date
Start-Sleep -Seconds $Idle
$b=[Snap]::Of($rootPid,$true); $tb=Tree-Sum $rootPid; $secs=((Get-Date)-$t0).TotalSeconds
$res.idle_host=$b | Select-Object * -ExcludeProperty ThreadCtx,ThreadStart
$res.idle_tree=$tb
$res.host_ctxsw_per_s=[math]::Round(($b.CtxSw-$a.CtxSw)/$secs,1)
$res.tree_ctxsw_per_s=[math]::Round(($tb.CtxSw-$ta.CtxSw)/$secs,1)
$res.host_cpu_ms_per_s=[math]::Round(($b.CpuMs-$a.CpuMs)/$secs,2)
$res.tree_cpu_pct_core=[math]::Round(($tb.CpuMs-$ta.CpuMs)/10/$secs,2)
$res.host_cycles_per_s=[math]::Round(($b.Cycles-$a.Cycles)/$secs/1e6,2)
$hot=@(); foreach($k in $b.ThreadCtx.Keys){ $d=$b.ThreadCtx[$k]; if($a.ThreadCtx.ContainsKey($k)){$d-=$a.ThreadCtx[$k]}; if($d -gt 0){ $hot+=[pscustomobject]@{tid=$k;ctx=$d;start=("{0:x}" -f $b.ThreadStart[$k])} } }
$res.hot_threads=$hot | Sort-Object ctx -Descending | Select-Object -First 12
$mods=(Get-Process -Id $rootPid).Modules | Where-Object { $_.ModuleName -like "boson*" -or $_.ModuleName -like "*.exe" } | ForEach-Object { "{0} base={1:x} size={2:x}" -f $_.ModuleName,[int64]$_.BaseAddress,$_.ModuleMemorySize }
$res.exe_module=$mods
$p.Refresh(); Add-Type -Name U -Namespace N2 -MemberDefinition '[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);'
$pidx=0; $res.ui_tid=[N2.U]::GetWindowThreadProcessId($p.MainWindowHandle,[ref]$pidx)
$res.host_modules=(Get-Process -Id $rootPid).Modules | ForEach-Object { $_.ModuleName }
$res | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 $Out
# shutdown
$tree=[Snap]::Tree($rootPid)
try { $p.CloseMainWindow() | Out-Null; if(-not $p.WaitForExit(8000)){ $p.Kill() } } catch {}
Start-Sleep -Milliseconds 800
foreach($id in $tree){ try { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue } catch {} }
Remove-Item -Recurse -Force $udf -ErrorAction SilentlyContinue
