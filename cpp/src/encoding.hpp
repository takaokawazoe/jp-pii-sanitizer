// 平文ファイルの文字コード復号（cp932 フォールバック）。
//
// **なぜ要るか**: 日本語 Windows の業務 CSV/TXT は Shift_JIS(cp932) が普通にある。
// UTF-8 として読むと不正バイト列になり、Rust シム（tokenizers / sudachi）の
// `CStr::to_str()` が失敗して例外 → 対処しないとプロセスが落ちる。PCRE2 も
// PCRE2_ERROR_UTF8_ERR* を返すので「PII ゼロ件」に見える（＝fail-open）。
//
// **移植性**: cp932 の変換表は OS 依存。Windows は MultiByteToWideChar(932) を使う。
// Linux/POSIX にはこの表が無い（iconv は環境依存で CI に持ち込みたくない）ので
// 復号は行わず false を返し、呼び出し側が utf8::repair で U+FFFD に潰す。
// 適合試験のフィクスチャは全て UTF-8 なので CI の結果は変わらない。
#pragma once

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

namespace encoding {

/// cp932 (Shift_JIS) → UTF-8。成功時のみ true を返し `out` を書く。
///
/// MB_ERR_INVALID_CHARS を付けるので、cp932 として解釈できないバイト列は失敗する。
/// 「UTF-8 でも cp932 でもない」ものを黙って化けさせないための保険。
inline bool cp932_to_utf8(const std::string& in, std::string& out) {
  if (in.empty()) {
    out.clear();
    return true;
  }
#ifdef _WIN32
  const int wn = ::MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, in.data(),
                                       static_cast<int>(in.size()), nullptr, 0);
  if (wn <= 0) return false;
  std::wstring w(static_cast<std::size_t>(wn), L'\0');
  if (::MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, in.data(), static_cast<int>(in.size()),
                            &w[0], wn) <= 0)
    return false;
  const int un = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
  if (un <= 0) return false;
  std::string u(static_cast<std::size_t>(un), '\0');
  if (::WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, &u[0], un, nullptr, nullptr) <= 0)
    return false;
  out.swap(u);
  return true;
#else
  (void)in;
  (void)out;
  return false;  // POSIX: 変換表を持たない（呼び出し側が U+FFFD で潰す）
#endif
}

}  // namespace encoding
