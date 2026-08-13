// Phase 5: WebView2 ホスト（イテレーション2 — コア配線）。
//
// app.py（Streamlit）の画面①〜④を、WebView2 越しに C++ コア（appcore::Core）へつなぐ。
//   JS →(postMessage: JSON文字列)→ native →(appcore)→(PostWebMessageAsJson)→ JS
// コマンド: open / extractPaths / preview / sanitize / reverse。
//
// GUI を目視せず配線を検証するため、`--selftest <log> [samplePath]` 指定時はネイティブから
// 直接 core を叩き（抽出→候補→サニタイズ）結果をログに書いて自動終了する。

// NOMINMAX: windows.h の min/max マクロがコアの std::min/std::max を壊すのを防ぐ
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW
#include <shobjidl.h>
#include <wrl.h>

#include "WebView2.h"

#include <fstream>
#include <string>
#include <vector>

#include "app/core.hpp"
#include "json.hpp"

using namespace Microsoft::WRL;
using nlohmann::json;

namespace {

ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
HWND g_hwnd = nullptr;
bool g_selftest = false;
std::wstring g_selftest_log;
std::string g_selftest_sample;
std::unique_ptr<appcore::Core> g_core;

// ---- 文字コード変換（Win32 は UTF-16、コアは UTF-8） ----
std::wstring to_wide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}
std::string to_utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

void log_line(const std::wstring& line) {
  if (g_selftest_log.empty()) return;
  std::wofstream f(g_selftest_log, std::ios::app);
  f << line << L"\n";
}

// ---- exe から models/ ディレクトリを解決 ----
std::string exe_dir() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring p(buf, n);
  const auto slash = p.find_last_of(L"\\/");
  return to_utf8(slash == std::wstring::npos ? p : p.substr(0, slash));
}
std::string resolve_data_root() {
  const std::string dir = exe_dir();
  // 配布時は exe の隣、開発時は build/ の1つ上（cpp/models）
  for (const std::string cand : {dir + "/models", dir + "/../models"}) {
    std::ifstream f(cand + "/patterns.json");
    if (f.good()) return cand;
  }
  return dir + "/models";
}

// WebView2 のユーザーデータフォルダ。既定は exe の隣に作られるため、読み取り専用の場所や
// ネットワーク共有から起動すると失敗する。%LOCALAPPDATA% 配下に固定してどこからでも動くように。
std::wstring user_data_folder() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  const std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : L".";
  return base + L"\\jp-pii-sanitizer\\WebView2";
}

// ---- ネイティブのファイルオープンダイアログ（複数選択） ----
std::vector<std::string> pick_files() {
  std::vector<std::string> out;
  ComPtr<IFileOpenDialog> dlg;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dlg))))
    return out;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
  COMDLG_FILTERSPEC filters[] = {
      {L"対応ファイル", L"*.docx;*.pptx;*.xlsx;*.pdf;*.csv;*.txt;*.md"},
      {L"すべて", L"*.*"}};
  dlg->SetFileTypes(2, filters);
  if (FAILED(dlg->Show(g_hwnd))) return out;  // キャンセル含む
  ComPtr<IShellItemArray> items;
  if (FAILED(dlg->GetResults(&items))) return out;
  DWORD count = 0;
  items->GetCount(&count);
  for (DWORD i = 0; i < count; ++i) {
    ComPtr<IShellItem> item;
    if (FAILED(items->GetItemAt(i, &item))) continue;
    LPWSTR path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
      out.push_back(to_utf8(path));
      CoTaskMemFree(path);
    }
  }
  return out;
}

void post_json(const json& j) {
  if (g_webview) g_webview->PostWebMessageAsJson(to_wide(j.dump()).c_str());
}

// ---- ネイティブの保存ダイアログ（保存先をユーザーが選ぶ） ----
// CSV は Excel が日本語を正しく開けるよう UTF-8 BOM を付ける。キャンセル時 false。
bool save_file(const std::wstring& suggested, const std::string& utf8, bool add_bom) {
  ComPtr<IFileSaveDialog> dlg;
  if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
    return false;
  dlg->SetFileName(suggested.c_str());
  COMDLG_FILTERSPEC filters[] = {{L"テキスト", L"*.txt"}, {L"CSV", L"*.csv"}, {L"すべて", L"*.*"}};
  dlg->SetFileTypes(3, filters);
  const bool is_csv = suggested.size() >= 4 && suggested.substr(suggested.size() - 4) == L".csv";
  dlg->SetFileTypeIndex(is_csv ? 2 : 1);
  dlg->SetDefaultExtension(is_csv ? L"csv" : L"txt");
  if (FAILED(dlg->Show(g_hwnd))) return false;  // キャンセル含む
  ComPtr<IShellItem> item;
  if (FAILED(dlg->GetResult(&item))) return false;
  LPWSTR path = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return false;
  const std::wstring wp(path);
  CoTaskMemFree(path);
  std::ofstream out(wp, std::ios::binary);  // MSVC は wchar_t パスの ofstream を受ける
  if (!out) return false;
  if (add_bom) {
    const char bom[3] = {static_cast<char>(0xEF), static_cast<char>(0xBB), static_cast<char>(0xBF)};
    out.write(bom, 3);
  }
  out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
  return out.good();
}

