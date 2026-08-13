// Phase 3 の完成条件を判定する適合試験。
//
// testdata/tokenize_ref.json が Python の正解。実文書5本 × 全候補（opt-out＝実運用の既定）で
//   ① readings   : フリガナ読みの生成
//   ② masked     : 可逆マスク（{{PERSON_1}} 形式）
//   ③ mapping    : 対応表（値→トークン。一貫性＝同じ値は同じトークン）
//   ④ redacted   : 不可逆マスク（[人名1] 形式）
//   ⑤ restored   : reverse(masked) の結果
//   ⑥ leftovers  : 逆置換後の残骸検出
// を突き合わせる。
//
// **restored は元テキストとは一致しない**（設計どおり）。フリガナ名寄せと空白無視マッチで
// 表記が正規化されるため。判定は「Python と同じ出力か」で行う。
//
// 使い方: phase3_tokenize [tokenize_ref.json] [sudachi_ref.json]

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "json.hpp"
#include "tokenizer.hpp"

using json = nlohmann::json;

int main(int argc, char** argv) {
  const std::string ref_path = (argc > 1) ? argv[1] : "testdata/tokenize_ref.json";
  const std::string sud_path = (argc > 2) ? argv[2] : "testdata/sudachi_ref.json";

  json ref, sref;
  {
    std::ifstream a(ref_path), b(sud_path);
    if (!a || !b) {
      std::fprintf(stderr, "oracle を開けません\n");
      return 2;
    }
    a >> ref;
    b >> sref;
  }
  const auto sp = sref["paths"];
  sudachi::Analyzer sd(sp["config"].get<std::string>(), sp["resource_dir"].get<std::string>(),
                       sp["system_dic"].get<std::string>());

  struct Score {
    std::size_t ok = 0, ng = 0;
  };
  Score reads, masked, mapping, redacted, restored, leftovers;
  int shown = 0;

  for (const auto& c : ref["cases"]) {
    const auto doc = c["doc"].get<std::string>();
    const auto text = c["text"].get<std::string>();
    std::vector<tokenizer::ConfirmedTerm> conf;
    for (const auto& x : c["confirmed"])
      conf.push_back({x["text"].get<std::string>(), x["type"].get<std::string>()});

    // ---- ① readings ----
    for (auto it = c["readings"].begin(); it != c["readings"].end(); ++it) {
      auto got = furigana::readings(sd, it.key());
      auto want = it.value().get<std::vector<std::string>>();
      // oracle は set 由来の順序ゆれを避けるためソート済み。こちらも揃える。
      std::sort(got.begin(), got.end());
      (got == want) ? ++reads.ok : ++reads.ng;
      if (got != want && shown++ < 6) {
        std::printf("    readings 差分 %s\n", it.key().c_str());
        std::printf("        C++   :");
        for (const auto& v : got) std::printf(" %s", v.c_str());
        std::printf("\n        Python:");
        for (const auto& v : want) std::printf(" %s", v.c_str());
        std::printf("\n");
      }
    }

    // ---- ②③⑤⑥ 可逆 ----
    tokenizer::Tokenizer tk(true);
    const auto got_masked = tk.tokenize(text, conf, &sd);
    (got_masked == c["masked"].get<std::string>()) ? ++masked.ok : ++masked.ng;

    json got_map = json::object();
    for (const auto& [v, t] : tk.mapping_ordered()) got_map[v] = t;
    (got_map == c["mapping"]) ? ++mapping.ok : ++mapping.ng;
    if (got_map != c["mapping"] && shown++ < 8) {
      std::printf("    mapping 差分 %s: C++=%zu件 Python=%zu件\n", doc.c_str(),
                  got_map.size(), c["mapping"].size());
      for (auto it = c["mapping"].begin(); it != c["mapping"].end(); ++it) {
        if (!got_map.contains(it.key())) {
          std::printf("        Python のみ: %s → %s\n", it.key().c_str(),
                      it.value().get<std::string>().c_str());
          break;
        }
        if (got_map[it.key()] != it.value()) {
          std::printf("        値ズレ: %s → C++=%s / Python=%s\n", it.key().c_str(),
                      got_map[it.key()].get<std::string>().c_str(),
                      it.value().get<std::string>().c_str());
          break;
        }
      }
    }

    const auto got_restored = tk.reverse(got_masked);
    (got_restored == c["restored"].get<std::string>()) ? ++restored.ok : ++restored.ng;

    const auto got_left = tokenizer::find_unreplaced_tokens(got_restored);
    (got_left == c["leftovers"].get<std::vector<std::string>>()) ? ++leftovers.ok : ++leftovers.ng;

    // ---- ④ 不可逆 ----
    tokenizer::Tokenizer tk2(false);
    const auto got_redacted = tk2.tokenize(text, conf, &sd);
    (got_redacted == c["redacted"].get<std::string>()) ? ++redacted.ok : ++redacted.ng;

    std::printf("  %-26s masked=%s mapping=%s redacted=%s restored=%s\n", doc.c_str(),
                got_masked == c["masked"].get<std::string>() ? "OK" : "NG",
                got_map == c["mapping"] ? "OK" : "NG",
                got_redacted == c["redacted"].get<std::string>() ? "OK" : "NG",
                got_restored == c["restored"].get<std::string>() ? "OK" : "NG");
  }

  auto line = [](const char* n, const Score& s) {
    std::printf("  %-18s %zu/%zu\n", n, s.ok, s.ok + s.ng);
  };
  std::printf("\n");
  line("readings", reads);
  line("masked(可逆)", masked);
  line("mapping(対応表)", mapping);
  line("redacted(不可逆)", redacted);
  line("restored(逆置換)", restored);
  line("leftovers(残骸)", leftovers);

  const bool pass = !reads.ng && !masked.ng && !mapping.ng && !redacted.ng && !restored.ng &&
                    !leftovers.ng && masked.ok;
  std::printf("\n  === Phase 3（トークン化・逆置換）: %s ===\n",
              pass ? "PASS (Python と完全一致)" : "FAIL");
  return pass ? 0 : 1;
}
