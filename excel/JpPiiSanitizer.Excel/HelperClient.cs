// ヘルパー exe との接続（docs/excel-helper.md の「トランスポート」「プロトコル」）。
//
// 起動経路は 2 段:
//   ① 自分で子として起動する（--pipe jp-pii-excel-<ExcelのPID> --parent-pid <ExcelのPID>）。
//      Job Object（KILL_ON_JOB_CLOSE）に入れるので、Excel が落ちてもヘルパーは残らない。
//   ② 起動できなければ（Office の子プロセス作成を止める EDR/ASR 等）、利用者が手で起動した
//      ヘルパー（既定のパイプ名 jp-pii-sanitizer-<ユーザーSID>）に接続する。
// プロトコルは誰が起動したかを仮定しないので、どちらでも同じコードで話せる。
//
// フレーム = 4 バイト LE 長さ ＋ UTF-8 JSON。要求は {id,cmd,...}、応答は {id,type,ok,...}。
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace JpPii.Excel
{
    public sealed class HelperException : Exception
    {
        public HelperException(string message) : base(message) { }
        public HelperException(string message, Exception inner) : base(message, inner) { }
    }

    public sealed class HelperClient : IDisposable
    {
        public const int ProtocolVersion = 1;
        private const int MaxFrameBytes = 256 * 1024 * 1024;

        // ---- 共有インスタンス（Excel 1 プロセスにヘルパー 1 つ） ----
        private static readonly object SharedLock = new object();
        private static HelperClient _shared;

        public static HelperClient Shared
        {
            get
            {
                lock (SharedLock) return _shared ?? (_shared = new HelperClient());
            }
        }

        public static void DisposeShared()
        {
            HelperClient c;
            lock (SharedLock) { c = _shared; _shared = null; }
            c?.Dispose();
        }

        // ---- 状態 ----
        private readonly object _io = new object();  // 1 度に 1 要求（ヘルパーは直列）
        private Process _proc;
        private IntPtr _job = IntPtr.Zero;
        private NamedPipeClientStream _pipe;
        private int _nextId;
        private readonly StringBuilder _stderr = new StringBuilder();
        private readonly TaskCompletionSource<string> _listening = new TaskCompletionSource<string>();

        public bool Connected => _pipe != null && _pipe.IsConnected;
        /// <summary>モデル読込済みか（init 成功後 true）。</summary>
        public bool Ready { get; private set; }
        /// <summary>接続の仕方（UI の状態表示用）。</summary>
        public string Mode { get; private set; } = "未接続";

        // ---- 接続 ----

        /// <summary>接続していなければ起動／接続する。失敗は HelperException（利用者向けの文言）。</summary>
        public void EnsureConnected()
        {
            lock (_io)
            {
                if (Connected) return;
                CloseQuietly();

                string spawnError;
                if (TrySpawnAndConnect(out spawnError)) return;

                // ② 手動起動されたヘルパーに接続する
                var sid = WindowsIdentity.GetCurrent().User?.Value ?? "";
                var fallback = "jp-pii-sanitizer-" + sid;
                if (TryConnectPipe(fallback, 2000, expectedPid: 0))
                {
                    Mode = "既存のヘルパーに接続（" + fallback + "）";
                    return;
                }
                throw new HelperException(
                    "ヘルパー（jp-pii-helper.exe）を起動できず、起動済みのものにも接続できませんでした。\n\n" +
                    "起動時のエラー: " + spawnError + "\n\n" +
                    "対処: " + Paths.HelperExe + " を手動で起動してから、もう一度お試しください。");
            }
        }

        private bool TrySpawnAndConnect(out string error)
        {
            error = null;
            var exe = Paths.HelperExe;
            if (!File.Exists(exe))
            {
                error = "見つかりません: " + exe;
                return false;
            }
            var excelPid = Process.GetCurrentProcess().Id;
            var pipeName = "jp-pii-excel-" + excelPid;
            var args = "--pipe " + pipeName + " --parent-pid " + excelPid;
            var data = Paths.DataDir;
            if (data != null) args += " --data \"" + data + "\"";

            var psi = new ProcessStartInfo(exe, args)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8,
                WorkingDirectory = Path.GetDirectoryName(exe) ?? ".",
            };
            try
            {
                _proc = Process.Start(psi);
            }
            catch (Win32Exception e)
            {
                error = e.Message;
                return false;
            }
            if (_proc == null)
            {
                error = "Process.Start が null を返しました";
                return false;
            }

            // Excel と運命を共にさせる。失敗しても致命ではない（--parent-pid が保険）。
            _job = CreateKillOnCloseJob();
            if (_job != IntPtr.Zero) AssignProcessToJobObject(_job, _proc.Handle);

            // stdout の最初の 1 行 {"event":"listening",...} を待つ。以降の出力は捨て、
            // stderr は診断用に貯める。同期読みと BeginOutputReadLine は混ぜられないので、
            // 最初からイベントで受ける。
            _proc.OutputDataReceived += (s, e) =>
            {
                if (e.Data != null && e.Data.Contains("\"listening\"")) _listening.TrySetResult(e.Data);
            };
            _proc.ErrorDataReceived += (s, e) =>
            {
                if (e.Data == null) return;
                lock (_stderr)
                {
                    _stderr.AppendLine(e.Data);
                    if (_stderr.Length > 8000) _stderr.Remove(0, _stderr.Length - 8000);
                }
            };
            _proc.BeginOutputReadLine();
            _proc.BeginErrorReadLine();

            var done = Task.WhenAny(_listening.Task, Task.Delay(TimeSpan.FromSeconds(20))).Result;
            if (done != _listening.Task)
            {
                error = _proc.HasExited
                    ? "ヘルパーが終了しました（exit " + _proc.ExitCode + "）: " + StderrTail()
                    : "ヘルパーが待ち受けを開始しません";
                KillProcess();
                return false;
            }

            if (!TryConnectPipe(pipeName, 5000, expectedPid: _proc.Id))
            {
                error = "ヘルパーのパイプに接続できません: " + StderrTail();
                KillProcess();
                return false;
            }
            Mode = "子プロセス（PID " + _proc.Id + "）";
            return true;
        }

        /// <summary>
        /// パイプに接続する。expectedPid が非 0 なら、サーバのプロセスが自分の起動した
        /// ものであることを確かめる（同名のパイプを別のプロセスに先取りされていないか）。
        /// </summary>
        private bool TryConnectPipe(string name, int timeoutMs, int expectedPid)
        {
            var pipe = new NamedPipeClientStream(".", name, PipeDirection.InOut, PipeOptions.None);
            try
            {
                pipe.Connect(timeoutMs);
            }
            catch (Exception)
            {
                pipe.Dispose();
                return false;
            }
            if (expectedPid != 0)
            {
                uint serverPid;
                if (!GetNamedPipeServerProcessId(pipe.SafePipeHandle.DangerousGetHandle(), out serverPid) ||
                    serverPid != (uint)expectedPid)
                {
                    pipe.Dispose();
                    return false;
                }
            }
            _pipe = pipe;
            return true;
        }

        // ---- 要求 ----

        /// <summary>1 要求を送って応答を待つ。ok:false でも応答は返す（UI が error を見せる）。</summary>
        public JObject Call(string cmd, JObject fields = null)
        {
            lock (_io)
            {
                EnsureConnected();
                var req = fields != null ? (JObject)fields.DeepClone() : new JObject();
                req["id"] = Interlocked.Increment(ref _nextId);
                req["cmd"] = cmd;
                try
                {
                    WriteFrame(req.ToString(Formatting.None));
                    var resp = JObject.Parse(ReadFrame());
                    if (cmd == "init" && resp.Value<bool?>("ok") == true) Ready = true;
                    return resp;
                }
                catch (HelperException) { CloseQuietly(); throw; }
                catch (Exception e)
                {
                    CloseQuietly();
                    throw new HelperException("ヘルパーとの通信が切れました（" + cmd + "）: " + e.Message +
                                              (StderrTail().Length > 0 ? "\n" + StderrTail() : ""), e);
                }
            }
        }

        private void WriteFrame(string json)
        {
            var body = Encoding.UTF8.GetBytes(json);
            if (body.Length > MaxFrameBytes) throw new HelperException("要求が大きすぎます（" + body.Length + " バイト）");
            var hdr = new byte[4];
            hdr[0] = (byte)(body.Length & 0xFF);
            hdr[1] = (byte)((body.Length >> 8) & 0xFF);
            hdr[2] = (byte)((body.Length >> 16) & 0xFF);
            hdr[3] = (byte)((body.Length >> 24) & 0xFF);
            _pipe.Write(hdr, 0, 4);
            _pipe.Write(body, 0, body.Length);
            _pipe.Flush();
        }

        private string ReadFrame()
        {
            var hdr = ReadExact(4);
            var n = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | (hdr[3] << 24);
            if (n < 0 || n > MaxFrameBytes) throw new HelperException("応答のフレーム長が不正です: " + n);
            return Encoding.UTF8.GetString(ReadExact(n));
        }

        private byte[] ReadExact(int n)
        {
            var buf = new byte[n];
            var got = 0;
            while (got < n)
            {
                var k = _pipe.Read(buf, got, n - got);
                if (k <= 0)
                    throw new HelperException(_proc != null && _proc.HasExited
                        ? "ヘルパーが終了しました（exit " + _proc.ExitCode + "）"
                        : "ヘルパーが応答せずに切断しました");
                got += k;
            }
            return buf;
        }

        // ---- 便利メソッド（プロトコルの各コマンド） ----

        public JObject Hello() => Call("hello");
        public JObject Init() => Call("init");
        public JObject Load(JArray cells) => Call("load", new JObject { ["cells"] = cells });
        // columns は列指定（[{c,r1,r2,kind,skip}]。docs/excel-helper.md）。null なら送らない。
        public JObject Preview(JArray confirmed, JArray columns = null)
        {
            var f = new JObject { ["confirmed"] = confirmed };
            if (columns != null) f["columns"] = columns;
            return Call("preview", f);
        }
        public JObject Sanitize(JArray confirmed, bool reversible, JArray columns = null)
        {
            var f = new JObject { ["confirmed"] = confirmed, ["reversible"] = reversible };
            if (columns != null) f["columns"] = columns;
            return Call("sanitize", f);
        }
        public JObject SaveMapping(string path, string passphrase)
        {
            var f = new JObject { ["path"] = path };
            if (!string.IsNullOrEmpty(passphrase)) f["passphrase"] = passphrase;
            return Call("saveMapping", f);
        }
        public JObject ReverseText(string text, string mappingPath, string passphrase)
        {
            var f = new JObject { ["text"] = text, ["mappingPath"] = mappingPath };
            if (!string.IsNullOrEmpty(passphrase)) f["passphrase"] = passphrase;
            return Call("reverse", f);
        }
        /// <summary>セル配列を実名に戻す。応答の cells は変わったセルだけ。</summary>
        public JObject ReverseCells(JArray cells, string mappingPath, string passphrase)
        {
            var f = new JObject { ["cells"] = cells, ["mappingPath"] = mappingPath };
            if (!string.IsNullOrEmpty(passphrase)) f["passphrase"] = passphrase;
            return Call("reverse", f);
        }
        public JObject Reset() => Call("reset");

        // ---- 後始末 ----

        public void Dispose()
        {
            lock (_io)
            {
                try
                {
                    if (Connected)
                    {
                        WriteFrame(new JObject { ["id"] = 0, ["cmd"] = "shutdown" }.ToString(Formatting.None));
                        ReadFrame();
                    }
                }
                catch (Exception) { /* 終了中のエラーは無視 */ }
                CloseQuietly();
                if (_proc != null)
                {
                    if (!_proc.WaitForExit(3000)) KillProcess();
                    _proc.Dispose();
                    _proc = null;
                }
                if (_job != IntPtr.Zero)
                {
                    CloseHandle(_job);  // KILL_ON_JOB_CLOSE: ここで子も終わる
                    _job = IntPtr.Zero;
                }
            }
        }

        private void CloseQuietly()
        {
            Ready = false;
            Mode = "未接続";
            try { _pipe?.Dispose(); } catch (Exception) { }
            _pipe = null;
        }

        private void KillProcess()
        {
            try { if (_proc != null && !_proc.HasExited) _proc.Kill(); } catch (Exception) { }
        }

        private string StderrTail()
        {
            lock (_stderr) return _stderr.ToString().Trim();
        }

        // ---- Win32 ----

        private static IntPtr CreateKillOnCloseJob()
        {
            var job = CreateJobObjectW(IntPtr.Zero, null);
            if (job == IntPtr.Zero) return IntPtr.Zero;
            var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, ref info,
                                         Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION))))
            {
                CloseHandle(job);
                return IntPtr.Zero;
            }
            return job;
        }

        private const int JobObjectExtendedLimitInformation = 9;
        private const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000;

        [StructLayout(LayoutKind.Sequential)]
        private struct JOBOBJECT_BASIC_LIMIT_INFORMATION
        {
            public long PerProcessUserTimeLimit;
            public long PerJobUserTimeLimit;
            public uint LimitFlags;
            public UIntPtr MinimumWorkingSetSize;
            public UIntPtr MaximumWorkingSetSize;
            public uint ActiveProcessLimit;
            public UIntPtr Affinity;
            public uint PriorityClass;
            public uint SchedulingClass;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct IO_COUNTERS
        {
            public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount;
            public ulong ReadTransferCount, WriteTransferCount, OtherTransferCount;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
        {
            public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
            public IO_COUNTERS IoInfo;
            public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CreateJobObjectW(IntPtr attributes, string name);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetInformationJobObject(IntPtr job, int infoClass,
            ref JOBOBJECT_EXTENDED_LIMIT_INFORMATION info, int size);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(IntPtr handle);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetNamedPipeServerProcessId(IntPtr pipe, out uint serverPid);
    }
}
