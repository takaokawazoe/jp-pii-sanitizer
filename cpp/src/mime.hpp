// MIME の共通部品（RFC 2047 ヘッダ復号 / base64 / quoted-printable / ヘッダ分解 /
// HTML→テキスト）。**外部ライブラリに依存しない。**
//
// **なぜ msg.hpp から分けたか**: これらは元々 msg.hpp にあったが、あのヘッダは冒頭で
// Aspose.Email を include する。GUI 側は Aspose を app/msg_bridge.cpp の 1 TU に閉じ込めて
// あり（/bigobj もそこだけ）、.eml のために msg.hpp を core.hpp から include すると
// その隔離が崩れる。.eml は RFC 5322 のテキストなので Aspose は要らない。
//
// msg.hpp は using 宣言でこれらを再公開しているので、msg::b64_decode 等の既存の
// 呼び出しはそのまま動く。
#pragma once

#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "encoding.hpp"
#include "numparse.hpp"
#include "re.hpp"
#include "utf8.hpp"

namespace mime {

// ヘッダから拾うキー。Received/DKIM 等の配管は採らない（PII が無く、量だけ増える）。
inline const std::vector<std::string> HEADER_KEYS = {
    "From", "To", "Cc", "Bcc", "Reply-To", "Sender", "Return-Path"};
// ---- RFC2047 デコード（=?charset?B/Q?text?=） ----

inline std::string b64_decode(const std::string& in) {
  static const std::string T =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int val = 0, bits = -8;
  for (unsigned char c : in) {
    if (c == '=') break;
    const auto pos = T.find(c);
    if (pos == std::string::npos) continue;
    val = (val << 6) + static_cast<int>(pos);
    bits += 6;
    if (bits >= 0) {
      out += static_cast<char>((val >> bits) & 0xFF);
      bits -= 8;
    }
  }
  return out;
}

inline bool is_hex_digit(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return (u >= '0' && u <= '9') || (u >= 'A' && u <= 'F') || (u >= 'a' && u <= 'f');
}

inline std::string q_decode(const std::string& in) {
  std::string out;
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '_') {
      out += ' ';
    } else if (in[i] == '=' && i + 2 < in.size() && is_hex_digit(in[i + 1]) &&
               is_hex_digit(in[i + 2])) {
      // 16進2桁であることを確かめてから変換する。壊れたヘッダの "=ZZ" で std::stoi が
      // invalid_argument を投げると、上位に受け皿が無い経路ではプロセスごと落ちる。
      out += static_cast<char>(numparse::to_int_hex(in.substr(i + 1, 2), 0));
      i += 2;
    } else {
      out += in[i];  // 不正な "=" は quopri と同じくそのまま残す
    }
  }
  return out;
}

/// RFC 2047 の encoded-word（`=?charset?B/Q?text?=`）を復号し UTF-8 に揃える。
///
/// **charset を実際に使う。** 以前はここで charset を読み飛ばして UTF-8 を前提にしていたが、
/// 日本語のメールの件名・差出人は `=?ISO-2022-JP?B?...?=` が普通に来る。復号したバイト列を
/// そのまま返すと出口の utf8::repair で U+FFFD に潰れ、件名と差出人が丸ごと消える
/// （＝そこにある PII を一つも検知できない）。手元のフィクスチャが全部 UTF-8 だったので
/// 露見していなかった。.eml だけでなく .msg のヘッダにも効く。
///
/// 変換できない charset は復号したバイト列をそのまま返す（従来と同じ）。読めない件名の
/// ために本文まで捨てるのは割に合わない。
inline std::string decode_mime_header(const std::string& value) {
  static const re::Regex enc{R"(=\?[^?]+\?[BbQq]\?[^?]*\?=)"};
  const auto matches = enc.finditer(value);
  if (matches.empty()) return value;
  std::string out;
  std::size_t prev = 0;
  bool prev_was_encoded = false;
  for (const auto& m : matches) {
    const std::string gap = value.substr(prev, m.begin - prev);
    // **隣り合う encoded-word の間の空白は捨てる**（RFC 2047 §6.2）。長い日本語の件名は
    // 76 バイト制限で複数の encoded-word に折り返されるので、残すと語の途中に空白が入る。
    const bool gap_is_ws =
        !gap.empty() && gap.find_first_not_of(" \t\r\n") == std::string::npos;
    if (!(prev_was_encoded && gap_is_ws)) out += gap;

    const std::string tok = m.text;
    const auto p1 = tok.find('?', 2);
    const auto p2 = tok.find('?', p1 + 1);
    const auto p3 = tok.find('?', p2 + 1);
    const char e = tok[p1 + 1];
    std::string charset = tok.substr(2, p1 - 2);
    // RFC 2231 の言語タグ（`=?utf-8*ja?B?...?=`）は charset 名から落とす。
    const auto star = charset.find('*');
    if (star != std::string::npos) charset.erase(star);
    const std::string body = tok.substr(p2 + 1, p3 - p2 - 1);
    const std::string raw = (e == 'B' || e == 'b') ? b64_decode(body) : q_decode(body);
    std::string conv;
    out += encoding::charset_to_utf8(charset, raw, conv) ? conv : raw;

    prev = m.end;
    prev_was_encoded = true;
  }
  out += value.substr(prev);
  return out;
}

