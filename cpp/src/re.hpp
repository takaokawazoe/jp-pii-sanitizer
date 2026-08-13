// PCRE2 の薄いラッパ（Python の re と同じ挙動に寄せる）。
//
// **なぜ RE2 でなく PCRE2 か**（当初案は RE2 だったが誤り）:
//   tokenizer.py の PHONE_RE / POSTAL_RE が**後読み** `(?<![\d\-])` を使っており、
//   RE2 は後読み・先読みを実装していない。PCRE2 は Perl 互換で Python re に近い。
//
// **Python re と揃えるために必須のフラグ**:
//   - PCRE2_UTF : `[一-龥]` 等の Unicode 範囲を効かせる（無いとバイト単位で無意味になる）
//   - PCRE2_UCP : `\s`/`\d`/`\w` を Unicode 対応にする。Python の re は str に対し
//                 `\s` が全角空白 U+3000 に、`\d` が全角数字 ０ にマッチする（実測）。
//                 UCP 無しだと ASCII のみになり、「有限会社　蛇喰産業」（全角空白）の
//                 `\s?` 等で挙動がズレる。
#pragma once

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace re {

struct Match {
  std::size_t begin;  // バイト位置
  std::size_t end;
  std::string text;
};

class Regex {
 public:
  explicit Regex(const std::string& pattern) : pattern_(pattern) {
    int err = 0;
    PCRE2_SIZE off = 0;
    code_ = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.c_str()), pattern.size(),
                          PCRE2_UTF | PCRE2_UCP, &err, &off, nullptr);
    if (!code_) {
      PCRE2_UCHAR buf[256];
      pcre2_get_error_message(err, buf, sizeof(buf));
      throw std::runtime_error("pcre2_compile 失敗 @" + std::to_string(off) + ": " +
                               reinterpret_cast<char*>(buf) + "  /  " + pattern);
    }
  }
  ~Regex() {
    if (code_) pcre2_code_free(code_);
  }
  Regex(const Regex&) = delete;
  Regex& operator=(const Regex&) = delete;

  /// Python の re.finditer 相当（重複しない左から順のマッチ）。
  std::vector<Match> finditer(const std::string& subject) const {
    std::vector<Match> out;
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(code_, nullptr);
    PCRE2_SIZE start = 0;
    while (start <= subject.size()) {
      const int rc = pcre2_match(code_, reinterpret_cast<PCRE2_SPTR>(subject.c_str()),
                                 subject.size(), start, 0, md, nullptr);
      if (rc < 0) break;  // PCRE2_ERROR_NOMATCH 等
      const PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
      const std::size_t b = ov[0], e = ov[1];
      out.push_back({b, e, subject.substr(b, e - b)});
      // 空マッチで無限ループしないように1つ進める
      start = (e > b) ? e : e + 1;
    }
    pcre2_match_data_free(md);
    return out;
  }

 private:
  std::string pattern_;
  pcre2_code* code_ = nullptr;
};

}  // namespace re
