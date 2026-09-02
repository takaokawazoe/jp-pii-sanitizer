// .eml（RFC 5322 / MIME）抽出。**Aspose は使わない**——.eml はテキストなので自前で読める。
//
// 方針は .msg と揃えてある:
//   - 出力は「件名/差出人/宛先/Cc ＋【ヘッダ】ブロック ＋ 本文」。
//   - **添付は中身を展開しない。** ファイル名だけを拾う（設計判断・docs/cli.md）。
//     添付を処理したい利用者は自分で保存して個別に読み込む。
//   - 読めなかったときは黙って空を返さず error に理由を書く。空の本文が
//     「PII が無かった」に見えるのが一番まずい。
//
// 共通の MIME 部品（RFC 2047 の復号・base64・ヘッダ分解・HTML→テキスト）は mime.hpp。
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include "encoding.hpp"
#include "extractors.hpp"  // dewrap_prose / read_file
#include "mime.hpp"
#include "utf8.hpp"

namespace eml {

/// 抽出結果。children を持たないのは、添付の中身を扱わないため。
struct Result {
  std::string text;
  std::vector<std::string> attachments;  // 添付ファイル名のみ
  std::string error;                     // 空でなければ読めなかった理由
};

namespace detail {

constexpr int MAX_DEPTH = 20;      // multipart の入れ子の上限（暴走よけ）
constexpr int MAX_PARTS = 512;     // パート数の上限（同上）

/// ヘッダ部と本文部を最初の空行で分ける。CRLF と LF の両方を受ける。
inline void split_headers_body(const std::string& raw, std::string& hdr, std::string& body) {
  const auto p_crlf = raw.find("\r\n\r\n");
  const auto p_lf = raw.find("\n\n");
  std::size_t cut = std::string::npos, skip = 0;
  if (p_crlf != std::string::npos && (p_lf == std::string::npos || p_crlf <= p_lf)) {
    cut = p_crlf;
    skip = 4;
  } else if (p_lf != std::string::npos) {
    cut = p_lf;
    skip = 2;
  }
  if (cut == std::string::npos) {  // 空行が無い＝全部ヘッダ（本文なし）
    hdr = raw;
    body.clear();
    return;
  }
  hdr = raw.substr(0, cut);
  body = raw.substr(cut + skip);
}

inline std::string to_lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

/// ヘッダ値の本体（";" より前）を小文字・trim して返す。
inline std::string value_main(const std::string& v) {
  const auto semi = v.find(';');
  return to_lower(utf8::trim(semi == std::string::npos ? v : v.substr(0, semi)));
}

/// ヘッダのパラメータを取り出す（charset / boundary / filename / name）。
///
/// RFC 2231 の 2 形式に対応する:
///   - `filename*=utf-8''%E6%97%A5.txt`（言語・charset 付き）
///   - `filename*0=...; filename*1=...`（分割）
/// 素の `filename="..."` も含め、日本語の添付ファイル名はこのどれかで来る。
inline std::string header_param(const std::string& value, const std::string& name) {
  // ";" で分割（引用符の中の ";" は区切りにしない）
  std::vector<std::string> segs;
  std::string cur;
  bool in_quote = false;
  for (char c : value) {
    if (c == '"') in_quote = !in_quote;
    if (c == ';' && !in_quote) {
      segs.push_back(cur);
      cur.clear();
      continue;
    }
    cur += c;
  }
  segs.push_back(cur);

  const std::string lname = to_lower(name);
  std::string plain, extended;
  std::vector<std::pair<int, std::string>> chunks;  // filename*N の断片
  bool chunks_extended = false;

  for (std::size_t i = 1; i < segs.size(); ++i) {  // segs[0] は本体（text/plain 等）
    const auto eq = segs[i].find('=');
    if (eq == std::string::npos) continue;
    std::string key = to_lower(utf8::trim(segs[i].substr(0, eq)));
    std::string val = utf8::trim(segs[i].substr(eq + 1));
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
    if (key == lname) {
      plain = val;
    } else if (key == lname + "*") {
      extended = val;
    } else if (key.rfind(lname + "*", 0) == 0) {
      std::string idx = key.substr(lname.size() + 1);
      bool ext = false;
      if (!idx.empty() && idx.back() == '*') {  // filename*0*=...
        ext = true;
        idx.pop_back();
      }
      bool digits = !idx.empty();
      for (char c : idx)
        if (!std::isdigit(static_cast<unsigned char>(c))) digits = false;
      if (digits) {
        chunks.emplace_back(std::atoi(idx.c_str()), val);
        if (ext) chunks_extended = true;
      }
    }
  }

  // RFC 2231 の拡張値: charset'language'percent-encoded
  auto decode_extended = [](const std::string& v) -> std::string {
    const auto q1 = v.find('\'');
    const auto q2 = (q1 == std::string::npos) ? std::string::npos : v.find('\'', q1 + 1);
    std::string charset, rest;
    if (q2 != std::string::npos) {
      charset = v.substr(0, q1);
      rest = v.substr(q2 + 1);
    } else {
      rest = v;
    }
    std::string raw;
    for (std::size_t i = 0; i < rest.size(); ++i) {
      if (rest[i] == '%' && i + 2 < rest.size() && mime::is_hex_digit(rest[i + 1]) &&
          mime::is_hex_digit(rest[i + 2])) {
        raw += static_cast<char>(numparse::to_int_hex(rest.substr(i + 1, 2), 0));
        i += 2;
      } else {
        raw += rest[i];
      }
    }
    std::string conv;
    return encoding::charset_to_utf8(charset, raw, conv) ? conv : raw;
  };

  if (!chunks.empty()) {
    std::sort(chunks.begin(), chunks.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string joined;
    for (const auto& [n, v] : chunks) joined += v;
    return chunks_extended ? decode_extended(joined) : mime::decode_mime_header(joined);
  }
  if (!extended.empty()) return decode_extended(extended);
  // 素の値にも encoded-word が来ることがある（規格違反だが実在する）
  return mime::decode_mime_header(plain);
}

/// **本文用**の quoted-printable 復号（RFC 2045 §6.7）。
///
/// mime.hpp の q_decode は encoded-word 用で、`_` を空白に変え、ソフト改行を知らない。
/// 本文にそれを使うと下線が消え、行末の `=` が残って語が繋がらない。**別物として書く。**
inline std::string qp_decode_body(const std::string& in) {
  std::string out;
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '=') {
      out += in[i];
      continue;
    }
    // ソフト改行（`=` + 改行）は行の継続を意味するので、丸ごと落とす
    if (i + 1 < in.size() && in[i + 1] == '\n') {
      i += 1;
      continue;
    }
    if (i + 2 < in.size() && in[i + 1] == '\r' && in[i + 2] == '\n') {
      i += 2;
      continue;
    }
    if (i + 2 < in.size() && mime::is_hex_digit(in[i + 1]) && mime::is_hex_digit(in[i + 2])) {
      out += static_cast<char>(numparse::to_int_hex(in.substr(i + 1, 2), 0));
      i += 2;
      continue;
    }
    out += in[i];  // 壊れた "=" は quopri と同じくそのまま残す
  }
  return out;
}

/// MIME の 1 パート。添付は body を持たない（中身を展開しないため）。
struct Part {
  std::string content_type = "text/plain";
  std::string charset;
  std::string filename;
  bool is_attachment = false;
  bool charset_failed = false;  // 本文はあったが文字コードを解釈できなかった
  std::string body;             // UTF-8。添付・multipart では空
  std::vector<Part> children;
};

/// multipart の本文を境界で分割する。
inline std::vector<std::string> split_multipart(const std::string& body,
                                                const std::string& boundary) {
  std::vector<std::string> parts;
  const std::string delim = "--" + boundary;
  std::size_t pos = 0;
  bool started = false;
  std::size_t part_start = 0;
  while (pos <= body.size()) {
    const auto hit = body.find(delim, pos);
    if (hit == std::string::npos) break;
    // 境界は行頭にしか現れない（本文中の偶然の一致を拾わないため）
    if (hit != 0 && body[hit - 1] != '\n') {
      pos = hit + delim.size();
      continue;
    }
    if (started) {
      std::size_t end = hit;
      // 直前の改行は境界の一部
      if (end > 0 && body[end - 1] == '\n') --end;
      if (end > 0 && body[end - 1] == '\r') --end;
      parts.push_back(body.substr(part_start, end - part_start));
    }
    const std::size_t after = hit + delim.size();
    if (after + 1 < body.size() && body[after] == '-' && body[after + 1] == '-') break;  // 終端
    // 境界行の残り（改行まで）を読み飛ばす
    const auto nl = body.find('\n', after);
    if (nl == std::string::npos) break;
    part_start = nl + 1;
    started = true;
    pos = part_start;
  }
  return parts;
}

inline Part parse_part(const std::string& raw, int depth, int& budget) {
  Part p;
  if (depth > MAX_DEPTH || --budget < 0) {
    p.content_type = "application/octet-stream";
    return p;
  }
  std::string hdr_raw, body;
  split_headers_body(raw, hdr_raw, body);
  const auto hdrs = mime::parse_headers(hdr_raw);

  std::string ct, cte, cd;
  for (const auto& [k, v] : hdrs) {
    const std::string lk = to_lower(k);
    if (lk == "content-type" && ct.empty()) ct = v;
    else if (lk == "content-transfer-encoding" && cte.empty()) cte = v;
    else if (lk == "content-disposition" && cd.empty()) cd = v;
  }
  if (!ct.empty()) p.content_type = value_main(ct);
  p.charset = header_param(ct, "charset");
  p.filename = header_param(cd, "filename");
  if (p.filename.empty()) p.filename = header_param(ct, "name");
  const std::string disp = value_main(cd);

  if (p.content_type.rfind("multipart/", 0) == 0) {
    const std::string boundary = header_param(ct, "boundary");
    if (boundary.empty()) return p;  // 境界が無い multipart は読めない
    for (const auto& sub : split_multipart(body, boundary))
      p.children.push_back(parse_part(sub, depth + 1, budget));
    return p;
  }

  // 添付の判定: 明示の attachment、または text/* でないのにファイル名が付いているもの。
  // **中身は復号しない**（保持もしない）。ファイル名だけ持ち帰る。
  const bool is_text = p.content_type.rfind("text/", 0) == 0;
  if (disp == "attachment" || (!is_text && !p.filename.empty()) ||
      p.content_type == "message/rfc822") {
    p.is_attachment = true;
    return p;
  }
  if (!is_text) return p;  // text/* 以外（画像等）は本文にしない

  const std::string enc = to_lower(utf8::trim(cte));
  std::string decoded;
  if (enc == "base64") decoded = mime::b64_decode(body);
  else if (enc == "quoted-printable") decoded = qp_decode_body(body);
  else decoded = body;  // 7bit / 8bit / binary / 未指定

  // CRLF を LF に揃える。メールの行末は CRLF なので、そのままだと本文中に \r が残り、
  // 下流（dewrap_prose の空行判定・ハイライトの文字位置）が狂う。
  auto to_lf = [](std::string s) {
    std::string o;
    o.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
      o += s[i];
    }
    return o;
  };
  decoded = to_lf(decoded);

