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

inline std::string read_all(const std::string& path) {
#ifdef _WIN32
  int n = ::MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), &w[0], n);
  std::ifstream f(w, std::ios::binary);
#else
  std::ifstream f(path, std::ios::binary);
#endif
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace fileio
