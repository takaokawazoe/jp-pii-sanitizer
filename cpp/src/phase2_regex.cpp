// Phase 2 の土台: 正規表現が Python re と同じスパンを返すか。
//
// testdata/regex_ref.json は Python が出した「正解」:
//   実文書5本 × 8パターン（社名前株/後株・住所・メール・電話・郵便・都道府県・PII分割）の全マッチ。
// 同じパターン文字列を PCRE2 に食わせ、スパンと表層が一致するかを見る。
//
// ここがズレると検知スパンが変わり、Phase 2 の recall（174/175）が再現しない。
// パイプライン全体を組む前に、regexエンジンの差だけを切り分けて潰すための試験。
//
// 使い方: phase2_regex [regex_ref.json]

#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "furigana.hpp"  // char_offsets（文字↔バイトの変換）
#include "json.hpp"
#include "re.hpp"

using json = nlohmann::json;

int main(int argc, char** argv) {
  const std::string ref_path = (argc > 1) ? argv[1] : "testdata/regex_ref.json";
  std::ifstream f(ref_path);
  if (!f) {
    std::fprintf(stderr, "開けません: %s\n", ref_path.c_str());
    return 2;
  }
  json ref;
  f >> ref;

  // パターン文字列は oracle 側が持っている（Python と同じ物を使う）。
  std::map<std::string, std::unique_ptr<re::Regex>> rx;
  for (auto it = ref["patterns"].begin(); it != ref["patterns"].end(); ++it) {
    try {
      rx[it.key()] = std::make_unique<re::Regex>(it.value().get<std::string>());
    } catch (const std::exception& e) {
      std::printf("  コンパイル失敗 [%s]: %s\n", it.key().c_str(), e.what());
      return 1;
    }
  }
  std::printf("パターン: %zu 種\n\n", rx.size());

  std::map<std::string, std::pair<std::size_t, std::size_t>> score;  // key → (ok, ng)
  int shown = 0;

  for (const auto& c : ref["cases"]) {
    const auto text = c["text"].get<std::string>();
    // Python のスパンは文字単位、PCRE2 はバイト単位。文字→バイトの対応表で突き合わせる。
    const auto off = furigana::char_offsets(text);

    for (auto& [key, r] : rx) {
      const auto got = r->finditer(text);
      const auto& want = c["matches"][key];
      auto& [ok, ng] = score[key];

      if (got.size() != want.size()) {
        ng += std::max(got.size(), want.size());
        if (shown++ < 8)
          std::printf("    件数差 [%s] %s: C++=%zu Python=%zu\n", key.c_str(),
                      c["doc"].get<std::string>().c_str(), got.size(), want.size());
        continue;
      }
      for (std::size_t i = 0; i < got.size(); ++i) {
        const std::size_t wb = off[want[i]["begin"].get<std::size_t>()];
        const std::size_t we = off[want[i]["end"].get<std::size_t>()];
        const bool same = got[i].begin == wb && got[i].end == we &&
                          got[i].text == want[i]["text"].get<std::string>();
        same ? ++ok : ++ng;
        if (!same && shown++ < 8) {
          std::printf("    差分 [%s] %s [%zu]\n", key.c_str(),
                      c["doc"].get<std::string>().c_str(), i);
          std::printf("        C++   : %zu..%zu %s\n", got[i].begin, got[i].end,
                      got[i].text.c_str());
          std::printf("        Python: %zu..%zu %s\n", wb, we,
                      want[i]["text"].get<std::string>().c_str());
        }
      }
    }
  }

  std::size_t tot_ok = 0, tot_ng = 0;
  std::printf("\n  パターン別:\n");
  for (const auto& [key, s] : score) {
    std::printf("    %-12s %zu/%zu\n", key.c_str(), s.first, s.first + s.second);
    tot_ok += s.first;
    tot_ng += s.second;
  }
  std::printf("\n  合計 %zu/%zu\n", tot_ok, tot_ok + tot_ng);
  const bool pass = (tot_ng == 0 && tot_ok > 0);
  std::printf("\n  === Phase 2 土台（regex 等価性）: %s ===\n",
              pass ? "PASS (Python re と完全一致)" : "FAIL");
  return pass ? 0 : 1;
}
