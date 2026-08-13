// Phase 1 の完成条件を判定する適合試験。
//
// testdata/sudachi_ref.json は SudachiPy が出した「正解」:
//   デモ文書から実際に出た全NER候補120件について、(surface, part_of_speech, reading_form)。
// 同じ入力を sudachi.rs シム経由で解析し、完全一致するかを見る。
//
// SudachiPy 0.6.11 は sudachi.rs のバインディングなので、同じリソース
// （sudachi.json / char.def / unk.def / rewrite.def / system.dic）を渡せば一致するはず。
// ここがズレると furigana.person_name_core / org_name_core が別物になり、
// Phase 2 の recall（174/175）が再現しない。
//
// 使い方: phase1_sudachi [sudachi_ref.json]

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "furigana.hpp"
#include "json.hpp"
#include "sudachi_shim.hpp"

using json = nlohmann::json;

int main(int argc, char** argv) {
  const std::string ref_path = (argc > 1) ? argv[1] : "testdata/sudachi_ref.json";
  std::ifstream f(ref_path);
  if (!f) {
    std::fprintf(stderr, "開けません: %s\n", ref_path.c_str());
    return 2;
  }
  json ref;
  f >> ref;

  // リソースのパスは oracle 側が記録している。**SudachiPy と同じ実体を渡すこと**が前提で、
  // 違うものを渡すとプラグイン設定が変わって結果がズレる。
  const auto paths = ref["paths"];
  const std::string config = paths["config"].get<std::string>();
  const std::string resdir = paths["resource_dir"].get<std::string>();
  const std::string sysdic = paths["system_dic"].get<std::string>();
  std::printf("config      : %s\n", config.c_str());
  std::printf("system_dic  : %s\n", sysdic.c_str());

  sudachi::Analyzer sd(config, resdir, sysdic);

  const auto& cases = ref["cases"];
  std::printf("cases       : %zu 入力\n\n", cases.size());

  std::size_t ok_morph = 0, ng_morph = 0, ok_person = 0, ng_person = 0, ok_org = 0, ng_org = 0;
  int shown = 0;

  for (const auto& c : cases) {
    const auto input = c["input"].get<std::string>();
    const auto& want = c["morphemes"];

    // ---- ① 形態素の等価性 ----
    const auto got = sd.tokenize(input);
    if (got.size() != want.size()) {
      ng_morph += want.size();
      if (shown++ < 6)
        std::printf("    形態素数が違う %s: C++=%zu Python=%zu\n", input.c_str(), got.size(),
                    want.size());
    } else {
      for (std::size_t i = 0; i < got.size(); ++i) {
        // begin/end も必ず突き合わせる。ここが文字/バイトで食い違うと furigana の
        // 切り出しが日本語で壊れるが、表層・品詞だけ見ていると気づけない。
        const bool m = got[i].surface == want[i]["surface"].get<std::string>() &&
                       got[i].pos == want[i]["pos"].get<std::vector<std::string>>() &&
                       got[i].reading == want[i]["reading"].get<std::string>() &&
                       got[i].begin == want[i]["begin"].get<std::size_t>() &&
                       got[i].end == want[i]["end"].get<std::size_t>();
        m ? ++ok_morph : ++ng_morph;
        if (!m && shown++ < 6) {
          std::printf("    差分 %s [%zu]: C++ surface=%s begin=%zu / Python surface=%s begin=%zu\n",
                      input.c_str(), i, got[i].surface.c_str(), got[i].begin,
                      want[i]["surface"].get<std::string>().c_str(),
                      want[i]["begin"].get<std::size_t>());
        }
      }
    }

    // ---- ② person_name_core / org_name_core の等価性（Phase 1 の完成条件） ----
    const auto p_want = c["person_name_core"].get<std::string>();
    const auto p_got = furigana::person_name_core(sd, input);
    (p_got == p_want) ? ++ok_person : ++ng_person;
    if (p_got != p_want && shown++ < 10)
      std::printf("    person 差分 %s → C++=%s / Python=%s\n", input.c_str(), p_got.c_str(),
                  p_want.c_str());

    const auto o_want = c["org_name_core"].get<std::string>();
    const auto o_got = furigana::org_name_core(sd, input);
    (o_got == o_want) ? ++ok_org : ++ng_org;
    if (o_got != o_want && shown++ < 10)
      std::printf("    org 差分 %s → C++=%s / Python=%s\n", input.c_str(), o_got.c_str(),
                  o_want.c_str());
  }

  std::printf("\n  形態素一致          %zu/%zu\n", ok_morph, ok_morph + ng_morph);
  std::printf("  person_name_core 一致 %zu/%zu\n", ok_person, ok_person + ng_person);
  std::printf("  org_name_core 一致    %zu/%zu\n", ok_org, ok_org + ng_org);
  const bool pass = (ng_morph == 0 && ng_person == 0 && ng_org == 0 && ok_morph > 0);
  std::printf("\n  === Phase 1: %s ===\n",
              pass ? "PASS (SudachiPy と完全一致)" : "FAIL");
  return pass ? 0 : 1;
}
