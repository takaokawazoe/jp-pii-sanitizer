// 文字コード → UTF-8 の変換。
//
// **なぜ要るか**: 日本語 Windows の業務 CSV/TXT は Shift_JIS(cp932) が普通にあり、
// 日本語のメール(.eml/.msg)のヘッダと本文は ISO-2022-JP が今でも珍しくない。
// UTF-8 として読むと不正バイト列になり、Rust シム（tokenizers / sudachi）の
// `CStr::to_str()` が失敗して例外 → 対処しないとプロセスが落ちる。PCRE2 も
// PCRE2_ERROR_UTF8_ERR* を返すので「PII ゼロ件」に見える（＝fail-open）。
// 変換しない場合の出口は utf8::repair による U+FFFD 潰しで、これは
// 「読めなかった」ではなく「PII が無かった」として利用者に見える。それが一番まずい。
//
// **移植性**: 変換表は OS 依存。Windows は MultiByteToWideChar、POSIX は iconv を使う。
// iconv は glibc 本体の一部なので **追加パッケージは要らない**（ubuntu の CI で
// ISO-2022-JP / CP932 / EUC-JP の往復を実測済み）。musl(Alpine) の iconv は
// スタブでこれらの表を持たないため、その環境では known_charset が false になる。
// 以前ここには「iconv は環境依存で CI に持ち込みたくない」と書いていたが、
// 追加依存が要らないことを確認したので方針を変えた。
#pragma once

#include <cctype>
#include <string>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <errno.h>
#  include <iconv.h>
#endif

