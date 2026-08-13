// Phase 5 コア残りの適合試験（find_mask_spans / safety_gate / tokenize_table）。
//
// UI そのものは Windows 固有だが、この3つは純ロジックなので Linux で oracle 検証してから
// WebView2 に載せる。
//
// 使い方: phase5_core [phase5_ref.json] [sudachi_ref.json]

#include <cstdio>
#include <fstream>
#include <string>

#include "extractors.hpp"
#include "json.hpp"
#include "step5.hpp"
#include "tokenizer.hpp"

using json = nlohmann::json;

int main(int argc, char** argv) {
  const std::string ref_path = (argc > 1) ? argv[1] : "testdata/phase5_ref.json";
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

  std::size_t span_ok = 0, span_ng = 0, gate_ok = 0, gate_ng = 0;
  int shown = 0;
  for (const auto& c : ref["cases"]) {
    const auto doc = c["doc"].get<std::string>();
    const auto text = c["text"].get<std::string>();
    std::vector<tokenizer::ConfirmedTerm> conf;
    for (const auto& x : c["confirmed"])
      conf.push_back({x["text"].get<std::string>(), x["type"].get<std::string>()});

    // ---- find_mask_spans ----
    const auto spans = tokenizer::find_mask_spans(text, conf, &sd);
    const auto& want = c["mask_spans"];
    bool spans_same = spans.size() == want.size();
    for (std::size_t i = 0; spans_same && i < spans.size(); ++i)
      spans_same = spans[i].begin == want[i][0].get<std::size_t>() &&
                   spans[i].end == want[i][1].get<std::size_t>() &&
                   spans[i].category == want[i][2].get<std::string>();
    spans_same ? ++span_ok : ++span_ng;
    if (!spans_same && shown++ < 6) {
      std::printf("  %-26s spans NG: C++=%zu Python=%zu\n", doc.c_str(), spans.size(),
                  want.size());
      for (std::size_t i = 0; i < std::min(spans.size(), want.size()); ++i)
        if (spans[i].begin != want[i][0].get<std::size_t>() ||
            spans[i].end != want[i][1].get<std::size_t>() ||
            spans[i].category != want[i][2].get<std::string>()) {
          std::printf("      [%zu] C++=(%zu,%zu,%s) Python=(%zu,%zu,%s)\n", i, spans[i].begin,
                      spans[i].end, spans[i].category.c_str(), want[i][0].get<std::size_t>(),
                      want[i][1].get<std::size_t>(), want[i][2].get<std::string>().c_str());
          break;
        }
    }

    // ---- safety_gate（opt-out で漏れゼロ） ----
    tokenizer::Tokenizer tok;
    const auto masked = tok.tokenize(text, conf, &sd);
    std::vector<std::string> raw;
    for (const auto& [v, t] : tok.mapping_ordered()) raw.push_back(v);
    const auto leaks = tokenizer::safety_gate(masked, raw);
    const bool clean = leaks.empty();
    const bool want_clean = c["gate_clean"].empty();
    (clean == want_clean) ? ++gate_ok : ++gate_ng;

    // ---- safety_gate（1語抜くと検出） ----
    if (!c["gate_leak"].is_null()) {
      const auto dropped = c["gate_leak"]["dropped"].get<std::string>();
      std::vector<tokenizer::ConfirmedTerm> conf2;
      bool skipped = false;
      for (const auto& x : conf)
        if (!skipped && x.text == dropped && x.entity_type == "PERSON")
          skipped = true;  // 最初の該当を1つ抜く
        else
          conf2.push_back(x);
      tokenizer::Tokenizer tok2;
      const auto masked2 = tok2.tokenize(text, conf2, &sd);
      std::vector<std::string> raw2;
      for (const auto& [v, t] : tok2.mapping_ordered()) raw2.push_back(v);
      raw2.push_back(dropped);
      const auto leaks2 = tokenizer::safety_gate(masked2, raw2);
      const bool found = std::find(leaks2.begin(), leaks2.end(), dropped) != leaks2.end();
      found ? ++gate_ok : ++gate_ng;
      if (!found && shown++ < 6)
        std::printf("  %-26s gate_leak NG: %s を検出できず\n", doc.c_str(), dropped.c_str());
    }

    std::printf("  %-26s spans %s / gate %s\n", doc.c_str(), spans_same ? "OK" : "NG",
                clean == want_clean ? "OK" : "NG");
  }

  // ---- tokenize_table ----
  bool table_ok = true;
  if (!ref["table"].is_null()) {
    const auto& t = ref["table"];
    const auto csv = t["csv"].get<std::string>();
    const auto tbl = extractors::read_csv_table(csv);
    tokenizer::Tokenizer tok;
    std::vector<std::string> ncols, ccols;
    for (const auto& x : t["name_cols"]) ncols.push_back(x.get<std::string>());
    for (const auto& x : t["company_cols"]) ccols.push_back(x.get<std::string>());
    const auto summary = step5::tokenize_table(tok, tbl.headers, tbl.rows, ncols, ccols, &sd);
    const bool sum_ok = summary == t["summary"].get<std::string>();
    // 対応表（値→トークン）の一致
    json got_map = json::object();
    for (const auto& [v, tk] : tok.mapping_ordered()) got_map[v] = tk;
    const bool map_ok = got_map == t["mapping"];
    table_ok = sum_ok && map_ok;
    std::printf("\n  [table] 要約 %s / 対応表 %s（C++=%zu Python=%zu）\n", sum_ok ? "OK" : "NG",
                map_ok ? "OK" : "NG", got_map.size(), t["mapping"].size());
    if (!map_ok)
      for (auto it = t["mapping"].begin(); it != t["mapping"].end(); ++it)
        if (!got_map.contains(it.key()) || got_map[it.key()] != it.value()) {
          std::printf("      差分: %s → C++=%s Python=%s\n", it.key().c_str(),
                      got_map.contains(it.key()) ? got_map[it.key()].get<std::string>().c_str()
                                                 : "(無)",
                      it.value().get<std::string>().c_str());
          break;
        }
  }

  std::printf("\n  find_mask_spans %zu/%zu ・ safety_gate %zu/%zu ・ tokenize_table %s\n", span_ok,
              span_ok + span_ng, gate_ok, gate_ok + gate_ng, table_ok ? "OK" : "NG");
  const bool pass = !span_ng && !gate_ng && table_ok && span_ok;
  std::printf("\n  === Phase 5 コア（安全ゲート・ハイライト・束ね）: %s ===\n",
              pass ? "PASS (Python と一致)" : "FAIL");
  return pass ? 0 : 1;
}
