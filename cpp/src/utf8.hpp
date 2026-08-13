// UTF-8 と Python 互換の文字列ユーティリティ。
//
// **Python の文字列は文字（コードポイント）単位、C++ の std::string はバイト列。**
// step2/furigana は `s[a:b]`・`len(s)`・`splitlines()` を文字単位で使うので、ここを
// バイトのまま写すと日本語で必ず壊れる。変換を1箇所に集める。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace utf8 {

/// UTF-8 文字列の「コードポイント境界のバイト位置」表。長さ (文字数+1)。
inline std::vector<std::size_t> char_offsets(const std::string& s) {
  std::vector<std::size_t> off;
  off.reserve(s.size() + 1);
  for (std::size_t i = 0; i < s.size();) {
    off.push_back(i);
    const unsigned char c = static_cast<unsigned char>(s[i]);
    i += (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
  }
  off.push_back(s.size());
  return off;
}

/// 文字数（Python の len(s) 相当）。
inline std::size_t char_len(const std::string& s) { return char_offsets(s).size() - 1; }

/// 1コードポイントが空白か。Python の str.strip() が落とす範囲のうち、日本語文書で
/// 実際に出るもの（ASCII空白・U+3000 全角空白・U+00A0 NBSP）を見る。
inline bool is_space_cp(const std::string& s, std::size_t at, std::size_t len) {
  const auto u = [&](std::size_t i) { return static_cast<unsigned char>(s[at + i]); };
  if (len == 1) {
    const char c = s[at];
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
  }
  if (len == 2) return u(0) == 0xC2 && u(1) == 0xA0;                  // U+00A0 NBSP
  if (len == 3) return u(0) == 0xE3 && u(1) == 0x80 && u(2) == 0x80;  // U+3000 全角空白
  return false;
}

/// 文字単位 [begin, end) を切り出す（Python の s[a:b] 相当）。
inline std::string slice(const std::string& s, const std::vector<std::size_t>& off,
                        std::size_t begin, std::size_t end) {
  if (begin >= off.size() || end >= off.size() || begin > end) return {};
  return s.substr(off[begin], off[end] - off[begin]);
}

inline std::string slice(const std::string& s, std::size_t begin, std::size_t end) {
  return slice(s, char_offsets(s), begin, end);
}

/// Python の str.strip() 相当。
///
/// **バイト単位の find_first_not_of(" \t\r\n　") を使ってはいけない。** 全角空白 U+3000 は
/// UTF-8 で E3 80 80 なので、除外集合に生バイト 0xE3/0x80 が混ざり、多バイト文字の一部を
/// 空白と誤認して壊す（実測: 「田中健一」→「田中健\xE4\xB8」、「グローバル…」→先頭が欠ける）。
inline std::string trim(const std::string& s) {
  const auto off = char_offsets(s);
  if (off.size() < 2) return s;
  std::size_t b = 0, e = off.size() - 1;
  while (b < e && is_space_cp(s, off[b], off[b + 1] - off[b])) ++b;
  while (e > b && is_space_cp(s, off[e - 1], off[e] - off[e - 1])) --e;
  return slice(s, off, b, e);
}

/// Python の str.splitlines(keepends=True) 相当。
///
/// **`\n` だけで割ってはいけない。** Python は `\r`・`\r\n`・`\v`・`\f`・`\x1c`〜`\x1e`・
/// `\x85`(NEL)・` `(LS)・` `(PS) でも分割する。現在のデモ5文書は `\n` のみだが、
/// PDFium は `\r\n` を出す（Phase 4b 実測）ので、Phase 4 で C++ 抽出に替えると効いてくる。
inline std::vector<std::string> splitlines_keepends(const std::string& s) {
  std::vector<std::string> out;
  const auto off = char_offsets(s);
  const std::size_t n = off.size() - 1;
  std::size_t start = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const std::string ch = slice(s, off, i, i + 1);
    bool is_break = false;
    std::size_t skip = 1;
    if (ch == "\n" || ch == "\v" || ch == "\f" || ch == "\x1c" || ch == "\x1d" ||
        ch == "\x1e" || ch == "" || ch == " " || ch == " ") {
      is_break = true;
    } else if (ch == "\r") {
      is_break = true;
      if (i + 1 < n && slice(s, off, i + 1, i + 2) == "\n") skip = 2;  // \r\n は1つの区切り
    }
    if (is_break) {
      out.push_back(slice(s, off, start, i + skip));
      i += skip - 1;
      start = i + 1;
    }
  }
  if (start < n) out.push_back(slice(s, off, start, n));
  return out;
}

/// step2._chunk_by_chars の移植。行単位で max_chars 以下に分割し (先頭文字位置, 文字列) を返す。
///
/// HF transformer(xlm-roberta) の入力は 512 トークン上限。日本語は概ね 1.5〜2字/トークンで、
/// 余裕を見て 250 字ごとに分割する。行=意味単位なので行境界で切り、固有名詞を途中で割らない。
/// 単体で上限超の行はハード分割する。
inline std::vector<std::pair<std::size_t, std::string>> chunk_by_chars(const std::string& text,
                                                                      std::size_t max_chars) {
  std::vector<std::pair<std::size_t, std::string>> chunks;
  std::string buf;
  std::size_t buf_len = 0, buf_start = 0, base = 0;
  for (const auto& line : splitlines_keepends(text)) {
    const std::size_t ln = char_len(line);
    if (ln > max_chars) {  // 1行が上限超え → 文字単位でハード分割
      if (!buf.empty()) {
        chunks.emplace_back(buf_start, buf);
        buf.clear();
        buf_len = 0;
      }
      const auto loff = char_offsets(line);
      for (std::size_t i = 0; i < ln; i += max_chars)
        chunks.emplace_back(base + i, slice(line, loff, i, std::min(i + max_chars, ln)));
      base += ln;
      continue;
    }
    if (!buf.empty() && buf_len + ln > max_chars) {
      chunks.emplace_back(buf_start, buf);
      buf.clear();
      buf_len = 0;
    }
    if (buf.empty()) buf_start = base;
    buf += line;
    buf_len += ln;
    base += ln;
  }
  if (!buf.empty()) chunks.emplace_back(buf_start, buf);
  return chunks;
}

}  // namespace utf8