namespace encoding {

namespace detail {

/// charset 名の表記ゆれを潰す（引用符・空白・大小・区切りの - _ を無視）。
inline std::string normalize_charset(const std::string& cs) {
  std::string s;
  for (char c : cs) {
    if (c == '"' || c == '\'' || c == ' ' || c == '\t' || c == '-' || c == '_') continue;
    s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

/// ISO-2022-JP は OS の変換表に任せず自前で剥がす（理由は下の iso2022jp_to_cp932）。
/// cp（コードページ）の位置に置く番兵で、実在のコードページ番号ではない。
inline constexpr unsigned ISO2022JP_MARKER = 0xFFFFFFFFu;

/// 変換先の指定。passthrough は「既に UTF-8 互換なので触らない」。
struct Target {
  bool known = false;
  bool passthrough = false;
  unsigned cp = 0;              // Windows のコードページ
  const char* iconv_name = "";  // POSIX の iconv 名
};

inline Target target_for(const std::string& charset) {
  const std::string s = normalize_charset(charset);
  // 未指定は RFC 2045 の既定（us-ascii）だが、実務では UTF-8 の方が当たる。
  // どちらにせよ素通しなので同じ扱いでよい。
  if (s.empty() || s == "utf8" || s == "usascii" || s == "ascii" || s == "ansix3.41")
    return {true, true, 0, ""};
  // ISO-2022-JP 系（RFC 1468）。ISO2022JP_MARKER は自前で剥がす合図。
  if (s == "iso2022jp" || s == "iso2022jp1" || s == "iso2022jp2" || s == "csiso2022jp" ||
      s == "junetcode")
    return {true, false, ISO2022JP_MARKER, "ISO-2022-JP"};
  if (s == "shiftjis" || s == "sjis" || s == "xsjis" || s == "cp932" || s == "windows31j" ||
      s == "ms932" || s == "mskanji" || s == "csshiftjis")
    return {true, false, 932, "CP932"};
  if (s == "eucjp" || s == "xeucjp" || s == "cseucpkdfmtjapanese" || s == "ujis")
    return {true, false, 20932, "EUC-JP"};  // Win32 は 20932。51932 は .NET 側の番号で無効
  // 欧文。無いと「Latin-1 の英文メール」が丸ごと不明扱いになるので入れてある。
  if (s == "iso88591" || s == "latin1" || s == "cslatin1") return {true, false, 28591, "ISO-8859-1"};
  if (s == "windows1252" || s == "cp1252") return {true, false, 1252, "CP1252"};
  return {};  // 未知（呼び出し側が「読めなかった」として扱う）
}


/// ISO-2022-JP (RFC 1468) → cp932 バイト列。成功時のみ true。
///
/// **なぜ OS に任せないか**: Windows のコードページ 50220/50221/50222 は
/// 「ASCII に戻るエスケープ」を解釈できない。`ESC ( B` も `ESC ( J` も U+E12A（私用領域）
/// になり、以降の ASCII が JIS の 2 バイトとして食われて本文が崩れる（実測）。
/// 日本語メールは「ASCII → JIS → ASCII」を行ごとに繰り返すので、これは致命的。
///
/// JIS X 0208 → Shift_JIS は**表が要らない算術変換**なので、エスケープの解釈だけ自前で
/// やって cp932 に落とし、cp932 → UTF-8 は OS（Windows なら 932・POSIX なら iconv）に渡す。
/// この段は両プラットフォームで共通なので、CI と利用者の手元で同じ結果になる。
///
/// ISO-2022-JP-2 の中国語/韓国語(`ESC $ A` 等)と JIS X 0212(`ESC $ ( D`)は扱わない。
/// 黙って化けさせず false を返し、呼び出し側が「読めなかった」と言えるようにする。
inline bool iso2022jp_to_cp932(const std::string& in, std::string& out) {
  enum class Mode { Ascii, Jis0208, Kana };
  Mode mode = Mode::Ascii;
  std::string o;
  o.reserve(in.size());
  for (std::size_t i = 0; i < in.size();) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    if (c == 0x1B) {  // エスケープシーケンス
      if (i + 2 < in.size() && in[i + 1] == '(') {
        const char f = in[i + 2];
        if (f == 'B' || f == 'J') { mode = Mode::Ascii; i += 3; continue; }   // ASCII / JIS-Roman
        if (f == 'I') { mode = Mode::Kana; i += 3; continue; }                // 半角カナ
        return false;
      }
      if (i + 2 < in.size() && in[i + 1] == '$') {
        const char f = in[i + 2];
        if (f == '@' || f == 'B') { mode = Mode::Jis0208; i += 3; continue; }  // JIS78 / JIS83
        return false;  // ESC $ A（GB2312）/ ESC $ ( D（JIS X 0212）等は非対応
      }
      return false;
    }
    if (c == '\r' || c == '\n') {  // 行末は ASCII に戻る（戻し忘れの実装への保険）
      mode = Mode::Ascii;
      o += static_cast<char>(c);
      ++i;
      continue;
    }
    if (mode == Mode::Ascii) {
      if (c >= 0x80) return false;  // 7bit のはずの場所に 8bit が来たら別の charset
      o += static_cast<char>(c);
      ++i;
      continue;
    }
    if (mode == Mode::Kana) {
      if (c < 0x21 || c > 0x5F) return false;
      o += static_cast<char>(c + 0x80);  // JIS X 0201 カナ → cp932 の 0xA1..0xDF
      ++i;
      continue;
    }
    // JIS X 0208: 2 バイトで 1 文字。区点 → Shift_JIS は算術で求まる。
    if (i + 1 >= in.size()) return false;
    const unsigned h = c, l = static_cast<unsigned char>(in[i + 1]);
    if (h < 0x21 || h > 0x7E || l < 0x21 || l > 0x7E) return false;
    const unsigned s1 = ((h - 1) >> 1) + (h < 0x5F ? 0x71 : 0xB1);
    const unsigned s2 = l + ((h & 1) ? (l < 0x60 ? 0x1F : 0x20) : 0x7E);
    o += static_cast<char>(s1);
    o += static_cast<char>(s2);
    i += 2;
  }
  out.swap(o);
  return true;
}
}  // namespace detail

/// この charset を扱えるか。false のとき変換は試みず、失敗として扱うのが正しい。
inline bool known_charset(const std::string& charset) {
  return detail::target_for(charset).known;
}

/// charset → UTF-8。成功時のみ true を返し `out` を書く。
///
/// **変換できないバイト列は黙って化けさせず false を返す。** 「UTF-8 でも cp932 でもない」
/// ものが U+FFFD だらけの本文として下流に流れると、PII ゼロ件と区別が付かなくなる。
inline bool charset_to_utf8(const std::string& charset, const std::string& in, std::string& out) {
  const detail::Target t = detail::target_for(charset);
  if (!t.known) return false;
  if (t.passthrough) {
    out = in;
    return true;
  }
  if (in.empty()) {
    out.clear();
    return true;
  }
  // ISO-2022-JP は**両プラットフォーム共通で**自前で cp932 に落としてから OS に渡す。
  // Windows のコードページが使い物にならないため（iso2022jp_to_cp932 のコメント参照）。
  if (t.cp == detail::ISO2022JP_MARKER) {
    std::string sjis;
    if (!detail::iso2022jp_to_cp932(in, sjis)) return false;
    return charset_to_utf8("cp932", sjis, out);
  }
#ifdef _WIN32
  // MB_ERR_INVALID_CHARS を受け付けないコードページがあるので、拒否されたら 0 で再試行する。
  // 付けられる限りは付ける（「cp932 として読めないバイト列」を黙って化けさせないため）。
  DWORD flags = MB_ERR_INVALID_CHARS;
  int wn = ::MultiByteToWideChar(t.cp, flags, in.data(), static_cast<int>(in.size()), nullptr, 0);
  if (wn <= 0 && ::GetLastError() == ERROR_INVALID_FLAGS) {
    flags = 0;
    wn = ::MultiByteToWideChar(t.cp, flags, in.data(), static_cast<int>(in.size()), nullptr, 0);
  }
  if (wn <= 0) return false;
  std::wstring w(static_cast<std::size_t>(wn), L'\0');
  if (::MultiByteToWideChar(t.cp, flags, in.data(), static_cast<int>(in.size()), &w[0], wn) <= 0)
    return false;
  const int un = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
  if (un <= 0) return false;
  std::string u(static_cast<std::size_t>(un), '\0');
  if (::WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, &u[0], un, nullptr, nullptr) <= 0)
    return false;
  out.swap(u);
  return true;
#else
  const iconv_t cd = ::iconv_open("UTF-8", t.iconv_name);
  if (cd == reinterpret_cast<iconv_t>(-1)) return false;  // musl 等、表を持たない環境
  std::string u;
  u.resize(in.size() * 4 + 8);
  char* inbuf = const_cast<char*>(in.data());
  std::size_t inleft = in.size();
  char* outbuf = &u[0];
  std::size_t outleft = u.size();
  bool ok = true;
  while (inleft > 0) {
    const std::size_t r = ::iconv(cd, &inbuf, &inleft, &outbuf, &outleft);
    if (r != static_cast<std::size_t>(-1)) break;
    if (errno == E2BIG) {  // 出力不足だけは伸ばして続ける
      const std::size_t used = u.size() - outleft;
      u.resize(u.size() * 2);
      outbuf = &u[0] + used;
      outleft = u.size() - used;
      continue;
    }
    ok = false;  // EILSEQ / EINVAL は「この charset として読めない」
    break;
  }
  ::iconv_close(cd);
  if (!ok) return false;
  u.resize(u.size() - outleft);
  out.swap(u);
  return true;
#endif
}

/// cp932 (Shift_JIS) → UTF-8。平文ファイルの復号から使う既存の入口。
inline bool cp932_to_utf8(const std::string& in, std::string& out) {
  return charset_to_utf8("cp932", in, out);
}

}  // namespace encoding
