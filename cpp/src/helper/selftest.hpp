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
#include "utf8.hpp"

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

  // ---- 6) 列指定（セル全体トークン・対象外・値の opt-out・ID 種別） ----
  std::printf("columns:\n");
  {
    const std::vector<Cell> t = {
        {1, 1, "ID"},    {1, 2, "氏名"},        {1, 3, "住所"},                         {1, 4, "電話"},          {1, 5, "備考"},             {1, 6, "メモ"},                    {1, 7, "メール"},            {1, 8, "No"},
        {2, 1, "A-001"}, {2, 2, "山田太郎"},    {2, 3, "〒100-0001 東京都千代田区千代田1-1"}, {2, 4, "内線1234"},      {2, 5, "山田太郎に連絡済み"}, {2, 6, "山田太郎に連絡済み"},        {2, 7, "taro@example.jp"},  {2, 8, "1"},
        {3, 1, "A-002"}, {3, 2, "なし"},        {3, 3, "-"},                            {3, 4, "03-1234-5678"},  {3, 5, "A-001 の件"},       {3, 6, "佐藤花子を担当"},            {3, 7, "-"},                {3, 8, "2"},
        {4, 1, "A-003"}, {4, 2, " 佐藤 花子"},  {4, 3, "大阪府大阪市北区1-1"},           {4, 4, "090-1111-2222"},                             {4, 6, "A-001 の件、A-0012 は別"},                               {4, 8, "3"},
    };
    const json ld2 = s.load(t);
    check(ld2.value("ok", false), "load ok（列指定用の表）");
    // 見出し行（1 行目）は r1=2 で外す。備考列は対象外。人名列の「なし」と住所列の「-」は外す。
    // メール列の「-」は skip に載せない（記号だけの値はヘルパーが自分で外す）。No 列は弱い値（1 桁）。
    const std::vector<ColumnRule> rules = {
        {1, 2, 4, "ID", {}},
        {2, 2, 4, "PERSON", {"なし"}},
        {3, 2, 4, "LOCATION", {"-"}},
        {4, 2, 4, "PHONE", {}},
        {5, 2, 4, "NONE", {}},
        {7, 2, 4, "EMAIL", {}},
        {8, 2, 4, "ID", {}},
    };
    // 人名列の値は辞書（confirmed）にも載せる＝他の列への波及はこちらで起きる。
    const std::vector<tokenizer::ConfirmedTerm> conf = {{"山田太郎", "PERSON"}, {"佐藤 花子", "PERSON"}};

    const json pv2 = s.preview(conf, rules);
    check(pv2.value("ok", false), "preview ok" + (pv2.value("ok", false) ? "" : "（" + pv2.value("error", "") + "）"));
    const json pc = pv2.value("cells", json::array());
    const json a23 = spans_at(pc, 2, 3);
    const std::string addr23 = "〒100-0001 東京都千代田区千代田1-1";
    check(a23.size() == 1 && a23[0][0] == 0 && a23[0][1] == utf8::char_len(addr23) &&
              a23[0][2] == "address" && a23[0][3] == addr23,
          "住所列: 郵便番号を含むセル全体が 1 つの住所スパン");
    const json a21 = spans_at(pc, 2, 1);
    check(a21.size() == 1 && a21[0][2] == "id" && a21[0][3] == "A-001", "ID 列: category=id・term=値");
    const json a24 = spans_at(pc, 2, 4);
    check(a24.size() == 1 && a24[0][2] == "phone", "電話列: 正規表現に合わない「内線1234」もセル全体が電話");
    check(spans_at(pc, 3, 2).empty() && spans_at(pc, 3, 3).empty(), "skip の値（なし・-）は伏せない");
    check(spans_at(pc, 2, 5).empty() && spans_at(pc, 3, 5).empty(), "対象外の列は氏名があってもスパンなし");
    check(spans_at(pc, 1, 2).empty() && spans_at(pc, 1, 1).empty(), "見出し行は列指定の外");
    const json a42 = spans_at(pc, 4, 2);
    check(a42.size() == 1 && a42[0][0] == 1 && a42[0][3] == "佐藤 花子", "先頭の空白は残し、値だけがスパン");
    check(spans_at(pc, 2, 6).size() == 1 && spans_at(pc, 3, 6).size() == 1, "メモ列（通常）には辞書経由で当たる");
    const json a46 = spans_at(pc, 4, 6);
    check(a46.size() == 1 && a46[0][0] == 0 && a46[0][3] == "A-001" && a46[0][2] == "id",
          "ID 列の値は通常セルにも語として当たる（「A-001 の件」の先頭だけ。「A-0012」には当たらない）");
    check(spans_at(pc, 3, 7).empty(), "メール列の「-」（記号だけ）は skip に無くてもスパンなし");
    check(spans_at(pc, 2, 7).size() == 1 && spans_at(pc, 2, 7)[0][2] == "email", "メール列はセル全体がメール");
    check(spans_at(pc, 2, 8).size() == 1 && spans_at(pc, 2, 8)[0][3] == "1", "弱い値（1 桁の No）も自分のセルでは伏せる");

    const json sn3 = s.sanitize(conf, true, rules);
    check(sn3.value("ok", false), "sanitize ok" + (sn3.value("ok", false) ? "" : "（" + sn3.value("error", "") + "）"));
    check(sn3.value("leaks", json::array()).empty(), "安全ゲート通過");
    const json sc = sn3.value("cells", json::array());
    auto text_at = [&](int r, int c) { const json x = cell_at(sc, r, c); return x.is_null() ? std::string("<unchanged>") : x.value("text", ""); };
    for (const auto& x : sc)
      std::printf("      R%dC%d → %s\n", x.value("r", 0), x.value("c", 0), x.value("text", "").c_str());
    check(text_at(2, 3) == "{{ADDRESS_1}}", "住所列のセルは丸ごと {{ADDRESS_1}}（郵便番号が別トークンにならない）");
    // 番号は行→列の順に振られる（行 2: A-001→ID_1, No「1」→ID_2 …）
    check(text_at(2, 1) == "{{ID_1}}" && text_at(3, 1) == "{{ID_3}}" && text_at(4, 1) == "{{ID_5}}", "ID 列は {{ID_n}}");
    check(text_at(2, 8) == "{{ID_2}}" && text_at(3, 8) == "{{ID_4}}" && text_at(4, 8) == "{{ID_6}}", "弱い値の No 列も自分のセルは {{ID_n}}");
    check(text_at(4, 6) == "{{ID_1}} の件、A-0012 は別", "メモ列の「A-001」は列と同じ {{ID_1}}、「A-0012」はそのまま");
    check(text_at(2, 7) == "{{EMAIL_1}}" && text_at(3, 7) == "<unchanged>", "メール列: 値は {{EMAIL_1}}、「-」は変わらない");
    check(text_at(2, 4) == "{{PHONE_1}}" && text_at(3, 4) == "{{PHONE_2}}", "電話列は内線も通常番号も {{PHONE_n}}");
    check(text_at(4, 2) == " {{PERSON_2}}", "先頭の空白を残して置換");
    check(text_at(2, 6) == "{{PERSON_1}}に連絡済み" && text_at(3, 6) == "{{PERSON_2}}を担当",
          "メモ列は列の値と同じトークン（空白違いの「佐藤花子」も寄る）");
    check(text_at(2, 5) == "<unchanged>" && text_at(3, 5) == "<unchanged>", "対象外の列は変わらない");
    check(text_at(3, 2) == "<unchanged>" && text_at(3, 3) == "<unchanged>", "skip の値は変わらない");
    const json exl = sn3.value("excludedLeaks", json::array());
    bool exl_ok = false;
    for (const auto& v : exl) if (v == "山田太郎") exl_ok = true;
    check(exl_ok, "対象外の列に残る値は excludedLeaks に出る（山田太郎）");
    check(sn3.value("mapping_count", 0) == 14, "対応表 14 件（ID×3・No×3・人名×2・住所×2・電話×3・メール×1。「-」は載らない）: " +
                                                    std::to_string(sn3.value("mapping_count", 0)));

    // 往復（空白違いの「佐藤花子」は確定語の表記「佐藤 花子」に寄って戻る）
    const std::string jsonl2 = s.mapping_jsonl();
    std::vector<Cell> masked2;
    for (const auto& x : sc) masked2.push_back({x.value("r", 0), x.value("c", 0), x.value("text", "")});
    const json rv2 = s.reverse_cells(masked2, jsonl2);
    bool rt2 = rv2.value("ok", false) && rv2.value("leftovers", json::array()).empty();
    for (const auto& orig : t) {
      const json r = cell_at(rv2.value("cells", json::array()), orig.row, orig.col);
      if (r.is_null()) continue;
      const std::string expect = (orig.row == 3 && orig.col == 6) ? "佐藤 花子を担当" : orig.text;
      if (r.value("text", "") != expect) {
        rt2 = false;
        std::printf("      R%dC%d: 期待「%s」 実際「%s」\n", orig.row, orig.col, expect.c_str(), r.value("text", "").c_str());
      }
    }
    check(rt2, "列指定の結果も往復で原文に戻る");

    const json sn3i = s.sanitize(conf, false, rules);
    const json i21 = cell_at(sn3i.value("cells", json::array()), 2, 1);
    check(sn3i.value("ok", false) && !i21.is_null() && i21.value("text", "") == "[識別子1]", "不可逆の ID は [識別子1]");

    const json bad = s.preview(conf, {{1, 2, 4, "DATE", {}}});
    check(!bad.value("ok", true), "未知の種別の列指定は ok:false");

    // プロトコル経由（columns の解釈）
    MemoryStream ms;
    json pv_req = {{"id", 1}, {"cmd", "preview"}, {"confirmed", json::array()}};
    pv_req["columns"] = json::array({{{"c", 1}, {"r1", 2}, {"kind", "ID"}},
                                     {{"c", 2}, {"r1", 2}, {"r2", 4}, {"kind", "PERSON"}, {"skip", json::array({"なし"})}}});
    ms.in += framed(pv_req.dump());
    proto::Server srv(s);
    srv.serve(ms);
    const auto rs = unframe_all(ms.out);
    check(rs.size() == 1 && rs[0].value("ok", false) &&
              spans_at(rs[0].value("cells", json::array()), 4, 1).size() == 1 &&
              spans_at(rs[0].value("cells", json::array()), 3, 2).empty() &&
              spans_at(rs[0].value("cells", json::array()), 4, 2).size() == 1,
          "protocol: columns（r2 省略は末尾まで・skip）が効く");
  }

  std::printf("%s (%d failure%s)\n", fails ? "SELFTEST FAIL" : "SELFTEST PASS", fails, fails == 1 ? "" : "s");
  return fails ? 5 : 0;
}

}  // namespace helper::selftest
