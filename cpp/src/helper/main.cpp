// jp-pii-helper: Excel アドイン用のヘルパー exe（docs/excel-helper.md）。
//
// helper::Session（セル単位の facade）を名前付きパイプ（Windows）または stdio で
// 公開する。アドイン（Excel-DNA / .NET）はこのプロセスを子として起動し、セルの値を
// 送って候補・プレビュー・サニタイズ結果を受け取る。
//
// なぜ Excel のプロセス内（DLL）ではなく別プロセスか:
//   - コアが落ちても Excel（と未保存のブック）を巻き添えにしない
//   - 約 1 GB のモデルを EXCEL.EXE に常駐させない（必要なときだけ起動し、終われば消える）
//   - Office 自身や他のアドインが別版の onnxruntime.dll を読んでいても衝突しない
//   - 対応表（最も機微な成果物）を、他社アドインやマクロが同居する Excel のメモリに乗せない
//
// トランスポート:
//   --pipe [NAME]   名前付きパイプ \\.\pipe\NAME（既定 jp-pii-sanitizer-<ユーザーSID>）。
//                   DACL は現在のユーザーのみ、リモート接続は拒否。1 度に 1 クライアント。
//   --stdio         標準入出力（Linux/macOS と、CI・デバッグ用）。
//   どちらも同じフレーム（4 バイト LE 長さ ＋ JSON）。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <sddl.h>      // SID 文字列・SDDL → セキュリティ記述子
#include <shellapi.h>  // CommandLineToArgvW（Windows は argv が cp932 なので UTF-16 から作り直す）
#else
#include <signal.h>
#include <unistd.h>
#endif

#include "data_root.hpp"
#include "helper/protocol.hpp"
#include "helper/selftest.hpp"
#include "helper/session.hpp"

namespace {

// ---------------------------------------------------------------- transports
struct StdioStream final : helper::proto::Stream {
  bool read_exact(void* buf, std::size_t n) override {
    auto* p = static_cast<unsigned char*>(buf);
    while (n > 0) {
      const std::size_t k = std::fread(p, 1, n, stdin);
      if (k == 0) return false;
      p += k;
      n -= k;
    }
    return true;
  }
  bool write_all(const void* buf, std::size_t n) override {
    return std::fwrite(buf, 1, n, stdout) == n && std::fflush(stdout) == 0;
  }
};

#ifdef _WIN32
struct PipeStream final : helper::proto::Stream {
  HANDLE h;
  explicit PipeStream(HANDLE handle) : h(handle) {}
  bool read_exact(void* buf, std::size_t n) override {
    auto* p = static_cast<unsigned char*>(buf);
    while (n > 0) {
      DWORD got = 0;
      const DWORD want = n > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<DWORD>(n);
      if (!ReadFile(h, p, want, &got, nullptr) || got == 0) return false;  // 切断は ERROR_BROKEN_PIPE
      p += got;
      n -= got;
    }
    return true;
  }
  bool write_all(const void* buf, std::size_t n) override {
    const auto* p = static_cast<const unsigned char*>(buf);
    while (n > 0) {
      DWORD put = 0;
      const DWORD want = n > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<DWORD>(n);
      if (!WriteFile(h, p, want, &put, nullptr) || put == 0) return false;
      p += put;
      n -= put;
    }
    return true;
  }
};

std::string wide_to_utf8(const wchar_t* w, int wlen = -1) {
  if (!w) return {};
  const int len = WideCharToMultiByte(CP_UTF8, 0, w, wlen, nullptr, 0, nullptr, nullptr);
  if (len <= 0) return {};
  std::string out(static_cast<std::size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, wlen, out.data(), len, nullptr, nullptr);
  if (wlen < 0 && !out.empty() && out.back() == '\0') out.pop_back();  // 終端込みで数えた分
  return out;
}

std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return {};
  const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (len <= 0) return {};
  std::wstring out(static_cast<std::size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
  return out;
}

/// 現在のユーザーの SID 文字列（S-1-5-21-…）。パイプ名の既定値と DACL に使う。
std::wstring current_user_sid() {
  HANDLE tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return {};
  DWORD need = 0;
  GetTokenInformation(tok, TokenUser, nullptr, 0, &need);
  std::vector<unsigned char> buf(need);
  std::wstring out;
  if (need && GetTokenInformation(tok, TokenUser, buf.data(), need, &need)) {
    LPWSTR s = nullptr;
    if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s)) {
      out = s;
      LocalFree(s);
    }
  }
  CloseHandle(tok);
  return out;
}

