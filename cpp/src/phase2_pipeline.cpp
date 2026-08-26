// Phase 2 の完成条件を判定する適合試験。
//
// testdata/candidates_ref.json の candidates が Python(E3'構成)の正解。
// 同じテキストから C++ の検知パイプラインを回し、候補リスト（表記・種別・出現数・並び）が
// 完全一致するかを見る。
//
// **なぜ候補で判定するか**: 完成条件は「eval で 174/175」だが eval は Python 側にある。
// 候補リストが一致すれば eval の入力が同じになり、結果も定義上一致する
// （Phase 0 の「ラベルが一致すればエンティティも一致する」と同じ論法）。
//
// 使い方: phase2_pipeline [--update] [candidates_ref.json] [regex_ref.json]
//
// `--update` は候補の期待値を現在の C++ 出力で書き戻す。検知を**意図的に**変えたときだけ
// 使い、差分を必ず読んでから commit すること（oracle_io.hpp の注記を参照）。

#include <cstdio>
#include <fstream>
#include <map>
#include <string>

#include "json.hpp"
#include "oracle_io.hpp"
#include "step2.hpp"

using json = nlohmann::json;

int main(int argc, char** argv) {
  const bool update = oracle::take_update_flag(argc, argv);
  const std::string cand_path = (argc > 1) ? argv[1] : "testdata/candidates_ref.json";
  const std::string rx_path = (argc > 2) ? argv[2] : "testdata/regex_ref.json";
  const std::string sud_path = (argc > 3) ? argv[3] : "testdata/sudachi_ref.json";

  json cref, rref, sref;
  {
    std::ifstream a(cand_path), b(rx_path), c(sud_path);
    if (!a || !b || !c) {
      std::fprintf(stderr, "oracle を開けません\n");
      return 2;
    }
    a >> cref;
    b >> rref;
    c >> sref;
  }

  // パターン文字列・辞書パス・ラベル写像は oracle 側が持つ（Python と同じ物を使う）。
  std::map<std::string, std::string> pats;
  for (auto it = rref["patterns"].begin(); it != rref["patterns"].end(); ++it)
    pats[it.key()] = it.value().get<std::string>();
  step2::Patterns P(pats);

  const auto sp = sref["paths"];
  sudachi::Analyzer sd(sp["config"].get<std::string>(), sp["resource_dir"].get<std::string>(),
                       sp["system_dic"].get<std::string>());

  std::map<std::string, std::string> lmap;
  for (auto it = cref["label_map"].begin(); it != cref["label_map"].end(); ++it)
    lmap[it.key()] = it.value().get<std::string>();
  hf::Ner ner("models/model_fp16.onnx", "models/tokenizer.json", "models/labels.json", lmap);

  std::size_t ok = 0, ng = 0;
  int shown = 0;
  for (auto& c : cref["cases"]) {
    const auto text = c["text"].get<std::string>();
    const auto got = step2::extract_candidates(P, sd, ner, text);

    if (update) {
      // 現在の出力をそのまま期待値にする。判定はせず、書き戻して次のケースへ。
      json arr = json::array();
      for (const auto& g : got)
        arr.push_back({{"text", g.text}, {"type", g.entity_type}, {"count", g.count}});
      c["candidates"] = arr;
      continue;
    }

    const auto& want = c["candidates"];
    const auto doc = c["doc"].get<std::string>();

    std::size_t doc_ok = 0;
    const std::size_t n = std::min(got.size(), want.size());
    for (std::size_t i = 0; i < n; ++i) {
      const bool same = got[i].text == want[i]["text"].get<std::string>() &&
                        got[i].entity_type == want[i]["type"].get<std::string>() &&
                        got[i].count == want[i]["count"].get<int>();
      if (same) {
        ++ok;
        ++doc_ok;
      } else {
        ++ng;
        if (shown++ < 10)
          std::printf("      差分 [%zu] C++=%s/%s/%d  Python=%s/%s/%d\n", i,
                      got[i].text.c_str(), got[i].entity_type.c_str(), got[i].count,
                      want[i]["text"].get<std::string>().c_str(),
                      want[i]["type"].get<std::string>().c_str(), want[i]["count"].get<int>());
      }
    }
    if (got.size() != want.size()) {
      ng += (got.size() > want.size() ? got.size() : want.size()) - n;
      std::printf("  %-26s 件数差: C++=%zu Python=%zu\n", doc.c_str(), got.size(), want.size());
      // 片方にしか無いものを出す（原因の当たりをつけるため）
      for (std::size_t i = n; i < got.size() && shown < 14; ++i, ++shown)
        std::printf("      C++ のみ: %s/%s/%d\n", got[i].text.c_str(),
                    got[i].entity_type.c_str(), got[i].count);
      for (std::size_t i = n; i < want.size() && shown < 14; ++i, ++shown)
        std::printf("      Python のみ: %s/%s/%d\n", want[i]["text"].get<std::string>().c_str(),
                    want[i]["type"].get<std::string>().c_str(), want[i]["count"].get<int>());
    } else {
      std::printf("  %-26s %zu/%zu\n", doc.c_str(), doc_ok, want.size());
    }
  }

  if (update) return oracle::write(cand_path, cref) ? 0 : 2;

  std::printf("\n  候補一致 %zu/%zu\n", ok, ok + ng);
  const bool pass = (ng == 0 && ok > 0);
  std::printf("\n  === Phase 2（検知パイプライン）: %s ===\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
