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
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "aspose/email/foss/msg/mapi_message.hpp"
#include "extractors.hpp"  // strip_ruby / dewrap_prose
#include "file_io.hpp"
#include "mime.hpp"
#include "numparse.hpp"
#include "re.hpp"
#include "utf8.hpp"

namespace msg {

namespace ae = aspose::email::foss::msg;


/// MAPI から出てきた文字列を UTF-8 に揃える。
///
/// **なぜ要るか**: MAPI のプロパティには Unicode(PT_UNICODE) と非 Unicode(PT_STRING8) が
/// あり、Aspose の decode_string8 は**バイト列をそのまま返す**（コードページ変換をしない）。
/// 日本語 Windows の Outlook が非 Unicode で保存した .msg は本文も件名も cp932 のままで、
/// これが下流の PCRE2 に届くと
///   「pcre2 match 失敗: UTF-8 error: isolated byte with 0x80 bit set」
/// で抽出ごと失敗する（実測・利用者からの報告）。出口の utf8::repair より手前で
/// dewrap_prose が正規表現を掛けるので、**入口で直す必要がある**。
///
/// 判定は平文ファイルと同じ decode_text_bytes（UTF-8 として妥当ならそのまま／
/// 駄目なら cp932 ／どちらでもなければ U+FFFD で潰す）。既に UTF-8 の .msg は素通しなので、
/// Unicode で保存された通常の .msg の挙動は変わらない。
///
/// **限界**: コードページは cp932 決め打ちで、PidTagMessageCodepage を見ていない。
/// 日本語以外の非 Unicode .msg（キリル文字等）は cp932 として読めず U+FFFD になる。
/// 平文 CSV/TXT の入口と同じ割り切りで、必要になったら広げる。
inline std::string mapi_text(const std::string& s) {
  return extractors::decode_text_bytes(s);
}

inline std::string prop_string(const ae::mapi_message& m, std::uint16_t id) {
  const std::any* a = m.get_property_value(id);
  if (!a) return {};
  // 取り出した時点で UTF-8 に揃える（理由は mapi_text）。
  if (const auto* s = std::any_cast<std::string>(a)) return mapi_text(*s);
  if (const auto* b = std::any_cast<std::vector<std::uint8_t>>(a))
    return mapi_text(std::string(b->begin(), b->end()));
  return {};
}

// MIME の共通部品は mime.hpp（Aspose 非依存）へ移した。.eml も同じ部品を使うが、
// あちらは Aspose を必要としないため。ここでは既存の呼び出し（msg::b64_decode 等）を
// 壊さないよう再公開するだけで、実装は 1 箇所にしかない。
using mime::b64_decode;
using mime::HEADER_KEYS;
using mime::decode_mime_header;
using mime::html_to_text;
using mime::is_hex_digit;
using mime::istarts_with;
using mime::parse_headers;
using mime::q_decode;

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
  // **パスを Aspose に渡さない。** `mapi_message::from_file` は narrow の文字列から
  // `std::filesystem::path` を作るが、**MSVC は narrow をロケールの ANSI コードページ
  // （日本語 Windows では cp932）として解釈する**。こちらが持っているのは UTF-8 なので、
  // 日本語のファイル名・フォルダ名だと別のパスを開きにいって
  //   「Unable to open CFB file: ...」（UTF-8 バイト列がたまたま cp932 として妥当な場合）
  //   「No mapping for the Unicode character exists in the target multi-byte code page.」
  // のどちらかになる。**.msg は日本語名だと一度も開けていなかった。**
  //
  // 自前で（ワイド API で）読んでからバイト列を渡す。file_io.hpp を使うのは他の入口と同じ
  // 理由で、「narrow パスをそのままファイル API に渡さない」がこのコードベースの規則。
  // PDFium と miniz は UTF-8 を自前で widen するので同じ問題は起きない（実測）。
  const std::string bytes = fileio::read_all(path);
  if (bytes.empty()) throw std::runtime_error("開けないか空のファイルです: " + path);
  std::istringstream in(bytes, std::ios::binary);
  auto m = ae::mapi_message::from_stream(in);
  std::vector<std::string> parts;

  // 件名・差出人・宛先・Cc
  // Aspose から出る文字列は全て mapi_text を通す（非 Unicode の .msg は cp932 のまま来る）。
  const std::string subject = mapi_text(m.subject());
  if (!subject.empty()) parts.push_back("件名: " + subject);
  const std::string sname = mapi_text(m.sender_name());
  const std::string semail = mapi_text(m.sender_email_address());
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
    const std::string nm = utf8::trim(mapi_text(r.display_name));
    const std::string ad = utf8::trim(mapi_text(r.email_address));
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
  // **dewrap_prose より前に復号すること。** あちらは正規表現を掛けるので、cp932 の
  // バイト列が届くと PCRE2 が UTF-8 エラーを投げて抽出ごと落ちる（出口の utf8::repair では遅い）。
  std::string body = extractors::dewrap_prose(mapi_text(m.body()));
  const std::string html = mapi_text(m.html_body());
  if (utf8::trim(body).empty() && !html.empty()) body = html_to_text(html);

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
    // ファイル名も非 Unicode のことがある（添付名は候補に載るので化けさせない）。
    const std::string fn = mapi_text(att.filename);
    if (!fn.empty() && !att.data.empty())
      res.children.push_back({fn, att.data});
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