std::vector<tokenizer::ConfirmedTerm> parse_confirmed(const json& arr) {
  std::vector<tokenizer::ConfirmedTerm> conf;
  if (arr.is_array())
    for (const auto& x : arr)
      conf.push_back({x.value("text", std::string()), x.value("type", std::string())});
  return conf;
}

// ---- JS からのコマンドを処理 ----
void handle_command(const std::string& raw) {
  json req;
  try {
    req = json::parse(raw);
  } catch (...) {
    return;  // JSON でないメッセージ（self-test の ping 等）は無視
  }
  const std::string cmd = req.value("cmd", std::string());
  const auto id = req.value("id", 0);
  json resp;
  resp["id"] = id;

  if (cmd == "pickFiles") {
    // ダイアログで選ぶだけ（抽出しない）。ファイル一覧の管理は JS 側（accumulate）。
    resp["type"] = "picked";
    resp["paths"] = pick_files();  // キャンセル時は空配列
    post_json(resp);
  } else if (cmd == "open") {
    const auto paths = pick_files();
    if (paths.empty()) { resp["ok"] = false; resp["cancelled"] = true; post_json(resp); return; }
    auto r = g_core->extract(paths);
    r["id"] = id; r["type"] = "extract";
    post_json(r);
  } else if (cmd == "extractPaths") {
    std::vector<std::string> paths;
    for (const auto& p : req["paths"]) paths.push_back(p.get<std::string>());
    auto r = g_core->extract(paths);
    r["id"] = id; r["type"] = "extract";
    post_json(r);
  } else if (cmd == "preview") {
    auto r = g_core->preview(parse_confirmed(req["confirmed"]));
    r["id"] = id; r["type"] = "preview";
    post_json(r);
  } else if (cmd == "sanitize") {
    auto r = g_core->sanitize(parse_confirmed(req["confirmed"]), req.value("reversible", true));
    r["id"] = id; r["type"] = "sanitize";
    post_json(r);
  } else if (cmd == "reverse") {
    std::vector<std::pair<std::string, std::string>> map;
    for (const auto& m : req["mapping"])
      map.emplace_back(m.value("token", std::string()), m.value("original", std::string()));
    auto r = g_core->reverse(req.value("text", std::string()), map);
    r["id"] = id; r["type"] = "reverse";
    post_json(r);
  } else if (cmd == "saveFile") {
    const std::string name = req.value("name", std::string("output.txt"));
    const std::string content = req.value("content", std::string());
    const bool is_csv = name.size() >= 4 && name.substr(name.size() - 4) == ".csv";
    resp["ok"] = save_file(to_wide(name), content, is_csv);  // CSV は BOM 付き
    resp["type"] = "save";
    post_json(resp);
  }
}

// ---- self-test: ネイティブから直接 core を叩いて配線を検証 ----
void run_selftest() {
  log_line(L"selftest: init...");
  if (!g_core->ensure_init()) {
    log_line(L"init FAILED: " + to_wide(g_core->last_error()));
    PostQuitMessage(4);
    return;
  }
  log_line(L"init OK");
  auto ex = g_core->extract({g_selftest_sample});
  const int ncand = ex.value("candidates", json::array()).size();
  int nspan = 0;
  for (const auto& b : ex.value("blocks", json::array())) nspan += (int)b.value("spans", json::array()).size();
  log_line(L"extract: candidates=" + std::to_wstring(ncand) + L" spans=" + std::to_wstring(nspan));

  // 全候補をマスク対象にしてサニタイズ（可逆）
  std::vector<tokenizer::ConfirmedTerm> conf;
  for (const auto& c : ex["candidates"])
    conf.push_back({c.value("text", std::string()), c.value("entity_type", std::string())});
  auto san = g_core->sanitize(conf, true);
  const int nleak = san.value("leaks", json::array()).size();
  const int nmap = san.value("mapping", json::array()).size();
  log_line(L"sanitize: mapping=" + std::to_wstring(nmap) + L" leaks=" + std::to_wstring(nleak));

  const bool pass = ncand > 0 && nleak == 0;
  log_line(pass ? L"SELFTEST PASS" : L"SELFTEST FAIL");
  PostQuitMessage(pass ? 0 : 5);
}

