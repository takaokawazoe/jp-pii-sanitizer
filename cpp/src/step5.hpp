// 束ね（step5_intake の bundle_blocks / tokenize_table）の移植（Phase 5 コア残り）。
//
// UI・CLI・build_case が共有する「案件単位の1本テキスト」を作る。データ表は列指定で
// トークン化し件数要約だけ本文へ（生データはAIに渡さない）。
#pragma once

#include <set>
#include <string>
#include <vector>

#include "num_patterns.hpp"
#include "re.hpp"
#include "tokenizer.hpp"

namespace step5 {

// データ表として扱う kind（extractors の TABLE_KINDS 相当）。
inline bool is_table_kind(const std::string& kind) {
  return kind == "xlsx-table" || kind == "csv-table";
}

/// 抽出結果1件（extractors.FileResult 相当）。UI/束ねが扱う最小構造。
struct FileResult {
  std::string source;  // ファイル名/シート名
  std::string kind;    // docx/pptx/pdf/text/eml/msg/xlsx/xlsx-prose/xlsx-table/csv-table/skipped
  std::string text;
  std::vector<std::string> headers;               // データ表のとき
  std::vector<std::vector<std::string>> rows;      // データ表のとき
  std::vector<std::string> name_cols, company_cols;
  bool has_table = false;
  // 添付ファイル名（.msg/.eml）。**中身は展開しない**——添付は利用者が自分で取り出して
  // 読み込む方針（docs 参照）。ここに載せるのは「添付があった」ことを警告するためと、
  // ファイル名自体が PII を持つため（社員名簿_山田太郎_確認用.csv のような例）。
  std::vector<std::string> attachments;
  // 抽出に失敗した理由（UI/CLI の表示専用）。**text に混ぜてはいけない**——text は
  // そのまま AI 送信テキストへ束ねられるので、例外メッセージが本文に紛れ込む。
  std::string error;
};

/// 束ねの先頭に付く【ファイル名】ブロックの本文（無ければ空）。
///
/// bundle_blocks から切り出してある。**ここに載る名前は検知の対象にしないと素通りする**
/// ので、UI/CLI 側が候補検知に掛けられるよう公開している（ファイル名にも氏名は普通に入る:
/// 例「社員名簿_山田太郎_確認用.csv」）。
inline std::vector<std::string> source_names(const std::vector<FileResult>& extracted) {
  std::set<std::string> nameset;  // 重複除去して昇順
  for (const auto& r : extracted)
    if (!r.source.empty() && r.kind != "xlsx") nameset.insert(r.source);
  return {nameset.begin(), nameset.end()};
}

inline std::string filenames_block(const std::vector<FileResult>& extracted) {
  const auto names = source_names(extracted);
  if (names.empty()) return {};
  std::string blk = "【ファイル名】";
  for (const auto& n : names) blk += "\n" + n;
  return blk;
}

/// データ表の指定列を丸ごとトークン化し件数要約を返す（tokenize_table の移植）。
///
/// **副作用**: 指定列のセル・全セルのメール/電話が tok の対応表に登録される。
inline std::string tokenize_table(tokenizer::Tokenizer& tok, const std::vector<std::string>& headers,
                                  const std::vector<std::vector<std::string>>& rows,
                                  const std::vector<std::string>& name_cols,
                                  const std::vector<std::string>& company_cols,
                                  sudachi::Analyzer* sd = nullptr) {
  std::map<int, std::string> col_type;
  for (std::size_t i = 0; i < headers.size(); ++i) {
    if (std::find(name_cols.begin(), name_cols.end(), headers[i]) != name_cols.end())
      col_type[static_cast<int>(i)] = "PERSON";
    else if (std::find(company_cols.begin(), company_cols.end(), headers[i]) != company_cols.end())
      col_type[static_cast<int>(i)] = "ORGANIZATION";
  }
  static const re::Regex email{R"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})"};
  static const re::Regex phone{numpat::PHONE};
  for (const auto& row : rows) {
    for (std::size_t i = 0; i < row.size(); ++i) {
      const std::string& cell = row[i];
      if (cell.empty()) continue;  // Python: if not cell（空文字はスキップ）
      const auto it = col_type.find(static_cast<int>(i));
      if (it != col_type.end()) {
        tok.tokenize(cell, {{cell, it->second}}, sd);  // 指定列＝確定リスト相当
      } else {
        // 非指定列: メール/電話を regex で自動トークン化（対応表に登録するだけ）
        for (const auto& m : email.finditer(cell)) tok.assign(m.text, "EMAIL");
        for (const auto& m : phone.finditer(cell)) tok.assign(m.text, "PHONE");
      }
    }
  }
  return "【データ表要約】" + std::to_string(rows.size()) +
         "件の個人データ（氏名・会社・連絡先）が含まれる。個票はAIに渡さず件数のみ報告に用いる。";
}

/// 抽出結果を案件単位の1本テキストに束ねる（bundle_blocks の移植）。
///
/// **副作用**: データ表の指定列は tok の対応表に登録される。
inline std::string bundle_blocks(tokenizer::Tokenizer& tok, const std::vector<FileResult>& extracted,
                                 sudachi::Analyzer* sd = nullptr) {
  // 複数ファイルのときだけ、各本文の先頭に【ファイル名】見出しを付ける
  // （どの本文がどのファイル由来か分かるように）。単一ファイルなら冗長なので付けない。
  int text_files = 0;
  for (const auto& r : extracted)
    if (!r.text.empty()) ++text_files;
  const bool per_file_header = text_files > 1;

  std::vector<std::string> blocks;
  for (const auto& r : extracted)
    if (!r.text.empty())
      blocks.push_back(per_file_header && !r.source.empty() ? "【" + r.source + "】\n" + r.text
                                                            : r.text);

  // 【ファイル名】ブロック（xlsx 容器は除く・重複除去して昇順）
  const std::string namesblk = filenames_block(extracted);
  if (!namesblk.empty()) blocks.insert(blocks.begin(), namesblk);

  for (const auto& r : extracted)
    if (is_table_kind(r.kind) && r.has_table)
      blocks.push_back(
          tokenize_table(tok, r.headers, r.rows, r.name_cols, r.company_cols, sd));

  std::string out;
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    if (i) out += "\n\n";
    out += blocks[i];
  }
  return out;
}

}  // namespace step5