  std::string conv;
  if (encoding::charset_to_utf8(p.charset, decoded, conv)) {
    p.body = conv;
  } else {
    // **黙って化けさせない。** 読めなかったことを上まで伝える。
    p.charset_failed = !utf8::trim(decoded).empty();
    p.body.clear();
  }
  return p;
}

/// 木から本文を選ぶ。平文優先、無ければ HTML（Python の get_body と同じ優先順）。
inline const Part* pick_body(const Part& p, const std::string& want) {
  if (!p.is_attachment && p.content_type == want && !utf8::trim(p.body).empty()) return &p;
  for (const auto& c : p.children)
    if (const Part* r = pick_body(c, want)) return r;
  return nullptr;
}

inline bool any_charset_failure(const Part& p) {
  if (p.charset_failed) return true;
  for (const auto& c : p.children)
    if (any_charset_failure(c)) return true;
  return false;
}

inline void collect_attachments(const Part& p, std::vector<std::string>& out) {
  if (p.is_attachment)
    out.push_back(p.filename.empty() ? "(名前のない添付)" : p.filename);
  for (const auto& c : p.children) collect_attachments(c, out);
}

}  // namespace detail

/// .eml を抽出する。件名/差出人/宛先/Cc ＋【ヘッダ】ブロック ＋ 本文。
inline Result extract_eml(const std::string& path) {
  Result res;
  const std::string raw = extractors::read_file(path);
  if (utf8::trim(raw).empty()) {
    res.error = "ファイルが空です";
    return res;
  }

  std::string hdr_raw, dummy;
  detail::split_headers_body(raw, hdr_raw, dummy);
  const auto hdrs = mime::parse_headers(hdr_raw);
  auto find_hdr = [&](const char* name) -> std::string {
    for (const auto& [k, v] : hdrs)
      if (detail::to_lower(k) == name) return utf8::trim(v);
    return {};
  };

  std::vector<std::string> parts;
  const std::string subject = find_hdr("subject");
  if (!subject.empty()) parts.push_back("件名: " + mime::decode_mime_header(subject));
  const std::string from = find_hdr("from");
  if (!from.empty()) parts.push_back("差出人: " + mime::decode_mime_header(from));
  const std::string to = find_hdr("to");
  if (!to.empty()) parts.push_back("宛先: " + mime::decode_mime_header(to));
  const std::string cc = find_hdr("cc");
  if (!cc.empty()) parts.push_back("Cc: " + mime::decode_mime_header(cc));

  // 【ヘッダ】ブロック。上で出した From/To/Cc は繰り返さない（AI に送る文字数が増えるだけ）。
  // 残りの HEADER_KEYS と X- ヘッダは差出人の情報を持つので拾う。
  {
    std::vector<std::string> hdr_lines;
    // 上で出した From/To/Cc は繰り返さない。残りの HEADER_KEYS を拾う。
    for (const auto& key : mime::HEADER_KEYS) {
      const std::string lk = detail::to_lower(key);
      if (lk == "from" || lk == "to" || lk == "cc") continue;
      for (const auto& [k, v] : hdrs)
        if (detail::to_lower(k) == detail::to_lower(key) && !utf8::trim(v).empty())
          hdr_lines.push_back(k + ": " + mime::decode_mime_header(v));
    }
    // **`X-` ヘッダは採らない**（理由は msg.hpp の同じ箇所。`.eml` も同じ配管が入る）。
    if (!hdr_lines.empty()) {
      std::string block = "【ヘッダ】";
      for (const auto& l : hdr_lines) block += "\n" + l;
      parts.push_back(block);
    }
  }

  int budget = detail::MAX_PARTS;
  const detail::Part root = detail::parse_part(raw, 0, budget);

  std::string body;
  if (const detail::Part* plain = detail::pick_body(root, "text/plain")) {
    body = extractors::dewrap_prose(plain->body);
  } else if (const detail::Part* html = detail::pick_body(root, "text/html")) {
    body = mime::html_to_text(html->body);
  }

  detail::collect_attachments(root, res.attachments);

  std::string header;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) header += "\n";
    header += parts[i];
  }
  res.text = utf8::trim(header + "\n\n" + body);

  if (utf8::trim(body).empty()) {
    // **空を黙って返さない。** 理由で分ける: 文字コードが読めなかったのか、本文が無いのか。
    res.error = detail::any_charset_failure(root)
                    ? "本文の文字コードを解釈できません（対応: UTF-8 / ISO-2022-JP / "
                      "Shift_JIS / EUC-JP）"
                    : "本文を抽出できません（text/plain・text/html とも空）";
  }
  return res;
}

/// 添付ファイル名を本文の末尾に足す（.msg と同じ形）。ファイル名自体が PII を持つので
/// 検知対象には載せる。**中身は開かない。**
inline std::string body_with_attachment_names(const Result& r) {
  if (r.attachments.empty()) return r.text;
  std::string out = r.text + "\n\n【添付ファイル】";
  for (const auto& n : r.attachments) out += "\n" + n;
  return out;
}

}  // namespace eml
