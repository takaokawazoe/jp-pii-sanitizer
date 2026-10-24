// ヘルパーの自己診断（`jp-pii-helper --selftest`）。
//
// GUI の --selftest と同じ発想: ダイアログもパイプも介さずに、配線が正しいことと、
// **対応表が GUI/CLI と同じ書式で出ること**を確かめる。加えてこの層固有の不変条件
// （セル境界をまたがない・番兵で割ると元のセル数に戻る）と、フレーミングを検査する。
#pragma once

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "helper/protocol.hpp"
#include "helper/session.hpp"
#include "json.hpp"
#include "mapping_io.hpp"

namespace helper::selftest {

using nlohmann::json;

/// メモリ上のトランスポート。要求を詰めてから serve に渡し、応答を out から読む。
struct MemoryStream final : proto::Stream {
  std::string in;
  std::size_t pos = 0;
  std::string out;
  bool read_exact(void* buf, std::size_t n) override {
    if (pos + n > in.size()) return false;
    std::memcpy(buf, in.data() + pos, n);
    pos += n;
    return true;
  }
  bool write_all(const void* buf, std::size_t n) override {
    out.append(static_cast<const char*>(buf), n);
    return true;
  }
};

inline std::string framed(const std::string& payload) {
  const auto n = static_cast<std::uint32_t>(payload.size());
  std::string f;
  f.push_back(static_cast<char>(n & 0xFF));
  f.push_back(static_cast<char>((n >> 8) & 0xFF));
  f.push_back(static_cast<char>((n >> 16) & 0xFF));
  f.push_back(static_cast<char>((n >> 24) & 0xFF));
  return f + payload;
}

inline std::vector<json> unframe_all(const std::string& bytes) {
  std::vector<json> out;
  MemoryStream ms;
  ms.in = bytes;
  std::string one;
  while (proto::read_frame(ms, one)) out.push_back(json::parse(one));
  return out;
}

/// cells:[{r,c,...}] から (r,c) のエントリを探す。無ければ null。
inline json cell_at(const json& cells, int r, int c) {
  if (cells.is_array())
    for (const auto& x : cells)
      if (x.value("r", 0) == r && x.value("c", 0) == c) return x;
  return nullptr;
}

inline json spans_at(const json& cells, int r, int c) {
  const json x = cell_at(cells, r, c);
  return x.is_null() ? json::array() : x.value("spans", json::array());
}

inline int run(const std::string& data_root) {
  int fails = 0;
  auto check = [&](bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "OK" : "NG", what.c_str());
    if (!ok) ++fails;
  };

  std::printf("selftest: data=%s\n", data_root.c_str());
  Session s(data_root);
  if (!s.ensure_init()) {
    std::printf("init FAILED: %s\n", s.last_error().c_str());
    return 4;
  }
  std::printf("init OK\n");

  // ---- 1) 読み込みと検知 ----
  // 見出し行 ＋ 2 人ぶん ＋ 境界検査用に姓と名を別セルに割った行 ＋ 空白だけのセル。
  const std::vector<Cell> cells = {
      {1, 1, "氏名"},     {1, 2, "電話"},          {1, 3, "メール"},               {1, 4, "備考"},
      {2, 1, "山田太郎"}, {2, 2, "03-1234-5678"},  {2, 3, "taro@example-corp.jp"}, {2, 4, "株式会社サンプル商事の担当"},
      {3, 1, "佐藤 花子"}, {3, 2, "090-9876-5432"}, {3, 3, ""},                     {3, 4, "ヤマダタロウ氏と面談"},
      {4, 1, "山田"},     {4, 2, "太郎"},
      {5, 1, "  "},
  };
  std::printf("load:\n");
  const json ld = s.load(cells);
  check(ld.value("ok", false), "load ok" + (ld.value("ok", false) ? "" : "（" + ld.value("error", "") + "）"));
  check(ld.value("cell_count", 0) == 13, "空セルを除いた件数が 13");
  const json cands = ld.value("candidates", json::array());
  check(!cands.empty(), "候補が 1 件以上（" + std::to_string(cands.size()) + " 件）");
  for (const auto& c : cands)
    std::printf("      %-13s %s ×%d\n", c.value("entity_type", "").c_str(), c.value("text", "").c_str(),
                c.value("count", 0));
  bool has_yamada = false;
  for (const auto& c : cands)
    if (c.value("text", "") == "山田太郎" && c.value("entity_type", "") == "PERSON") has_yamada = true;
  check(has_yamada, "山田太郎 が PERSON 候補にある");