/// 名前付きパイプのサーバループ。接続ごとに Server を作り、切れたら次を待つ。
/// shutdown を受けたら戻る。
int run_pipe_server(helper::Session& session, std::wstring name) {
  const std::wstring sid = current_user_sid();
  if (sid.empty()) {
    std::fprintf(stderr, "error: 現在のユーザーの SID を取得できません\n");
    return 1;
  }
  if (name.empty()) name = L"jp-pii-sanitizer-" + sid;
  const std::wstring full = L"\\\\.\\pipe\\" + name;

  // **本人だけが開けるパイプにする。** D:P(A;;GA;;;<SID>) = 継承を切り、現在のユーザーに
  // 全権、それ以外には何も与えない。PIPE_REJECT_REMOTE_CLIENTS でネットワーク越しも拒む。
  // （接続してきた相手が本当にアドインかは検証できない——同じユーザーのプロセスなら
  //   誰でも繋げる。これは「同じ PC でマルウェアが動いている」脅威と同じで、対象外。）
  PSECURITY_DESCRIPTOR sd = nullptr;
  const std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
    std::fprintf(stderr, "error: パイプのセキュリティ記述子を作れません (%lu)\n", GetLastError());
    return 1;
  }
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.lpSecurityDescriptor = sd;
  sa.bInheritHandle = FALSE;

  auto create = [&](bool first) -> HANDLE {
    // 初回は FILE_FLAG_FIRST_PIPE_INSTANCE: 同名のパイプが既にあれば（別のヘルパー、
    // または名前を先取りした何か）2 つ目にならずに失敗する。
    return CreateNamedPipeW(full.c_str(),
                            PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            2 /* 現在の接続 ＋ 次の待ち受け */, 64 * 1024, 64 * 1024, 0, &sa);
  };

  HANDLE h = create(true);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD err = GetLastError();
    std::fprintf(stderr, "error: パイプを作れません %s (%lu)%s\n", wide_to_utf8(full.c_str()).c_str(), err,
                 (err == ERROR_ACCESS_DENIED || err == ERROR_PIPE_BUSY) ? " — 既に起動していませんか" : "");
    LocalFree(sd);
    return 1;
  }
  // 起動した側（アドイン）が接続先を知るための 1 行。stdout が無ければ失敗しても構わない。
  {
    nlohmann::json ev = {{"event", "listening"}, {"pipe", wide_to_utf8(full.c_str())},
                         {"pid", static_cast<int>(GetCurrentProcessId())},
                         {"protocol", helper::proto::PROTOCOL_VERSION}};
    std::printf("%s\n", ev.dump().c_str());
    std::fflush(stdout);
  }

  bool quit = false;
  while (!quit) {
    const BOOL connected = ConnectNamedPipe(h, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
    if (!connected) {
      CloseHandle(h);
      h = create(false);
      if (h == INVALID_HANDLE_VALUE) break;
      continue;
    }
    // 次の待ち受けを**先に**作ってから今の接続を処理する。閉じてから作ると、その隙に
    // 同名のパイプを他プロセスに取られうる。
    HANDLE next = create(false);
    {
      PipeStream st(h);
      helper::proto::Server srv(session);
      quit = srv.serve(st);
      FlushFileBuffers(h);
      DisconnectNamedPipe(h);
      CloseHandle(h);
    }
    // 接続が切れたらセッションの状態（セル・候補・**対応表**）を捨てる。
    // 次の接続は白紙から始まり、前の作業の対応表がメモリに残らない。
    session.clear();
    if (next == INVALID_HANDLE_VALUE) break;
    h = next;
  }
  if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  LocalFree(sd);
  return 0;
}
#endif  // _WIN32

