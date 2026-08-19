// UTF-8 パスでのファイル読み（移植性ギャップの吸収）。
//
// MSVC の std::ifstream はナロー(char)パスを ANSI コードページ（このマシンは cp932）として
// 解釈するため、UTF-8 の日本語ファイル名（例: 社員名簿_山田太郎_確認用.csv）を開けず、
// 内容が空になって CSV が 0 行になる。Windows では UTF-16 に変換し、ワイド版コンストラクタ
// （MSVC 拡張）で開く。Linux/POSIX は UTF-8 をそのまま扱えるので従来どおり。
// ※ 第三者製リーダー（miniz/PDFium）は内部で UTF-8→ワイド変換するので日本語名でも開けており、
//   壊れていたのは自前の std::ifstream 経路（read_file）だけ。
#pragma once

#include <fstream>
#include <ios>
#include <sstream>
#include <string>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace fileio {

#ifdef _WIN32
/// UTF-8 パス → UTF-16。ナロー版 fstream に渡すと ANSI(cp932) 解釈で日本語パスを開けない。
inline std::wstring widen(const std::string& path) {
  if (path.empty()) return std::wstring();
  int n = ::MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), &w[0], n);
  return w;
}
#endif

/// 読み取り用に開く。**必ずこれを通すこと**（素の ifstream は日本語パスで開けない）。
inline std::ifstream open_read(const std::string& path) {
#ifdef _WIN32
  return std::ifstream(widen(path), std::ios::binary);
#else
  return std::ifstream(path, std::ios::binary);
#endif
}

/// 書き込み用に開く。同上。
inline std::ofstream open_write(const std::string& path) {
#ifdef _WIN32
  return std::ofstream(widen(path), std::ios::binary);
#else
  return std::ofstream(path, std::ios::binary);
#endif
}

inline std::string read_all(const std::string& path) {
  std::ifstream f = open_read(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

/// 存在するか（開けるか）。素の ifstream での存在確認は日本語パスで必ず false になる。
inline bool exists(const std::string& path) {
  std::ifstream f = open_read(path);
  return f.good();
}

/// 書き出し。成否を返す。
inline bool write_all(const std::string& path, const std::string& data) {
  std::ofstream f = open_write(path);
  if (!f) return false;
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
  return f.good();
}

}  // namespace fileio