/// transport headers 文字列を (key, value) に分解（RFC822・折り返し行を継続）。
inline std::vector<std::pair<std::string, std::string>> parse_headers(const std::string& raw) {
  std::vector<std::pair<std::string, std::string>> out;
  std::string line;
  auto flush = [&](const std::string& l) {
    if (l.empty()) return;
    if ((l[0] == ' ' || l[0] == '\t') && !out.empty()) {
      out.back().second += l;  // 折り返し（継続行）
      return;
    }
    const auto colon = l.find(':');
    if (colon == std::string::npos) return;
    std::string key = l.substr(0, colon);
    std::string val = l.substr(colon + 1);
    if (!val.empty() && val[0] == ' ') val.erase(0, 1);
    out.emplace_back(key, val);
  };
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\r') continue;
    if (raw[i] == '\n') {
      flush(line);
      line.clear();
    } else {
      line += raw[i];
    }
  }
  flush(line);
  return out;
}

inline bool istarts_with(const std::string& s, const std::string& p) {
  if (s.size() < p.size()) return false;
  for (std::size_t i = 0; i < p.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(s[i])) !=
        std::tolower(static_cast<unsigned char>(p[i])))
      return false;
  return true;
}

// ---- HTML → テキスト（_html_to_text 相当・bs4 の近似） ----
// bs4 の get_text("\n") をタグ剥がしで近似する。byte一致は狙わない（NER候補で判定）。
inline std::string html_to_text(const std::string& html) {
  // script/style/head を中身ごと除去
  std::string h = html;
  for (const char* tag : {"script", "style", "head"}) {
    const re::Regex block{std::string("(?is)<") + tag + R"(\b[^>]*>.*?</)" + tag + ">"};
    std::string tmp;
    std::size_t prev = 0;
    for (const auto& m : block.finditer(h)) {
      tmp += h.substr(prev, m.begin - prev);
      prev = m.end;
    }
    tmp += h.substr(prev);
    h = tmp;
  }
  // タグを改行に、テキストノードを残す
  static const re::Regex tag{R"(<[^>]+>)"};
  std::string txt;
  std::size_t prev = 0;
  for (const auto& m : tag.finditer(h)) {
    txt += h.substr(prev, m.begin - prev);
    txt += "\n";
    prev = m.end;
  }
  txt += h.substr(prev);
  // HTML 実体参照の最小デコード
  static const std::pair<const char*, const char*> ents[] = {
      {"&nbsp;", " "}, {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""},
      {"&#39;", "'"}, {"&apos;", "'"}};
  for (const auto& [from, to] : ents) {
    std::string tmp;
    std::size_t p = 0, q;
    const std::string f = from;
    while ((q = txt.find(f, p)) != std::string::npos) {
      tmp += txt.substr(p, q - p);
      tmp += to;
      p = q + f.size();
    }
    tmp += txt.substr(p);
    txt = tmp;
  }
  // 行ごとに strip、空行除去、3連続改行→2（bs4 経路の後処理を近似）
  std::vector<std::string> lines;
  for (const auto& ln : utf8::splitlines_keepends(txt)) {
    std::string s = ln;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    s = utf8::trim(s);
    if (!s.empty()) lines.push_back(s);
  }
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i) out += "\n";
    out += lines[i];
  }
  return utf8::trim(out);
}

}  // namespace mime
