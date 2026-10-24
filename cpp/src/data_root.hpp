// モデル・辞書ディレクトリ（data root）の解決。CLI とヘルパーが共有する。
//
//   --data DIR > <exe_dir>/models > ./models
//
// ポータブル ZIP でもアドインのインストール先（%LOCALAPPDATA%）でも、exe の隣の
// models/ をフラグ無しで見つけることがねらい。
#pragma once

#include <string>

#include "file_io.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace dataroot {

/// 実行ファイルのあるディレクトリ（UTF-8）。取れなければ "."。
inline std::string exe_dir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return ".";
  const std::wstring w(buf, n);
  const auto slash = w.find_last_of(L"\\/");
  const std::wstring dir = (slash == std::wstring::npos) ? L"." : w.substr(0, slash);
  // 日本語ユーザー名のプロファイル配下に置かれることがあるので、UTF-8 で返す
  // （ファイルを開くときは fileio がワイド版に戻す）。
  const int len = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), static_cast<int>(dir.size()),
                                      nullptr, 0, nullptr, nullptr);
  if (len <= 0) return ".";
  std::string out(static_cast<std::size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), static_cast<int>(dir.size()), out.data(), len,
                      nullptr, nullptr);
  return out;
#else
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return ".";
  const std::string p(buf, static_cast<std::size_t>(n));
  const auto slash = p.find_last_of('/');
  return slash == std::string::npos ? "." : p.substr(0, slash);
#endif
}

/// models ディレクトリ。override_dir が空でなければそれを返す。
///
/// **素の std::ifstream で存在確認しないこと。** MSVC はナローパスを ANSI(cp932) と
/// して解釈するため、日本語を含むパスで必ず false になる。fileio はワイド版で開く。
inline std::string resolve(const std::string& override_dir) {
  if (!override_dir.empty()) return override_dir;
  const std::string beside = exe_dir() + "/models";
  if (fileio::exists(beside + "/patterns.json")) return beside;
  if (fileio::exists("models/patterns.json")) return "models";
  return beside;  // 失敗時のエラーメッセージ用にとりあえず返す
}

}  // namespace dataroot
