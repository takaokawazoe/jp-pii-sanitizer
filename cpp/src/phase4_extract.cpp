// Phase 4 の完成条件を判定する適合試験。
//
// testdata/extract_ref.json の各形式の抽出テキストが Python 版と一致するか。
// まず txt/csv（4d）→ docx/pptx/xlsx（4a）と段階的に対象を増やす。
// pdf(4b)/msg(4c) はスパイク済みで、繋ぎ込み時にここへ追加する。
//
// 使い方: phase4_extract [extract_ref.json]

#include <cstdio>
#include <fstream>
#include <string>

#include "extractors.hpp"
#include "file_io.hpp"
#include "json.hpp"
#include "ooxml.hpp"
#include "pdf.hpp"

using json = nlohmann::json;

int main(int argc, char** argv) {
  const std::string ref_path = (argc > 1) ? argv[1] : "testdata/extract_ref.json";
  std::ifstream f(ref_path);
  if (!f) {
    std::fprintf(stderr, "開けません: %s\n", ref_path.c_str());
    return 2;
  }
  json ref;
  f >> ref;

  std::size_t ok = 0, ng = 0;
  int shown = 0;
  auto report = [&](const std::string& kind, const std::string& path, bool same,
                    const std::string& got = "", const std::string& want = "") {
    same ? ++ok : ++ng;
    std::printf("  [%-10s] %-34s %s\n", kind.c_str(),
                path.substr(path.find_last_of('/') + 1).c_str(), same ? "OK" : "NG");
    if (!same && shown++ < 4) {
      // 先頭のズレ位置を出す
      std::size_t i = 0;
      while (i < got.size() && i < want.size() && got[i] == want[i]) ++i;
      std::printf("      len: C++=%zu Python=%zu  先頭ズレ@%zu\n", got.size(), want.size(), i);
      std::printf("      C++   …%s\n", got.substr(i > 20 ? i - 20 : 0, 60).c_str());
      std::printf("      Python…%s\n", want.substr(i > 20 ? i - 20 : 0, 60).c_str());
    }
  };

  for (const auto& c : ref["cases"]) {
    const auto kind = c["kind"].get<std::string>();
    const auto path = c["path"].get<std::string>();

    if (kind == "txt") {
      const auto got = extractors::extract_plain(path);
      report(kind, path, got == c["text"].get<std::string>(), got, c["text"].get<std::string>());
    } else if (kind == "csv-table") {
      const auto t = extractors::read_csv_table(path);
      json got;
      got["headers"] = t.headers;
      got["rows"] = t.rows;
      const bool same =
          got["headers"] == c["headers"] && got["rows"] == c["rows"];
      report(kind, path, same);
      if (!same && shown++ < 4)
        std::printf("      headers C++=%zu Python=%zu / rows C++=%zu Python=%zu\n",
                    t.headers.size(), c["headers"].size(), t.rows.size(), c["rows"].size());
    } else if (kind == "docx") {
      const auto got = ooxml::extract_docx(path);
      report(kind, path, got == c["text"].get<std::string>(), got, c["text"].get<std::string>());
    } else if (kind == "pptx") {
      const auto got = ooxml::extract_pptx(path);
      report(kind, path, got == c["text"].get<std::string>(), got, c["text"].get<std::string>());
    } else if (kind == "xlsx-prose") {
      const auto got = ooxml::read_xlsx_prose(path);
      report(kind, path, got == c["text"].get<std::string>(), got, c["text"].get<std::string>());
    } else if (kind == "pdf") {
      const auto got = pdf::extract_pdf(path);
      report(kind, path, got == c["text"].get<std::string>(), got, c["text"].get<std::string>());
    } else {
      std::printf("  [%-10s] %s  (未対応・skip)\n", kind.c_str(), path.c_str());
    }
  }


  // ---- xlsx のふりがな（<rPh>）を本文に混ぜないか（回帰試験）----
  //
  // 日本語版 Excel はセルに入力したふりがなを同じ <si> の中に
  // `<rPh sb=".." eb=".."><t>ヤマダ</t></rPh>` として持つ。`.//t` を素直に全部拾うと
  // 「山田太郎ヤマダタロウ」になり、**Excel の画面にも openpyxl にも現れない文字列**が
  // 本文に混ざる。カタカナの断片が人名候補として出る原因にもなる。
  //
  // セルの値として入っている「フリガナ列」（si[3] の サトウ ミサキ）は今までどおり拾うこと。
  // 見えているデータなので対象から外す理由が無い。
  int rph_ng = 0;
  {
    const std::string p = "testdata/xlsx_furigana.xlsx";
    if (!fileio::exists(p)) {
      std::printf("  NG: フィクスチャが無い: %s\n", p.c_str());
      ++rph_ng;
    } else {
      const std::string got = ooxml::read_xlsx_prose(p);
      for (const char* want : {"山田太郎", "営業部", "サトウ ミサキ"}) {
        if (got.find(want) == std::string::npos) {
          std::printf("  NG: xlsx に %s が無い\n", want);
          ++rph_ng;
        }
      }
      for (const char* bad : {"ヤマダ", "タロウ", "エイギョウブ"}) {
        if (got.find(bad) != std::string::npos) {
          std::printf("  NG: ふりがな %s が本文に混ざっている\n", bad);
          ++rph_ng;
        }
      }
      if (rph_ng) std::printf("      抽出: %s\n", got.c_str());
    }
    if (rph_ng == 0) std::printf("  xlsx のふりがな(rPh)を除外: OK\n");
  }
  std::printf("\n  抽出一致 %zu/%zu\n", ok, ok + ng);
  const bool pass = (ng == 0 && ok > 0 && rph_ng == 0);
  std::printf("\n  === Phase 4（入口・抽出テキスト）: %s ===\n",
              pass ? "PASS (Python と一致)" : "FAIL");
  return pass ? 0 : 1;
}
