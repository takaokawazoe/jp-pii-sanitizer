// extractors.py の移植。入口＝形式別テキスト化。
//
// このヘッダは平文(csv/txt)と OOXML(docx/pptx/xlsx) を扱う。
// pdf=PDFium・msg=Aspose は別スパイク済みで、繋ぎ込みは Phase 4b/4c で行う。
#pragma once

#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "file_io.hpp"  // UTF-8 パスでのファイル読み（MSVC の cp932 パス問題を吸収）
#include "re.hpp"
#include "utf8.hpp"  // trim（Python の str.strip 相当）

namespace extractors {

// 難読漢字のルビ注記「漢字(かな)」を除去（_strip_ruby / _RUBY_RE の移植）。
// 漢字直後の半/全角括弧内がかな1〜10字のものだけ対象。一般の括弧注記は残す。
inline std::string strip_ruby(const std::string& text) {
  static const re::Regex ruby{
      R"((?<=[一-龥々〆ヶ])[（(][ぁ-んァ-ヶーゝゞ・]{1,10}[）)])"};
  std::string out;
  std::size_t prev = 0;
  for (const auto& m : ruby.finditer(text)) {
    out += text.substr(prev, m.begin - prev);
    prev = m.end;
  }
  out += text.substr(prev);
  return out;
}

/// 平文バイト列を復号する（_decode_text_bytes の移植）。
///
/// **現状は UTF-8（BOM 除去つき）のみ完全対応。** Python 版は BOM付きUTF-8 → cp932 →
/// UTF-8(置換) の順に試すが、cp932 の完全なデコード表は重く、実運用サンプル（添付CSV/TXT）は
/// すべて UTF-8 だった（実測）。cp932 の .txt/.csv が来たら文字化けするので、Phase 4 完了前に
/// Windows 由来ファイルで要検証（リスク項目）。
inline std::string decode_text_bytes(const std::string& data) {
  // UTF-8 BOM を除去（utf-8-sig 相当）
  if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
      static_cast<unsigned char>(data[1]) == 0xBB && static_cast<unsigned char>(data[2]) == 0xBF) {
    return data.substr(3);
  }
  return data;
}

inline std::string read_file(const std::string& path) {
  return fileio::read_all(path);
}

/// .txt / .csv をテキスト化する（extract_plain の移植）。
inline std::string extract_plain(const std::string& path) {
  return strip_ruby(decode_text_bytes(read_file(path)));
}

/// 散文の行末折り返し（語中改行）を詰める（dewrap_prose の移植）。
///
/// 空行（`\n[ \t]*\n`）で区切り、「。」を含むブロックだけ単一改行を除去する。
/// ヘッダ・表・箇条書き（「。」を含まないブロック）はそのまま保つ。メール本文向け。
/// Python: "".join(p.replace("\n","") if "。" in p else p for p in re.split(r"(\n[ \t]*\n)", text))
inline std::string dewrap_prose(const std::string& text) {
  static const re::Regex sep{R"(\n[ \t]*\n)"};  // 区切り（キャプチャ相当＝区切りも残す）
  const std::string maru = "。";  // U+3002
  std::string out;
  std::size_t prev = 0;
  auto emit = [&](const std::string& block, bool is_sep) {
    if (is_sep) {
      out += block;  // 区切りはそのまま
    } else if (block.find(maru) != std::string::npos) {
      for (char c : block)
        if (c != '\n') out += c;  // 「。」を含むブロックは改行を全除去
    } else {
      out += block;
    }
  };
  for (const auto& m : sep.finditer(text)) {
    emit(text.substr(prev, m.begin - prev), false);
    emit(text.substr(m.begin, m.end - m.begin), true);
    prev = m.end;
  }
  emit(text.substr(prev), false);
  return out;
}

// ---- CSV ----

struct Table {
  std::vector<std::string> headers;
  std::vector<std::vector<std::string>> rows;
};

