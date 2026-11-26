// Process-tree measurement primitives for bench/run_compare.ps1 (compiled with Add-Type; C# 5 compatible so it also
// builds under Windows PowerShell 5.1). Follows the whole tree of a launched executable (Toolhelp parent-pid walk with a
// creation-time veto against pid reuse), keeps a handle to every process it has seen so that CPU time and cycles of
// processes that already exited still count, and samples memory, handles and threads of the live ones.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Cmp
{
    public sealed class Snapshot
    {
        public double AtMs;               // wall clock (ms since the Unix epoch) when taken
        public double CpuMs;              // kernel + user CPU time of every process ever seen in the tree
        public double CycleMillions;      // QueryProcessCycleTime of every process ever seen, in millions
        public int Alive;                 // live processes
        public int Handles;               // handle count over live processes
        public int Threads;               // thread count over live processes
        public double HostCommitMb;       // root process: private bytes (PrivateUsage)
        public double HostPrivateWsMb;    // root process: private working set
        public double TreePrivateWsMb;    // all live processes: private working set (Task Manager "Memory")
        public double TreeCommitMb;       // all live processes: private bytes
        public double TreeWsMb;           // all live processes: working set (counts shared pages in every process)
        public List<int> Pids = new List<int>();
    }

    public sealed class Tree : IDisposable
    {
        sealed class Tracked
        {
            public int Pid;
            public int Ppid;
            public IntPtr Handle;
            public ulong Created;
            public bool Alive = true;
            public int Threads;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct FILETIME { public uint Low; public uint High; }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct PROCESSENTRY32
        {
            public uint dwSize; public uint cntUsage; public uint th32ProcessID; public UIntPtr th32DefaultHeapID;
            public uint th32ModuleID; public uint cntThreads; public uint th32ParentProcessID; public int pcPriClassBase;
            public uint dwFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string szExeFile;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct PMC2
        {
            public uint cb; public uint PageFaultCount;
            public UIntPtr PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
                QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage, PrivateWorkingSetSize;
            public ulong SharedCommitUsage;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct RECT { public int Left, Top, Right, Bottom; }

        delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lparam);

        [DllImport("kernel32.dll")] static extern void GetSystemTimePreciseAsFileTime(out FILETIME t);
        [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint pid);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool Process32FirstW(IntPtr snap, ref PROCESSENTRY32 e);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool Process32NextW(IntPtr snap, ref PROCESSENTRY32 e);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
        [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
        [DllImport("kernel32.dll")] static extern bool GetProcessTimes(IntPtr h, out FILETIME c, out FILETIME e, out FILETIME k, out FILETIME u);
        [DllImport("kernel32.dll")] static extern bool QueryProcessCycleTime(IntPtr h, out ulong cycles);
        [DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr h, out uint code);
        [DllImport("kernel32.dll")] static extern bool GetProcessHandleCount(IntPtr h, out uint count);
        [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
        [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr h, uint code);
        [DllImport("kernel32.dll", EntryPoint = "K32GetProcessMemoryInfo")] static extern bool GetProcessMemoryInfo(IntPtr h, ref PMC2 c, uint cb);
        [DllImport("kernel32.dll")] static extern bool GetSystemTimes(out FILETIME idle, out FILETIME kernel, out FILETIME user);
        [DllImport("ntdll.dll")] static extern int NtQuerySystemInformation(int infoClass, IntPtr buffer, int length, out int returned);
        [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lparam);
        [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
        [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
        [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd, out RECT r);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr hwnd, System.Text.StringBuilder s, int max);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr hwnd, System.Text.StringBuilder s, int max);
        [DllImport("user32.dll", EntryPoint = "GetWindowLongW")] static extern int GetWindowLong(IntPtr hwnd, int index);
        [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
        [DllImport("user32.dll")] static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);

        const uint PROCESS_TERMINATE = 0x0001;
        const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
        const uint SYNCHRONIZE = 0x00100000;
        const uint TH32CS_SNAPPROCESS = 0x2;
        const uint STILL_ACTIVE = 259;
        const uint WM_CLOSE = 0x0010;
        const double Mb = 1024.0 * 1024.0;

        readonly Dictionary<int, Tracked> m_tracked = new Dictionary<int, Tracked>();
        readonly Process m_process;
        public readonly int RootPid;
        public readonly double LaunchMs;

        static ulong ToU64(FILETIME t) { return ((ulong)t.High << 32) | t.Low; }

        /// <summary>Wall clock in ms since the Unix epoch, sub-millisecond resolution.</summary>
        public static double NowMs()
        {
            FILETIME t;
            GetSystemTimePreciseAsFileTime(out t);
            return (ToU64(t) - 116444736000000000UL) / 10000.0;
        }

        Tree(Process process, double launchMs)
        {
            m_process = process;
            RootPid = process.Id;
            LaunchMs = launchMs;
            Track(RootPid, 0);
        }

        public static Tree Start(string exe, string arguments, string workDir, IDictionary<string, string> env)
        {
            ProcessStartInfo psi = new ProcessStartInfo(exe, arguments);
            psi.UseShellExecute = false;
            psi.WorkingDirectory = workDir;
            foreach (KeyValuePair<string, string> kv in env) psi.EnvironmentVariables[kv.Key] = kv.Value;
            double launch = NowMs();
            Process p = Process.Start(psi);
            return new Tree(p, launch);
        }

        Tracked Track(int pid, int ppid)
        {
            IntPtr h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, false, (uint)pid);
            if (h == IntPtr.Zero) return null;
            FILETIME c, e, k, u;
            if (!GetProcessTimes(h, out c, out e, out k, out u)) { CloseHandle(h); return null; }
            Tracked t = new Tracked();
            t.Pid = pid; t.Ppid = ppid; t.Handle = h; t.Created = ToU64(c);
            m_tracked[pid] = t;
            return t;
        }

        static bool IsAlive(IntPtr h)
        {
            uint code;
            return GetExitCodeProcess(h, out code) && code == STILL_ACTIVE;
        }

        /// <summary>Walks the process list once: adopts new children of tracked live processes, updates liveness and thread counts.</summary>
        public void Refresh()
        {
            IntPtr snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap == new IntPtr(-1)) return;
            List<PROCESSENTRY32> all = new List<PROCESSENTRY32>(512);
            try
            {
                PROCESSENTRY32 e = new PROCESSENTRY32();
                e.dwSize = (uint)Marshal.SizeOf(typeof(PROCESSENTRY32));
                bool ok = Process32FirstW(snap, ref e);
                while (ok) { all.Add(e); ok = Process32NextW(snap, ref e); }
            }
            finally { CloseHandle(snap); }

            foreach (Tracked t in m_tracked.Values) if (t.Alive && !IsAlive(t.Handle)) t.Alive = false;
            bool grew = true;
            while (grew)
            {
                grew = false;
                foreach (PROCESSENTRY32 e in all)
                {
                    int pid = (int)e.th32ProcessID;
                    if (m_tracked.ContainsKey(pid)) continue;
                    Tracked parent;
                    if (!m_tracked.TryGetValue((int)e.th32ParentProcessID, out parent) || !parent.Alive) continue;
                    Tracked child = Track(pid, parent.Pid);
                    if (child == null) continue;
                    if (child.Created < parent.Created) // pid reuse: this process predates the claimed parent
                    {
                        CloseHandle(child.Handle);
                        m_tracked.Remove(pid);
                        continue;
                    }
                    grew = true;
                }
            }
            foreach (PROCESSENTRY32 e in all)
            {
                Tracked t;
                if (m_tracked.TryGetValue((int)e.th32ProcessID, out t)) t.Threads = (int)e.cntThreads;
            }
        }

        public Snapshot Sample()
        {
            Refresh();
            Snapshot s = new Snapshot();
            s.AtMs = NowMs();
            foreach (Tracked t in m_tracked.Values)
            {
                FILETIME c, e, k, u;
                if (GetProcessTimes(t.Handle, out c, out e, out k, out u)) s.CpuMs += (ToU64(k) + ToU64(u)) / 10000.0;
                ulong cycles;
                if (QueryProcessCycleTime(t.Handle, out cycles)) s.CycleMillions += cycles / 1e6;
                if (!t.Alive) continue;
                s.Alive++;
                s.Pids.Add(t.Pid);
                s.Threads += t.Threads;
                uint handles;
                if (GetProcessHandleCount(t.Handle, out handles)) s.Handles += (int)handles;
                PMC2 m = new PMC2();
                m.cb = (uint)Marshal.SizeOf(typeof(PMC2));
                if (GetProcessMemoryInfo(t.Handle, ref m, m.cb))
                {
                    double commit = (double)m.PrivateUsage.ToUInt64() / Mb;
                    double pws = (double)m.PrivateWorkingSetSize.ToUInt64() / Mb;
                    s.TreeCommitMb += commit;
                    s.TreePrivateWsMb += pws;
                    s.TreeWsMb += (double)m.WorkingSetSize.ToUInt64() / Mb;
                    if (t.Pid == RootPid) { s.HostCommitMb = commit; s.HostPrivateWsMb = pws; }
                }
            }
            return s;
        }

        /// <summary>An application window: visible, titled (WS_CAPTION) and at least 100x100 px. Skips helper windows
        /// such as Tao's 16x16 "Thread Event Target" that are visible but are not what a user sees.</summary>
        static bool IsAppWindow(IntPtr hwnd)
        {
            RECT r;
            if (!IsWindowVisible(hwnd) || !GetWindowRect(hwnd, out r)) return false;
            return (r.Right - r.Left) >= 100 && (r.Bottom - r.Top) >= 100 && (GetWindowLong(hwnd, -16) & 0x00C00000) == 0x00C00000;
        }

        List<IntPtr> VisibleWindows()
        {
            HashSet<uint> pids = new HashSet<uint>();
            foreach (Tracked t in m_tracked.Values) if (t.Alive) pids.Add((uint)t.Pid);
            List<IntPtr> found = new List<IntPtr>();
            EnumWindows(delegate (IntPtr hwnd, IntPtr lp)
            {
                uint pid;
                GetWindowThreadProcessId(hwnd, out pid);
                if (pids.Contains(pid) && IsAppWindow(hwnd)) found.Add(hwnd);
                return true;
            }, IntPtr.Zero);
            return found;
        }

        /// <summary>"hwnd class 'title' style" of the visible windows (diagnostics).</summary>
        public string DescribeWindows()
        {
            List<string> parts = new List<string>();
            foreach (IntPtr h in VisibleWindows())
            {
                System.Text.StringBuilder cls = new System.Text.StringBuilder(128);
                System.Text.StringBuilder title = new System.Text.StringBuilder(128);
                GetClassNameW(h, cls, 128);
                GetWindowTextW(h, title, 128);
                RECT r;
                GetWindowRect(h, out r);
                parts.Add(h.ToInt64().ToString("x") + " " + cls + " '" + title + "' style=" + GetWindowLong(h, -16).ToString("x") + " " + (r.Right - r.Left) + "x" + (r.Bottom - r.Top));
            }
            return string.Join("; ", parts.ToArray());
        }

        public int VisibleWindowCount() { return VisibleWindows().Count; }

        /// <summary>Wall clock ms of the first poll that sees an application window (see IsAppWindow) of the root process; -1 on timeout.
        /// Polls with EnumWindows only (no process snapshots, which would perturb the app being started).</summary>
        public double WaitFirstWindow(int timeoutMs)
        {
            double deadline = NowMs() + timeoutMs;
            while (NowMs() < deadline)
            {
                bool found = false;
                EnumWindows(delegate (IntPtr hwnd, IntPtr lp)
                {
                    uint pid;
                    GetWindowThreadProcessId(hwnd, out pid);
                    if (pid == (uint)RootPid && IsAppWindow(hwnd)) { found = true; return false; }
                    return true;
                }, IntPtr.Zero);
                if (found) return NowMs();
                System.Threading.Thread.Sleep(1);
            }
            return -1;
        }

        /// <summary>Makes the tree's application windows topmost (without activating them), so that no other window - other
        /// programs on a shared machine - can cover them. A covered window is "occluded": Chromium stops painting it, pauses
        /// requestAnimationFrame and drops the renderer to idle priority, which stalled whole runs for reasons that have
        /// nothing to do with the framework being measured.</summary>
        public void RaiseWindows()
        {
            const uint SWP_NOSIZE = 0x1, SWP_NOMOVE = 0x2, SWP_NOACTIVATE = 0x10;
            foreach (IntPtr h in VisibleWindows()) SetWindowPos(h, new IntPtr(-1) /* HWND_TOPMOST */, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
        }

        public void CloseWindows()
        {
            foreach (IntPtr h in VisibleWindows()) PostMessageW(h, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
        }

        /// <summary>Wall clock ms at which every process known to the tree (call Refresh() just before closing) has exited;
        /// -1 on timeout. Waits on the process handles and does not enumerate processes in the meantime: polling the
        /// process list during teardown was observed to keep the Boson/Tauri/Wails WebView2 browser process alive for
        /// tens of seconds, which would have turned the measurement into the thing being measured.</summary>
        public double WaitAllExited(int timeoutMs)
        {
            double deadline = NowMs() + timeoutMs;
            List<Tracked> known = new List<Tracked>(m_tracked.Values);
            foreach (Tracked t in known)
            {
                double left = deadline - NowMs();
                if (left < 0) left = 0;
                if (WaitForSingleObject(t.Handle, (uint)left) != 0) return -1;
                t.Alive = false;
            }
            return NowMs();
        }

        /// <summary>Wall clock ms at which the launched (root) process has exited; -1 on timeout.</summary>
        public double WaitRootExited(int timeoutMs)
        {
            Tracked root;
            if (!m_tracked.TryGetValue(RootPid, out root)) return -1;
            return WaitForSingleObject(root.Handle, (uint)timeoutMs) == 0 ? NowMs() : -1;
        }

        /// <summary>"pid:image" of the live processes of the tree (diagnostics for a shutdown that did not finish).</summary>
        public string DescribeAlive()
        {
            List<string> parts = new List<string>();
            foreach (Tracked t in m_tracked.Values)
            {
                if (!t.Alive) continue;
                string name = "?";
                try { name = Process.GetProcessById(t.Pid).ProcessName; } catch (Exception) { }
                parts.Add(t.Pid + ":" + name);
            }
            return string.Join(", ", parts.ToArray());
        }

        public void KillAll()
        {
            Refresh();
            foreach (Tracked t in m_tracked.Values) if (t.Alive) TerminateProcess(t.Handle, 1);
        }

        /// <summary>Cumulative context switches (sum over the live threads) of every live process of the tree, from
        /// NtQuerySystemInformation(SystemProcessInformation). Threads that exit between two calls take their count with them.</summary>
        public Dictionary<int, ulong> ContextSwitches()
        {
            const int SystemProcessInformation = 5;
            const int ThreadsOffset = 0x100;   // sizeof(SYSTEM_PROCESS_INFORMATION) on x64
            const int ThreadSize = 0x50;       // sizeof(SYSTEM_THREAD_INFORMATION) on x64
            const int ContextSwitchesOffset = 64;
            Dictionary<int, ulong> result = new Dictionary<int, ulong>();
            int size = 1 << 20;
            IntPtr buffer = IntPtr.Zero;
            try
            {
                while (true)
                {
                    buffer = Marshal.AllocHGlobal(size);
                    int needed;
                    int status = NtQuerySystemInformation(SystemProcessInformation, buffer, size, out needed);
                    if (status == unchecked((int)0xC0000004)) // STATUS_INFO_LENGTH_MISMATCH
                    {
                        Marshal.FreeHGlobal(buffer);
                        buffer = IntPtr.Zero;
                        size = Math.Max(needed, size) + (1 << 18);
                        continue;
                    }
                    if (status != 0) return result;
                    break;
                }
                int offset = 0;
                while (true)
                {
                    IntPtr entry = new IntPtr(buffer.ToInt64() + offset);
                    int next = Marshal.ReadInt32(entry, 0);
                    int threads = Marshal.ReadInt32(entry, 4);
                    int pid = (int)Marshal.ReadInt64(entry, 80);
                    Tracked t;
                    if (m_tracked.TryGetValue(pid, out t) && t.Alive)
                    {
                        ulong sum = 0;
                        for (int i = 0; i < threads; i++) sum += (uint)Marshal.ReadInt32(entry, ThreadsOffset + i * ThreadSize + ContextSwitchesOffset);
                        result[pid] = sum;
                    }
                    if (next == 0) break;
                    offset += next;
                }
            }
            finally { if (buffer != IntPtr.Zero) Marshal.FreeHGlobal(buffer); }
            return result;
        }

        /// <summary>{idle, kernel, user} system-wide CPU time in 100 ns units (kernel includes idle).</summary>
        public static ulong[] SystemTimes()
        {
            FILETIME i, k, u;
            GetSystemTimes(out i, out k, out u);
            return new ulong[] { ToU64(i), ToU64(k), ToU64(u) };
        }

        public void Dispose()
        {
            foreach (Tracked t in m_tracked.Values) CloseHandle(t.Handle);
            m_tracked.Clear();
            if (m_process != null) m_process.Dispose();
        }
    }
}