/// 親（Excel／アドイン）が消えたら自分も終わる。Job Object で縛られていなくても
/// 孤児にならないための保険。
void watch_parent(unsigned long pid) {
#ifdef _WIN32
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  if (!h) return;
  std::thread([h] {
    WaitForSingleObject(h, INFINITE);
    ExitProcess(0);
  }).detach();
#else
  std::thread([pid] {
    for (;;) {
      if (::kill(static_cast<pid_t>(pid), 0) != 0) _exit(0);
      ::sleep(1);
    }
  }).detach();
#endif
}

// ---------------------------------------------------------------- args
struct Args {
  bool help = false;
  bool selftest = false;
  bool stdio = false;
  bool pipe = false;
  std::string pipe_name;
  std::string data;
  unsigned long parent_pid = 0;
};

Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string t = argv[i];
    auto need = [&](const char* what) -> std::string {
      if (i + 1 >= argc) throw std::runtime_error(std::string("値がありません: ") + what);
      return argv[++i];
    };
    if (t == "-h" || t == "--help") a.help = true;
    else if (t == "--selftest") a.selftest = true;
    else if (t == "--stdio") a.stdio = true;
    else if (t == "--pipe") {
      a.pipe = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') a.pipe_name = argv[++i];
    }
    else if (t == "--data") a.data = need("--data");
    else if (t == "--parent-pid") a.parent_pid = std::strtoul(need("--parent-pid").c_str(), nullptr, 10);
    else throw std::runtime_error("不明なオプション: " + t);
  }
  if (a.stdio && a.pipe) throw std::runtime_error("--stdio と --pipe は同時に指定できません");
  return a;
}

void print_usage() {
  std::fprintf(stderr,
    "jp-pii-helper — Excel アドイン用ヘルパー（docs/excel-helper.md）\n"
    "\n"
    "  jp-pii-helper [--pipe [NAME]] [--data DIR] [--parent-pid N]   名前付きパイプで待ち受け（Windows 既定）\n"
    "  jp-pii-helper --stdio [--data DIR] [--parent-pid N]           標準入出力で待ち受け（POSIX 既定）\n"
    "  jp-pii-helper --selftest [--data DIR]                         自己診断\n"
    "\n"
    "  --data DIR      models ディレクトリ（既定: <exe の隣>/models、無ければ ./models）\n"
    "  --parent-pid N  この PID のプロセスが終了したら自分も終了する\n"
    "  --pipe NAME     \\\\.\\pipe\\NAME で待つ（既定 jp-pii-sanitizer-<ユーザーSID>）\n");
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  // Windows は argv を cp932 で渡す。--data に日本語（ユーザー名）が入ると内部の UTF-8 と
  // 一致しなくなるので、UTF-16 のコマンドラインから UTF-8 で作り直す。
  int wargc = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  std::vector<std::string> u8;
  std::vector<char*> u8ptr;
  if (wargv) {
    for (int i = 0; i < wargc; ++i) u8.push_back(wide_to_utf8(wargv[i]));
    LocalFree(wargv);
    for (auto& s : u8) u8ptr.push_back(s.data());
    argc = wargc;
    argv = u8ptr.data();
  }
#endif
  Args a;
  try {
    a = parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    print_usage();
    return 2;
  }
  if (a.help) {
    print_usage();
    return 0;
  }
  const std::string data_root = dataroot::resolve(a.data);
  if (a.selftest) return helper::selftest::run(data_root);

  if (a.parent_pid) watch_parent(a.parent_pid);
  helper::Session session(data_root);

#ifdef _WIN32
  const bool use_stdio = a.stdio;
#else
  const bool use_stdio = !a.pipe;
#endif
  if (use_stdio) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);   // フレームはバイナリ。CRLF 変換されると長さが狂う
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    StdioStream st;
    helper::proto::Server srv(session);
    srv.serve(st);
    session.clear();
    return 0;
  }
#ifdef _WIN32
  return run_pipe_server(session, utf8_to_wide(a.pipe_name));
#else
  std::fprintf(stderr, "error: 名前付きパイプは Windows のみです。--stdio を使ってください。\n");
  return 2;
#endif
}