/// RFC 4180 準拠の CSV パーサ（Python csv.reader 相当）。
///
/// 区切り文字は Python 版が csv.Sniffer で判定するが、実運用サンプルは全て `,` だった。
/// ここでは `,` 固定＋引用符（"..."／埋め込み改行／"" エスケープ）に対応する。
/// Sniffer 相当（; \t | の自動判定）は必要になってから足す。
inline std::vector<std::vector<std::string>> parse_csv(const std::string& text,
                                                       char delim = ',') {
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> row;
  std::string field;
  bool in_quotes = false;
  bool row_started = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (in_quotes) {
      if (c == '"') {
        if (i + 1 < text.size() && text[i + 1] == '"') {  // "" → " のエスケープ
          field += '"';
          ++i;
        } else {
          in_quotes = false;
        }
      } else {
        field += c;
      }
    } else if (c == '"') {
      in_quotes = true;
      row_started = true;
    } else if (c == delim) {
      row.push_back(field);
      field.clear();
      row_started = true;
    } else if (c == '\n' || c == '\r') {
      if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;  // \r\n
      if (row_started || !field.empty() || !row.empty()) {
        row.push_back(field);
        rows.push_back(row);
        row.clear();
        field.clear();
      }
      row_started = false;
    } else {
      field += c;
      row_started = true;
    }
  }
  if (row_started || !field.empty() || !row.empty()) {
    row.push_back(field);
    rows.push_back(row);
  }
  return rows;
}

inline std::vector<std::vector<std::string>> read_csv_rows(const std::string& path) {
  return parse_csv(decode_text_bytes(read_file(path)));
}

inline bool cell_nonempty(const std::string& s) { return !utf8::trim(s).empty(); }

/// 実データ表が始まる行を推定（_first_table_row の移植）。
inline std::size_t first_table_row(const std::vector<std::vector<std::string>>& rows) {
  std::vector<std::size_t> counts;
  for (const auto& row : rows) {
    std::size_t n = 0;
    for (const auto& c : row)
      if (cell_nonempty(c)) ++n;
    counts.push_back(n);
  }
  // 非空セル2つ以上の行のうち最頻の幅
  std::map<std::size_t, int> freq;
  for (auto c : counts)
    if (c >= 2) ++freq[c];
  if (freq.empty()) {
    for (std::size_t i = 0; i < counts.size(); ++i)
      if (counts[i] >= 1) return i;
    return 0;
  }
  // most_common(1): 最頻。同数なら最初に現れた幅（Counter の仕様）
  std::size_t width = 0;
  int best = -1;
  std::map<std::size_t, int> first_seen;
  int order = 0;
  for (auto c : counts)
    if (c >= 2 && !first_seen.count(c)) first_seen[c] = order++;
  for (const auto& [w, f] : freq) {
    if (f > best || (f == best && first_seen[w] < first_seen[width])) {
      best = f;
      width = w;
    }
  }
  for (std::size_t i = 0; i < counts.size(); ++i)
    if (counts[i] == width) return i;
  return 0;
}

/// 見出し行のインデックスを推定（_detect_header_row の移植）。
inline std::size_t detect_header_row(const std::vector<std::vector<std::string>>& rows,
                                     const std::vector<std::string>& wanted) {
  if (!wanted.empty()) {
    std::size_t best_i = 0;
    int best_score = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      std::set<std::string> cells;
      for (const auto& c : rows[i]) cells.insert(utf8::trim(c));
      int score = 0;
      for (const auto& w : wanted)
        if (cells.count(w)) ++score;
      if (score > best_score) {
        best_score = score;
        best_i = i;
      }
      if (score == static_cast<int>(wanted.size())) return i;
    }
    if (best_score > 0) return best_i;
  }
  return first_table_row(rows);
}

/// (見出し, 行) にする（_rows_to_table の移植）。
inline Table rows_to_table(const std::vector<std::vector<std::string>>& rows,
                           const std::vector<std::string>& wanted = {}) {
  Table t;
  if (rows.empty()) return t;
  const std::size_t hi = detect_header_row(rows, wanted);
  t.headers = rows[hi];
  for (std::size_t i = hi + 1; i < rows.size(); ++i) {
    bool any = false;
    for (const auto& c : rows[i])
      if (!c.empty()) {  // Python: any(c is not None ...)。CSVは常に文字列なので「空でない」
        any = true;
        break;
      }
    // 実際には Python は None 判定なので、空文字だけの行も残る。CSV は None にならないため
    // 「1セルでも存在する（=列がある）」で判定する。空行（列0）は parse_csv が出さない。
    if (!rows[i].empty()) t.rows.push_back(rows[i]);
    (void)any;
  }
  return t;
}

inline Table read_csv_table(const std::string& path, const std::vector<std::string>& wanted = {}) {
  return rows_to_table(read_csv_rows(path), wanted);
}

}  // namespace extractors
