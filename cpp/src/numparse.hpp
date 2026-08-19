// 壊れた入力に耐える数値パース。
//
// **なぜ std::stoi を直接使わないか**: 変換できない文字列で `std::invalid_argument`、
// 範囲外で `std::out_of_range` を投げる。XML 属性やヘッダの値はユーザーが持ち込む
// 壊れたファイル由来なので、ここで例外が出るとそのままプロセスまで飛ぶ
// （GUI は WebView2 のコールバックを突き抜けて terminate する）。既定値で流す。
#pragma once

#include <cerrno>   // errno / ERANGE（MSVC は cstdlib 経由で入るが GCC では明示が要る）
#include <cstdlib>
#include <string>

namespace numparse {

/// 10進整数として読む。読めなければ `def`。先頭の空白は許すが、末尾のゴミも許容する
/// （XML の "12 " 等）。完全一致を要求したい場面は呼び出し側で確かめること。
inline int to_int(const std::string& s, int def = 0) {
  if (s.empty()) return def;
  errno = 0;
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (end == s.c_str() || errno == ERANGE) return def;
  if (v < -2147483647L - 1 || v > 2147483647L) return def;
  return static_cast<int>(v);
}

/// 16進整数として読む（quoted-printable の "=XX" 等）。読めなければ `def`。
inline int to_int_hex(const std::string& s, int def = 0) {
  if (s.empty()) return def;
  errno = 0;
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 16);
  if (end == s.c_str() || errno == ERANGE) return def;
  return static_cast<int>(v);
}

}  // namespace numparse