  // ---- 2) プレビュー: セル境界の不変条件 ----
  std::printf("preview:\n");
  const std::vector<tokenizer::ConfirmedTerm> conf1 = {{"山田太郎", "PERSON"}};
  const json pv = s.preview(conf1);
  check(pv.value("ok", false), "preview ok");
  const json pcells = pv.value("cells", json::array());
  check(spans_at(pcells, 2, 1).size() == 1, "R2C1「山田太郎」に 1 スパン");
  check(spans_at(pcells, 4, 1).empty() && spans_at(pcells, 4, 2).empty(),
        "姓と名を別セルに割った「山田｜太郎」には当たらない（境界をまたがない）");
  const json sp34 = spans_at(pcells, 3, 4);
  check(sp34.size() == 1 && sp34[0].size() == 4 && sp34[0][3] == "山田太郎",
        "読み「ヤマダタロウ」もハイライトされ、由来は漢字表記「山田太郎」");
  check(spans_at(pcells, 2, 2).size() == 1 && spans_at(pcells, 2, 3).size() == 1 &&
            spans_at(pcells, 3, 2).size() == 1,
        "電話・メールは確定リストに無くても当たる");
  check(spans_at(pcells, 2, 4).empty() && spans_at(pcells, 3, 1).empty(),
        "確定していない社名・氏名には当たらない");

  // ---- 3) サニタイズ（可逆）: 変わるセルの集合 ----
  std::printf("sanitize:\n");
  const json sn = s.sanitize(conf1, true);
  check(sn.value("ok", false), "sanitize ok" + (sn.value("ok", false) ? "" : "（" + sn.value("error", "") + "）"));
  check(sn.value("leaks", json::array()).empty(), "安全ゲート通過（leaks 0）");
  check(sn.value("mapping_count", 0) == 4, "対応表 4 件（山田太郎・電話×2・メール。読みは同一トークン）");
  const json scells = sn.value("cells", json::array());
  check(scells.size() == 5, "変わったセルは 5 つ（" + std::to_string(scells.size()) + "）");
  check(!cell_at(scells, 4, 1).is_null() == false && cell_at(scells, 4, 2).is_null(),
        "分割セルは変わらない");
  const json c21 = cell_at(scells, 2, 1), c34 = cell_at(scells, 3, 4);
  check(!c21.is_null() && c21.value("text", "") == "{{PERSON_1}}", "R2C1 → {{PERSON_1}}");
  check(!c34.is_null() && c34.value("text", "") == "{{PERSON_1}}氏と面談", "R3C4 の読みも同じ {{PERSON_1}}");
  for (const auto& x : scells)
    std::printf("      R%dC%d → %s\n", x.value("r", 0), x.value("c", 0), x.value("text", "").c_str());

  // 全候補を確定しても壊れない（件数は NER 次第なので ok と leaks だけ見る）
  std::vector<tokenizer::ConfirmedTerm> conf_all = conf1;
  for (const auto& c : cands) conf_all.push_back({c.value("text", ""), c.value("entity_type", "")});
  const json sn_all = s.sanitize(conf_all, true);
  check(sn_all.value("ok", false) && sn_all.value("leaks", json::array()).empty(),
        "全候補でもサニタイズできて安全ゲートを通る");
  // 不可逆モード
  const json sn_irr = s.sanitize(conf1, false);
  const json i21 = cell_at(sn_irr.value("cells", json::array()), 2, 1);
  check(sn_irr.value("ok", false) && !i21.is_null() && i21.value("text", "") == "[人名1]" &&
            sn_irr.value("mapping_count", 1) == 0 && !s.has_mapping(),
        "不可逆モードは [人名1] で、対応表を残さない");

  // ---- 4) 対応表の書式と往復 ----
  std::printf("mapping:\n");
  // conf1 の可逆サニタイズをやり直して対応表を持った状態に戻す
  const json sn2 = s.sanitize(conf1, true);
  const std::string jsonl = s.mapping_jsonl();
  check(jsonl.rfind("{\"_meta\":{\"version\":1}}", 0) == 0, "先頭行が {\"_meta\":{\"version\":1}}（GUI/CLI と同じ書式）");
  bool parsed_ok = false;
  try {
    parsed_ok = mapping_io::parse(jsonl).size() == 4;
  } catch (...) {
  }
  check(parsed_ok, "mapping_io（CLI の読み手）で 4 件読める");

  std::vector<Cell> masked;
  for (const auto& x : sn2.value("cells", json::array()))
    masked.push_back({x.value("r", 0), x.value("c", 0), x.value("text", "")});
  const json rv = s.reverse_cells(masked, jsonl);
  check(rv.value("ok", false), "reverse_cells ok");
  check(rv.value("leftovers", json::array()).empty(), "未置換トークン 0");
  bool roundtrip = true;
  for (const auto& orig : cells) {
    const json r = cell_at(rv.value("cells", json::array()), orig.row, orig.col);
    if (r.is_null()) continue;
    // 読み（ヤマダタロウ）は設計上、漢字表記に寄せて戻る。それ以外は原文と一致する。
    const std::string expect = (orig.row == 3 && orig.col == 4) ? "山田太郎氏と面談" : orig.text;
    if (r.value("text", "") != expect) {
      roundtrip = false;
      std::printf("      R%dC%d: 期待「%s」 実際「%s」\n", orig.row, orig.col, expect.c_str(),
                  r.value("text", "").c_str());
    }
  }
  check(roundtrip && rv.value("cells", json::array()).size() == masked.size(), "往復で原文に戻る");