// UI は src/app/index.html に分離（MSVC の文字列リテラル上限 C2026 回避＋編集容易化）。
// ビルド時に exe の隣へコピーする（CMake POST_BUILD）。実行時はそこから読む。
std::wstring load_ui_html() {
  const std::string dir = exe_dir();
  for (const std::string cand :
       {dir + "/index.html", dir + "/../src/app/index.html", dir + "/../../src/app/index.html"}) {
    const std::string html = fileio::read_all(cand);
    if (!html.empty()) return to_wide(html);
  }
  return L"<!doctype html><meta charset=utf-8><h2>index.html が見つかりません</h2>";
}

void on_web_message(const std::wstring& wmsg) {
  handle_command(to_utf8(wmsg));
}

void resize_to_client() {
  if (!g_controller || !g_hwnd) return;
  RECT rc; GetClientRect(g_hwnd, &rc);
  g_controller->put_Bounds(rc);
}

HRESULT on_controller_created(HRESULT result, ICoreWebView2Controller* controller) {
  if (FAILED(result) || !controller) { log_line(L"controller FAILED"); if (g_selftest) PostQuitMessage(2); return result; }
  g_controller = controller;
  g_controller->get_CoreWebView2(&g_webview);
  resize_to_client();

  EventRegistrationToken tok{};
  g_webview->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            LPWSTR raw = nullptr;
            if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
              std::wstring m(raw); CoTaskMemFree(raw); on_web_message(m);
            }
            return S_OK;
          }).Get(), &tok);

  g_webview->NavigateToString(load_ui_html().c_str());
  return S_OK;
}

HRESULT on_env_created(HRESULT result, ICoreWebView2Environment* env) {
  if (FAILED(result) || !env) { log_line(L"environment FAILED"); if (g_selftest) PostQuitMessage(3); return result; }
  env->CreateCoreWebView2Controller(
      g_hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(on_controller_created).Get());
  return S_OK;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SIZE: resize_to_client(); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void parse_cmdline() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return;
  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    if (a == L"--selftest" && i + 1 < argc) { g_selftest = true; g_selftest_log = argv[++i]; }
    else if (!a.empty() && a[0] != L'-') g_selftest_sample = to_utf8(a);
  }
  LocalFree(argv);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow) {
  parse_cmdline();
  if (!g_selftest_log.empty()) { std::wofstream f(g_selftest_log, std::ios::trunc); f << L"start\n"; }

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  g_core = std::make_unique<appcore::Core>(resolve_data_root());

  // self-test はウィンドウも WebView2 も使わず、ネイティブから直接 core を叩く
  if (g_selftest) {
    run_selftest();
    MSG m;  // PostQuitMessage を消化
    while (GetMessageW(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    CoUninitialize();
    return (int)m.wParam;
  }

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc); wc.lpfnWndProc = wnd_proc; wc.hInstance = hInst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"JpPiiSanitizerWnd";
  wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));    // app.rc の ICON 1（タイトルバー/タスクバー）
  wc.hIconSm = wc.hIcon;
  RegisterClassExW(&wc);
  // 既定サイズは画面の作業領域に対して大きめ（ただし画面をはみ出さない）にして中央へ。
  // 以前の 1100x760 だと縦が足りずサニタイズ実行ボタンが隠れることがあった。
  RECT wa{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  const int waW = wa.right - wa.left, waH = wa.bottom - wa.top;
  int winW = waW - 80; if (winW > 1360) winW = 1360;
  int winH = waH - 60; if (winH > 960) winH = 960;
  const int winX = wa.left + (waW - winW) / 2;
  const int winY = wa.top + (waH - winH) / 2;
  g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"PIIサニタイザ", WS_OVERLAPPEDWINDOW,
                           winX, winY, winW, winH, nullptr, nullptr, hInst, nullptr);
  if (!g_hwnd) return 1;
  ShowWindow(g_hwnd, nShow); UpdateWindow(g_hwnd);

  const std::wstring udf = user_data_folder();
  HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, udf.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(on_env_created).Get());
  if (FAILED(hr)) {
    MessageBoxW(g_hwnd, L"WebView2 ランタイムが見つかりません。", L"起動エラー", MB_ICONERROR);
    return 1;
  }

  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
  g_controller.Reset(); g_webview.Reset(); g_core.reset();
  CoUninitialize();
  return 0;
}
