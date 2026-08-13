// Phase 0 の残り半分: テキスト -> input_ids ＋ offsets を C++ 側で通す適合試験。
//
// phase0_parity は ref.json の input_ids をそのまま使っており、「テキストをどう分割したか」は
// Python 任せだった。ここでは models/tokenizer.json を Rust tokenizers（自作シム）で読み、
// ref.json の text から input_ids と offsets を復元して突き合わせる。
//
// **offsets も必ず測る。** HF の aggregation_strategy="simple" はトークンの文字オフセットから
// エンティティのスパンを組むので、ここが文字/バイトで食い違うと Phase 2 の hf_spans が
// 日本語で必ず壊れる。ids だけ見ていると気づけない。
//
// 使い方: phase0_tokenize [tokenizer.json] [ref.json]

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "json.hpp"
#include "tokenizer_shim.hpp"

using json = nlohmann::json;

int main(int argc, char** argv) {
  const std::string tok_path = (argc > 1) ? argv[1] : "models/tokenizer.json";
  const std::string ref_path = (argc > 2) ? argv[2] : "testdata/ref.json";

  std::ifstream f(ref_path);
  if (!f) {
    std::fprintf(stderr, "開けません: %s\n", ref_path.c_str());
    return 2;
  }
  json ref;
  f >> ref;

  tok::Tokenizer tk(tok_path);
  std::printf("tokenizer : %s\n", tok_path.c_str());
  std::printf("ref       : %s (%zu チャンク)\n", ref_path.c_str(), ref.size());
  std::printf("add_special_tokens = true（<s>/</s> を付ける。transformers の既定と揃える）\n\n");

  std::size_t ok_ids = 0, ng_ids = 0, ok_off = 0, ng_off = 0;
  int shown = 0;

  for (const auto& r : ref) {
    const auto text = r["text"].get<std::string>();
    const auto want_ids = r["input_ids"].get<std::vector<std::int64_t>>();
    const auto enc = tk.encode(text);

    // ---- ids ----
    if (enc.ids == want_ids) {
      ++ok_ids;
    } else {
      ++ng_ids;
      if (shown++ < 5) {
        std::printf("    ids 不一致 %s#%d: C++=%zu tok / Python=%zu tok\n",
                    r["doc"].get<std::string>().c_str(), r["chunk"].get<int>(), enc.ids.size(),
                    want_ids.size());
        for (std::size_t i = 0; i < std::min(enc.ids.size(), want_ids.size()); ++i) {
          if (enc.ids[i] != want_ids[i]) {
            std::printf("        先頭のズレ: idx=%zu C++=%lld Python=%lld\n", i,
                        static_cast<long long>(enc.ids[i]),
                        static_cast<long long>(want_ids[i]));
            break;
          }
        }
      }
    }

    // ---- offsets（文字単位で一致するか） ----
    if (!r.contains("offsets")) continue;
    const auto want_off = r["offsets"].get<std::vector<std::vector<std::size_t>>>();
    bool off_same = (enc.begins.size() == want_off.size());
    if (off_same) {
      for (std::size_t i = 0; i < want_off.size(); ++i) {
        if (enc.begins[i] != want_off[i][0] || enc.ends[i] != want_off[i][1]) {
          off_same = false;
          if (shown++ < 5)
            std::printf("    offset 差分 %s#%d [%zu]: C++=(%zu,%zu) Python=(%zu,%zu)\n",
                        r["doc"].get<std::string>().c_str(), r["chunk"].get<int>(), i,
                        enc.begins[i], enc.ends[i], want_off[i][0], want_off[i][1]);
          break;
        }
      }
    }
    off_same ? ++ok_off : ++ng_off;
  }

  std::printf("\n  ids 一致     %zu/%zu\n", ok_ids, ok_ids + ng_ids);
  std::printf("  offsets 一致 %zu/%zu\n", ok_off, ok_off + ng_off);
  const bool pass = (ng_ids == 0 && ng_off == 0 && ok_ids > 0);
  std::printf("\n  === トークナイザ等価性: %s ===\n",
              pass ? "PASS (Python と完全一致)" : "FAIL");
  return pass ? 0 : 1;
}