  const json rt = s.reverse_text("担当: {{PERSON_1}}（{{PHONE_1}}）", jsonl);
  check(rt.value("ok", false) && rt.value("restored", "") == "担当: 山田太郎（03-1234-5678）",
        "reverse_text も同じ対応表で戻る");

  // パスフレーズ保護: 無しでは needsPassphrase（黙って 0 件成功しない）、有りで戻る
  const std::string kPass = "selftest-passphrase";
  const std::string enc = s.mapping_jsonl_encrypted(kPass);
  const json rv_no = s.reverse_cells(masked, enc);
  const json rv_ok = s.reverse_cells(masked, enc, kPass);
  check(!rv_no.value("ok", true) && rv_no.value("needsPassphrase", false),
        "暗号化対応表: パスフレーズ無しは needsPassphrase");
  check(rv_ok.value("ok", false) && rv_ok.value("leftovers", json::array()).empty() &&
            cell_at(rv_ok.value("cells", json::array()), 2, 1).value("text", "") == "山田太郎",
        "暗号化対応表: パスフレーズ有りで戻る");

  // ---- 5) プロトコル（フレーミングとディスパッチ） ----
  std::printf("protocol:\n");
  {
    json ld_req = {{"id", 3}, {"cmd", "load"}};
    ld_req["cells"] = json::array();
    for (const auto& c : cells) ld_req["cells"].push_back({{"r", c.row}, {"c", c.col}, {"text", c.text}});
    json pv_req = {{"id", 4}, {"cmd", "preview"}, {"confirmed", json::array({{{"text", "山田太郎"}, {"type", "PERSON"}}})}};
    json sn_req = {{"id", 5}, {"cmd", "sanitize"}, {"reversible", true},
                   {"confirmed", json::array({{{"text", "山田太郎"}, {"type", "PERSON"}}})}};
    MemoryStream ms;
    ms.in += framed(json{{"id", 1}, {"cmd", "hello"}}.dump());
    ms.in += framed(json{{"id", 2}, {"cmd", "init"}}.dump());
    ms.in += framed(ld_req.dump());
    ms.in += framed(pv_req.dump());
    ms.in += framed(sn_req.dump());
    ms.in += framed("this is not json");                       // 壊れた要求 → エラー応答で続行
    ms.in += framed(json{{"id", 7}, {"cmd", "nosuch"}}.dump());  // 未知コマンド
    ms.in += framed(json{{"id", 8}, {"cmd", "reset"}}.dump());
    ms.in += framed(json{{"id", 9}, {"cmd", "shutdown"}}.dump());
    ms.in += framed(json{{"id", 10}, {"cmd", "hello"}}.dump());  // shutdown 後は読まれない
    proto::Server srv(s);
    const bool quit = srv.serve(ms);
    const auto resps = unframe_all(ms.out);
    check(quit, "shutdown で serve が true を返す");
    check(resps.size() == 9, "応答が 9 件（shutdown 後の要求は処理しない）: " + std::to_string(resps.size()));
    auto at = [&](std::size_t i) { return i < resps.size() ? resps[i] : json{}; };
    check(at(0).value("ok", false) && at(0).value("protocol", 0) == proto::PROTOCOL_VERSION &&
              at(0).value("type", "") == "hello" && at(0).value("id", 0) == 1,
          "hello: ok / protocol / type / id");
    check(at(1).value("ok", false), "init ok");
    check(at(2).value("ok", false) && at(2).value("cell_count", 0) == 13, "load ok（13 セル）");
    check(at(3).value("ok", false) && spans_at(at(3).value("cells", json::array()), 2, 1).size() == 1, "preview ok");
    check(at(4).value("ok", false) && at(4).value("changed", 0) == 5, "sanitize ok（5 セル）");
    check(!at(5).value("ok", true) && at(5).value("type", "") == "error", "壊れた要求はエラー応答（切断しない）");
    check(!at(6).value("ok", true) && at(6).value("id", 0) == 7, "未知コマンドはエラー応答（id を返す）");
    check(at(7).value("ok", false) && at(8).value("ok", false), "reset / shutdown ok");
  }
  {
    // 上限超過の長さは切断扱い（読み手のメモリを食い潰さない）
    MemoryStream ms;
    ms.in = std::string("\xFF\xFF\xFF\xFF", 4);
    proto::Server srv(s);
    const bool quit = srv.serve(ms);
    check(!quit && ms.out.empty(), "上限超過のフレームは応答せず切断扱い");
  }

  std::printf("%s (%d failure%s)\n", fails ? "SELFTEST FAIL" : "SELFTEST PASS", fails, fails == 1 ? "" : "s");
  return fails ? 5 : 0;
}

}  // namespace helper::selftest
