// .msg 抽出。Aspose.Email FOSS(vendoring+patch) を使う。
//
// **完成条件は NER 候補の一致**（byte一致ではない）。extract_msg の m.to/m.cc は独自ロジックで
// 再構成され生プロパティと一致しないため（08は email のみ・01は名前付き）、byte 再現は脆い。
// msg-sample の目的は本文以外(ヘッダ・添付)の PII 網羅なので、候補が一致すれば十分。
//
// Aspose の patch: any_to_string が PT_BINARY(HTML本文 0x1013) で空を返すバグを修正済み
// （third_party/aspose-email/src/msg/mapi_message.cpp・実物の .msg で html_body() が取れる）。
#pragma once

#include <any>
#include <string>
#include <vector>

#include "aspose/email/foss/msg/mapi_message.hpp"
#include "extractors.hpp"  // strip_ruby / dewrap_prose
#include "numparse.hpp"
#include "re.hpp"
#include "utf8.hpp"

namespace msg {

namespace ae = aspose::email::foss::msg;

// トランスポートヘッダから拾うキー（_HEADER_KEYS）。Received/DKIM 等の配管は採らない。
inline const std::vector<std::string> HEADER_KEYS = {
    "From", "To", "Cc", "Bcc", "Reply-To", "Sender", "Return-Path"};

inline std::string prop_string(const ae::mapi_message& m, std::uint16_t id) {
  const std::any* a = m.get_property_value(id);
  if (!a) return {};
  if (const auto* s = std::any_cast<std::string>(a)) return *s;
  if (const auto* b = std::any_cast<std::vector<std::uint8_t>>(a))
    return std::string(b->begin(), b->end());
  return {};
}

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

/// RFC2047（`=?utf-8?B?...?=`）を復号（_decode_mime_header 相当）。charset は UTF-8 前提。
inline std::string decode_mime_header(const std::string& value) {
  static const re::Regex enc{R"(=\?[^?]+\?[BbQq]\?[^?]*\?=)"};
  const auto matches = enc.finditer(value);
  if (matches.empty()) return value;
  std::string out;
  std::size_t prev = 0;
  for (const auto& m : matches) {
    out += value.substr(prev, m.begin - prev);
    // =?charset?enc?text?=
    const std::string tok = m.text;
    const auto p1 = tok.find('?', 2);
    const auto p2 = tok.find('?', p1 + 1);
    const auto p3 = tok.find('?', p2 + 1);
    const char e = tok[p1 + 1];
    const std::string body = tok.substr(p2 + 1, p3 - p2 - 1);
    out += (e == 'B' || e == 'b') ? b64_decode(body) : q_decode(body);
    prev = m.end;
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

struct Child {
  std::string filename;
  std::vector<std::uint8_t> data;
};

struct Result {
  std::string text;
  std::string skipped_reason;
  std::vector<Child> children;  // 添付（filename, data）。再帰展開は呼び出し側で
};

/// .msg を抽出（extract_msg 相当）。件名/差出人/宛先/Cc/受信者/ヘッダ/本文＋添付。
inline Result extract_msg(const std::string& path) {
  auto m = ae::mapi_message::from_file(path);
  std::vector<std::string> parts;

  // 件名・差出人・宛先・Cc
  if (!m.subject().empty()) parts.push_back("件名: " + m.subject());
  const std::string sname = m.sender_name();
  const std::string semail = m.sender_email_address();
  std::string sender = sname;
  if (!semail.empty()) sender = (sname.empty() ? "" : sname + " ") + "<" + semail + ">";
  if (!sender.empty()) parts.push_back("差出人: " + sender);
  // 宛先/Cc は DisplayTo/DisplayCc（NER候補で判定するので生プロパティで十分）
  const std::string to = prop_string(m, 0x0E04);
  if (!to.empty()) parts.push_back("宛先: " + to);
  const std::string cc = prop_string(m, 0x0E03);
  if (!cc.empty()) parts.push_back("Cc: " + cc);

  // 受信者ストレージ
  for (const auto& r : m.recipients()) {
    const std::string nm = utf8::trim(r.display_name);
    const std::string ad = utf8::trim(r.email_address);
    if (!nm.empty() || !ad.empty())
      parts.push_back(utf8::trim("受信者: " + nm + " <" + ad + ">"));
  }

  // トランスポートヘッダ（【ヘッダ】ブロック）
  const std::string raw_hdr = prop_string(m, 0x007D);
  if (!raw_hdr.empty()) {
    const auto hdrs = parse_headers(raw_hdr);
    std::vector<std::string> hdr_lines;
    for (const auto& key : HEADER_KEYS)
      for (const auto& [k, v] : hdrs)
        if (k == key && !utf8::trim(v).empty())
          hdr_lines.push_back(k + ": " + decode_mime_header(v));
    for (const auto& [k, v] : hdrs)
      if (istarts_with(k, "x-") && !utf8::trim(v).empty())
        hdr_lines.push_back(k + ": " + decode_mime_header(v));
    if (!hdr_lines.empty()) {
      std::string block = "【ヘッダ】";
      for (const auto& l : hdr_lines) block += "\n" + l;
      parts.push_back(block);
    }
  }

  // 本文: 平文優先、無ければ HTML→text
  std::string body = extractors::dewrap_prose(m.body());
  if (utf8::trim(body).empty() && !m.html_body().empty()) body = html_to_text(m.html_body());

  std::string header;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) header += "\n";
    header += parts[i];
  }
  Result res;
  res.text = utf8::trim(header + "\n\n" + body);
  if (utf8::trim(body).empty())
    res.skipped_reason = "本文を抽出できない（平文・HTML本文とも空。RTF圧縮本文のみの可能性）";

  // 添付（filename, data）。ネスト .msg（embedded_message）は呼び出し側で再帰。
  for (auto& att : m.attachments()) {
    if (!att.filename.empty() && !att.data.empty())
      res.children.push_back({att.filename, att.data});
  }
  return res;
}

/// 添付ファイル名だけを取り出す（**中身は展開しない**）。
///
/// 添付を自動展開しないのは意図的な設計判断（docs/cli.md）。展開には一時ファイルが要り
/// （miniz も PDFium もパスを要求する）、メールという外部由来の入力を自動でネイティブ
/// パーサに食わせることになる。SECURITY.md が挙げる「細工した入力から到達可能な
/// パーサのメモリ安全性」の面を、わざわざ広げないための判断。
/// 添付を処理したい利用者は、自分で保存して個別に読み込む。
inline std::vector<std::string> attachment_names(const Result& r) {
  std::vector<std::string> out;
  for (const auto& c : r.children)
    if (!c.filename.empty()) out.push_back(c.filename);
  return out;
}

/// 本文の末尾に添付ファイル名を並べたもの。
/// **ファイル名自体が PII を持つ**（社員名簿_山田太郎_確認用.csv）ので検知対象に載せる。
inline std::string body_with_attachment_names(const Result& r) {
  std::string text = r.text;
  for (const auto& n : attachment_names(r)) text += "\n【添付】" + n;
  return text;
}

}  // namespace msg
