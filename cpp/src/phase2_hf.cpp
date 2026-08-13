// Phase 2 の中間段: HF NER のスパンが Python と一致するか。
//
// testdata/candidates_ref.json の hf_spans が Python(E3'構成)の正解。
// ONNX とトークナイザは Phase 0 で等価性を実測済みなので、ここで検証するのは
//   ・チャンク分割（_chunk_by_chars 250字）
//   ・aggregation_strategy="simple" の集約（softmax→argmax→連続同ラベル結合→"O"除去）
//   ・entity_group の split("-",1)[-1]（ORG-P→"P" / ORG-O→"O"）
// の写経が正しいか。
//
// score は候補の並びにも _drop にも使われない（extract_candidates は
// (種別, -出現数, 表記) で並べる）ので、判定は type/begin/end で行い score は参考表示に留める。
//
// 使い方: phase2_hf [candidates_ref.json]

#include <cstdio>
#include <fstream>
#include <map>
#include <string>

#include "hf_ner.hpp"
#include "json.hpp"

using json = nlohmann::json;

int main(int argc, char** argv) {
  const std::string ref_path = (argc > 1) ? argv[1] : "testdata/candidates_ref.json";
  std::ifstream f(ref_path);
  if (!f) {
    std::fprintf(stderr, "開けません: %s\n", ref_path.c_str());
    return 2;
  }
  json ref;
  f >> ref;

  // ラベル写像は oracle 側が持つ（Python と同じものを使う）。
  std::map<std::string, std::string> lmap;
  for (auto it = ref["label_map"].begin(); it != ref["label_map"].end(); ++it)
    lmap[it.key()] = it.value().get<std::string>();
  std::printf("label_map:");
  for (const auto& [k, v] : lmap) std::printf(" %s→%s", k.c_str(), v.c_str());
  std::printf("\n\n");

  hf::Ner ner("models/model_fp16.onnx", "models/tokenizer.json", "models/labels.json", lmap);

  std::size_t ok = 0, ng = 0;
  int shown = 0;
  for (const auto& c : ref["cases"]) {
    const auto text = c["text"].get<std::string>();
    const auto got = ner.spans(text);
    const auto& want = c["hf_spans"];

    if (got.size() != want.size()) {
      std::printf("  %-26s 件数差: C++=%zu Python=%zu\n", c["doc"].get<std::string>().c_str(),
                  got.size(), want.size());
      ng += std::max(got.size(), want.size());
      // 先頭の食い違いを出す（原因の当たりをつけるため）
      for (std::size_t i = 0; i < std::min(got.size(), want.size()) && shown < 6; ++i) {
        if (got[i].type != want[i]["type"].get<std::string>() ||
            got[i].begin != want[i]["begin"].get<std::size_t>() ||
            got[i].end != want[i]["end"].get<std::size_t>()) {
          ++shown;
          std::printf("      [%zu] C++=%s(%zu,%zu) / Python=%s(%zu,%zu)\n", i, got[i].type.c_str(),
                      got[i].begin, got[i].end, want[i]["type"].get<std::string>().c_str(),
                      want[i]["begin"].get<std::size_t>(), want[i]["end"].get<std::size_t>());
          break;
        }
      }
      continue;
    }
    std::size_t doc_ok = 0;
    for (std::size_t i = 0; i < got.size(); ++i) {
      const bool same = got[i].type == want[i]["type"].get<std::string>() &&
                        got[i].begin == want[i]["begin"].get<std::size_t>() &&
                        got[i].end == want[i]["end"].get<std::size_t>();
      same ? (++ok, ++doc_ok) : ++ng;
      if (!same && shown++ < 6)
        std::printf("      差分 [%zu] C++=%s(%zu,%zu) / Python=%s(%zu,%zu)\n", i,
                    got[i].type.c_str(), got[i].begin, got[i].end,
                    want[i]["type"].get<std::string>().c_str(),
                    want[i]["begin"].get<std::size_t>(), want[i]["end"].get<std::size_t>());
    }
    std::printf("  %-26s %zu/%zu\n", c["doc"].get<std::string>().c_str(), doc_ok, got.size());
  }

  std::printf("\n  hf_spans 一致 %zu/%zu\n", ok, ok + ng);
  const bool pass = (ng == 0 && ok > 0);
  std::printf("\n  === Phase 2 中間段（HF集約）: %s ===\n",
              pass ? "PASS (Python と完全一致)" : "FAIL");
  return pass ? 0 : 1;
}
