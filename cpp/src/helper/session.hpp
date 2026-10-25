// Excel アドイン用ヘルパーの中核（docs/excel-helper.md）。
//
// GUI の appcore::Core が「ファイルの集合」を 1 案件として扱うのに対し、ここは
// 「セルの集合」（アクティブシートまたは選択範囲）を 1 案件として扱う。検知・マスク・
// 逆置換の中身は step2 / tokenizer をそのまま使い、この層が引き受けるのは
// セル ↔ テキストの往復だけ。抽出器は要らない——セルの値はアドインが Excel から渡す。
//
// **セル境界をまたぐマッチを作らない。** これがこの層の不変条件。
// プレビューもサニタイズも、全セルを番兵 CELL_SEP で連結した 1 本のテキストに対して
// GUI と同じマッチャ（find_mask_spans / Tokenizer::tokenize）を 1 回ずつ掛ける。
// 番兵はどのパターンも越えられないので、得られたスパンは必ず 1 セルに収まり、
// 置換後のテキストを番兵で割れば元のセル数に戻る。セルごとにマッチャを呼ぶ案は
// 退けた——確定語 1 つにつき正規表現を 1 本コンパイルするので、セル数 × 確定語数で
// 効いてくる（5,000 セル × 50 語で数秒。opt-out のたびに待たされる）。
//
// 検知（NER）だけは別のテキストに掛ける: 行内をタブ・行間を改行で結んだもので、
// xlsx-prose（ooxml_xlsx::sheet_prose）が作るのと同じ形。検証済みの経路から
// 外れないためと、番兵のような制御文字をモデルに見せないため。候補は「語」なので、
// どちらのテキストから出たかは以降に影響しない。
#pragma once

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "file_io.hpp"
#include "hf_ner.hpp"
#include "json.hpp"
#include "mapping_io.hpp"
#include "step2.hpp"
#include "sudachi_shim.hpp"
#include "tokenizer.hpp"
#include "utf8.hpp"

namespace helper {

using nlohmann::json;

/// 1 セル。row/col は 1 始まり（Excel の行番号・列番号）。
/// text は**表示文字列**。数値セルの表示形式（先頭 0 の電話番号など）の適用は
/// アドイン側の責務で、ここに来る時点で文字列になっている。
struct Cell {
  int row = 0;
  int col = 0;
  std::string text;
};

/// 列指定（アドインの「列の扱い」）。col 列の r1..r2 行（1 始まり・両端含む）に kind を適用する。
///
/// - kind が PERSON / ORGANIZATION / LOCATION / PHONE / EMAIL / ID のとき、該当セルは
///   **セル全体を 1 つのトークン**にする（前後の空白は残す）。確定語の照合や番号系の
///   正規表現は通さない——住所列「〒100-0001 東京都…」で郵便番号だけ先に置き換わって
///   残りの住所が確定語に当たらなくなる、という置換順の問題を避けるため。
///   値は他の（通常の）セルにも**語として**波及させ、同じトークンにする（備考「A-001 の件」→
///   「{{ID_1}} の件」。「A-0012」の中には当てない）。人名・組織・住所はアドインが confirmed にも
///   載せるので部分一致・読み寄せも効く。ただし **弱い値**（1 文字、または数字だけで 5 文字未満。
///   「1」「0042」）は自分のセルでだけ伏せ、波及も安全ゲートの総当たりもしない——文章中の
///   あらゆる「1」が残存扱いになるため。
/// - **記号だけの値**（「-」「―」）は列指定でもトークンにしない（skip と同じ扱い）。
///   定型の空欄印をメールや ID として対応表に載せても意味がなく、ゲートの誤報の元になる。
/// - kind が NONE のとき、該当セルは**対象外**: 検知も置換もせず原文のまま返す。
/// - skip はその列で伏せない値（候補一覧でチェックを外した値）。前後の空白を除いて比べる。
///   skip に当たったセルは通常セルと同じ扱い（確定語・番号系の照合は受ける）。
///
/// 同じセルに複数の規則が当たれば後勝ち。表の見出し行は r1 の外に置いて除く。
struct ColumnRule {
  int col = 0;
  int r1 = 0;
  int r2 = 0;
  std::string kind;
  std::vector<std::string> skip;
};

/// 列指定で使える種別か。NONE（対象外）も含む。
inline bool is_column_kind(const std::string& kind) {
  return kind == "NONE" || tokenizer::TOKEN_PREFIX.count(kind) > 0;
}

/// セル境界の番兵。U+001F（Unit Separator）。
///
/// - PCRE2_UCP の `\s` は Unicode White_Space にしか当たらず、U+001F は Cc なので
///   当たらない → 確定語の空白無視マッチ（`\s*`）がセルをまたげない。
/// - 番号系の区切りクラス（numpat::detail::SEP / DASH）にも入っていない。
/// - 確定語の完全一致（4 文字未満）は本文からの検索なので、語に番兵が無ければ越えない。
/// - Excel のセルには通常入力できない制御文字。万一入っていれば load で空白に潰す。
inline constexpr char CELL_SEP = '\x1F';

/// アドイン 1 接続ぶんの状態。GUI の Core（Streamlit の session_state 相当）に対応する。
class Session {
 public:
  // data_root は models/（model_fp16.onnx・tokenizer.json・labels.json・patterns.json）と
  // sudachi/（resources・system.dic）を含むディレクトリ。
  explicit Session(std::string data_root) : data_root_(std::move(data_root)) {}

  bool ready() const { return inited_; }
  const std::string& last_error() const { return err_; }
  std::size_t cell_count() const { return cells_.size(); }

  // モデル・辞書・パターンを読み込む（重い・初回のみ）。成否を返す。Core::ensure_init と同じ。
  bool ensure_init() {
    if (inited_) return true;
    try {
      const std::string pj = fileio::read_all(data_root_ + "/patterns.json");
      if (pj.empty()) throw std::runtime_error("patterns.json が読めません: " + data_root_);
      const json pjson = json::parse(pj);
      std::map<std::string, std::string> pats;
      for (auto it = pjson["patterns"].begin(); it != pjson["patterns"].end(); ++it)
        pats[it.key()] = it.value().get<std::string>();
      patterns_ = std::make_unique<step2::Patterns>(pats);

      std::map<std::string, std::string> lmap;
      for (auto it = pjson["label_map"].begin(); it != pjson["label_map"].end(); ++it)
        lmap[it.key()] = it.value().get<std::string>();

      const std::string sres = data_root_ + "/sudachi/resources";
      sd_ = std::make_unique<sudachi::Analyzer>(sres + "/sudachi.json", sres,
                                                data_root_ + "/sudachi/system.dic");
      ner_ = std::make_unique<hf::Ner>(data_root_ + "/model_fp16.onnx",
                                       data_root_ + "/tokenizer.json",
                                       data_root_ + "/labels.json", lmap);
      inited_ = true;
      return true;
    } catch (const std::exception& e) {
      err_ = e.what();
      return false;
    }
  }

  // ① セルを読み込み ＋ ② 候補検知。
  // 返り値: {ok, cell_count, candidates:[{text,entity_type,count}],
  //          cells:[{r,c,spans:[[begin,end,category,term],...]}], warnings:[...]}
  // cells に載るのはスパンを持つセルだけ。begin/end はそのセル内の文字オフセット。
  json load(std::vector<Cell> cells) {
    if (!ensure_init()) return err_json();
    clear();

    // 正規化。番兵を潰し、UTF-8 を修復し、空セルを落とし、行→列の順に並べる。
    // ---- ここが「正当な UTF-8」の出口。下流は PCRE2 も Rust シムも不正列を前提にしない ----
    for (auto& c : cells) {
      std::replace(c.text.begin(), c.text.end(), CELL_SEP, ' ');
      c.text = utf8::repair(c.text);
      if (utf8::trim(c.text).empty()) continue;
      cells_.push_back(std::move(c));
    }
    std::stable_sort(cells_.begin(), cells_.end(), [](const Cell& a, const Cell& b) {
      return a.row != b.row ? a.row < b.row : a.col < b.col;
    });

    // 検知用テキスト（sheet_prose と同じ形: 行内タブ・行間改行）。
    std::string detect_text;
    for (std::size_t i = 0; i < cells_.size(); ++i) {
      if (i) detect_text += (cells_[i].row == cells_[i - 1].row) ? '\t' : '\n';
      detect_text += cells_[i].text;
    }

    std::vector<std::string> warnings;
    if (!detect_text.empty()) {
      try {
        last_candidates_ = step2::extract_candidates(*patterns_, *sd_, *ner_, detect_text);
        // モデルがセル境界（タブ・改行）をまたいで拾った語は捨てる。番兵入りの作業テキストには
        // 当たりようがなく、候補一覧に「0 セル」で並ぶだけ。
        last_candidates_.erase(
            std::remove_if(last_candidates_.begin(), last_candidates_.end(),
                           [](const step2::Candidate& c) { return c.text.find_first_of("\t\n") != std::string::npos; }),
            last_candidates_.end());
      } catch (const std::exception& e) {
        // 検知が落ちても読み込んだセルは残す（番号系の自動マスクと手動追記は使える）。
        warnings.push_back(std::string("検知に失敗しました（") + e.what() + "）");
      }
    }

    json out;
    out["ok"] = true;
    out["cell_count"] = cells_.size();
    out["candidates"] = candidates_json(last_candidates_);
    const json pv = preview(candidates_as_confirmed(last_candidates_));
    if (!pv.value("ok", false)) return pv;
    out["cells"] = pv["cells"];
    out["warnings"] = warnings;
    return out;
  }

  // ② プレビュー更新（opt-out・列指定を反映して再ハイライト）。返り値: {ok, cells:[{r,c,spans}]}
  json preview(const std::vector<tokenizer::ConfirmedTerm>& confirmed,
               const std::vector<ColumnRule>& rules = {}) {
    if (!ensure_init()) return err_json();
    try {
      const Plan plan = make_plan(rules);
      // 列指定のセル（全体トークン・対象外）は空にしてから通常の照合に掛ける。
      // 番兵はそのまま残すので、セル番号は変わらない。
      Work w = build_work(plan, nullptr);
      // 列指定の値は語として他のセルにも当てる（サニタイズと同じ辞書で照合する＝プレビューが真実）
      const ColumnTerms ct = column_terms(plan, confirmed);
      std::map<std::size_t, json> per_cell = normal_spans(w, with_column_terms(confirmed, ct));
      for (std::size_t i = 0; i < cells_.size(); ++i) {
        if (plan.mode[i] != CellMode::Whole) continue;
        json spans = json::array();
        spans.push_back({plan.lead[i], plan.lead[i] + utf8::char_len(plan.value[i]),
                         tokenizer::hl_category(plan.kind[i]), plan.value[i]});
        per_cell[i] = spans;
      }
      json out;
      out["ok"] = true;
      out["cells"] = cells_with_spans(per_cell);
      return out;
    } catch (const std::exception& e) {
      return fail_json(std::string("プレビューの更新に失敗しました: ") + e.what());
    }
  }

  // ③ サニタイズ。返り値: {ok, cells:[{r,c,text}], cell_count, changed, mapping_count,
  //                        reversible, leaks:[...], excludedLeaks:[...]}
  // cells に載るのは**値が変わったセルだけ**。アドインは手元の値をこれで上書きして別ブックを作る。
  // excludedLeaks は対象外（NONE）の列に残っているマスク対象の値。利用者が明示して外した
  // 列なので止めはしないが、黙りもしない。
  json sanitize(const std::vector<tokenizer::ConfirmedTerm>& confirmed, bool reversible,
                const std::vector<ColumnRule>& rules = {}) {
    if (!ensure_init()) return err_json();
    if (cells_.empty()) return fail_json("読み込まれたセルがありません。");
    last_mapping_.clear();
    try {
      const Plan plan = make_plan(rules);
      tokenizer::Tokenizer tok(reversible);
      // 全体トークンのセルは先に assign して置く。番号を先に取るので、列の値が
      // 他セルの照合（confirmed 経由）で当たっても同じトークンになる。
      Work w = build_work(plan, &tok);
      const ColumnTerms ct = column_terms(plan, confirmed);
      const std::string sanitized = tok.tokenize(w.text, with_column_terms(confirmed, ct), sd_.get());

      const auto mapping = tok.mapping_ordered();
      const auto leaks = gate(sanitized, mapping, ct);

      // 番兵で割って元のセルへ戻す。数が合わなければ不変条件が破れている——黙って
      // ずらして書き戻すと、別のセルに別人の値が入る最悪の形になるので、ここで止める。
      auto parts = split_cells(sanitized);
      if (parts.size() != cells_.size())
        return fail_json("内部不整合: サニタイズ後のセル数が一致しません（" +
                         std::to_string(parts.size()) + " / " + std::to_string(cells_.size()) + "）");

      // 対象外のセルは原文に戻す（作業テキストでは空にしていた）。
      std::string excluded_text;
      for (std::size_t i = 0; i < cells_.size(); ++i) {
        if (plan.mode[i] != CellMode::Excluded) continue;
        parts[i] = cells_[i].text;
        excluded_text += cells_[i].text;
        excluded_text += '\n';
      }
      const auto excluded_leaks = gate(excluded_text, mapping, ct);

      // 不可逆モードは対応表を残さない＝ここで捨てる。
      if (reversible) last_mapping_ = tok.mapping_ordered();

      json cells = json::array();
      for (std::size_t i = 0; i < cells_.size(); ++i)
        if (parts[i] != cells_[i].text)
          cells.push_back({{"r", cells_[i].row}, {"c", cells_[i].col}, {"text", parts[i]}});

      json out;
      out["ok"] = true;
      out["cells"] = cells;
      out["cell_count"] = cells_.size();
      out["changed"] = cells.size();
      out["mapping_count"] = static_cast<int>(last_mapping_.size());
      out["reversible"] = reversible;
      out["leaks"] = leaks;  // 空なら安全ゲート通過
      out["excludedLeaks"] = excluded_leaks;
      return out;
    } catch (const std::exception& e) {
      last_mapping_.clear();
      return fail_json(std::string("サニタイズに失敗しました: ") + e.what());
    }
  }

  /// 直近のサニタイズが作った対応表（平文 JSONL）。無ければ空。
  std::string mapping_jsonl() const { return mapping_io::to_jsonl(last_mapping_); }
  /// 同じものをパスフレーズで保護した形（version 2 の封筒）。
  std::string mapping_jsonl_encrypted(std::string_view passphrase) const {
    return mapping_io::to_jsonl_encrypted(last_mapping_, passphrase);
  }
  bool has_mapping() const { return !last_mapping_.empty(); }

  // ④ 逆置換（テキスト）。{ok, restored, leftovers:[...]}。Core::reverse と同じ。
  json reverse_text(const std::string& text, const std::string& mapping_body,
                    std::string_view passphrase = {}) {
    std::vector<mapping_io::Entry> value_token;
    json fail;
    if (!parse_mapping(mapping_body, passphrase, value_token, fail)) return fail;
    try {
      tokenizer::Tokenizer tok;
      tok.load_mapping(value_token);
      const std::string restored = tok.reverse(utf8::repair(text));
      json out;
      out["ok"] = true;
      out["restored"] = restored;
      out["leftovers"] = tokenizer::find_unreplaced_tokens(restored);
      return out;
    } catch (const std::exception& e) {
      return fail_json(std::string("逆置換に失敗しました: ") + e.what());
    }
  }

  // ④' 逆置換（セル）。AI が返してきた表をそのまま戻す用。各セルを独立に逆置換する。
  // {ok, cells:[{r,c,text}]（変わったセルだけ）, leftovers:[...]}
  json reverse_cells(const std::vector<Cell>& cells, const std::string& mapping_body,
                     std::string_view passphrase = {}) {
    std::vector<mapping_io::Entry> value_token;
    json fail;
    if (!parse_mapping(mapping_body, passphrase, value_token, fail)) return fail;
    try {
      tokenizer::Tokenizer tok;
      tok.load_mapping(value_token);
      json out_cells = json::array();
      std::vector<std::string> leftovers;
      for (const auto& c : cells) {
        const std::string src = utf8::repair(c.text);
        const std::string restored = tok.reverse(src);
        for (const auto& t : tokenizer::find_unreplaced_tokens(restored))
          if (std::find(leftovers.begin(), leftovers.end(), t) == leftovers.end())
            leftovers.push_back(t);
        if (restored != src) out_cells.push_back({{"r", c.row}, {"c", c.col}, {"text", restored}});
      }
      json out;
      out["ok"] = true;
      out["cells"] = out_cells;
      out["leftovers"] = leftovers;
      return out;
    } catch (const std::exception& e) {
      return fail_json(std::string("逆置換に失敗しました: ") + e.what());
    }
  }

  /// 読み込み済みのセル・候補・**対応表**を消す。対応表は最も機微な成果物なので確実に落とす。
  void clear() {
    cells_.clear();
    last_candidates_.clear();
    last_mapping_.clear();
  }

 private:
  json err_json() const {
    json out;
    out["ok"] = false;
    out["error"] = err_.empty() ? "初期化に失敗しました" : err_;
    return out;
  }

  static json fail_json(const std::string& message) {
    json out;
    out["ok"] = false;
    out["error"] = message;
    return out;
  }

  /// 対応表の解釈は mapping_io（GUI/CLI と同じ読み手）。パスフレーズ付きなら
  /// needsPassphrase を立てて返し、呼び出し側に入力欄を出させる。
  static bool parse_mapping(const std::string& body, std::string_view passphrase,
                            std::vector<mapping_io::Entry>& out, json& fail) {
    try {
      out = mapping_io::parse(body, passphrase);
      return true;
    } catch (const mapping_io::NeedsPassphrase& e) {
      fail = fail_json(e.what());
      fail["needsPassphrase"] = true;
      return false;
    } catch (const std::exception& e) {
      fail = fail_json(e.what());  // 行番号つきの書式エラーをそのまま見せる
      return false;
    }
  }

  static std::vector<std::string> split_cells(const std::string& s) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
      const auto p = s.find(CELL_SEP, start);
      if (p == std::string::npos) {
        parts.push_back(s.substr(start));
        return parts;
      }
      parts.push_back(s.substr(start, p - start));
      start = p + 1;
    }
  }

  // ---- 列指定の適用 ----
  // 1 回のプレビュー／サニタイズごとに「各セルをどう扱うか」を決める（Plan）。列指定は
  // load ではなく preview/sanitize に乗る: 列の扱いを変えるたびに NER を回し直さないため。

  enum class CellMode : char { Normal, Whole, Excluded };

  struct Plan {
    std::vector<CellMode> mode;
    std::vector<std::string> kind;   // Whole のときの種別
    std::vector<std::string> value;  // Whole のときの値（前後の空白を除いたもの）
    std::vector<std::size_t> lead;   // value の前にある空白の文字数（スパンのオフセット用）
  };

  Plan make_plan(const std::vector<ColumnRule>& rules) const {
    for (const auto& r : rules)
      if (!is_column_kind(r.kind)) throw std::runtime_error("列指定の種別が不正です: " + r.kind);
    Plan p;
    const std::size_t n = cells_.size();
    p.mode.assign(n, CellMode::Normal);
    p.kind.assign(n, std::string());
    p.value.assign(n, std::string());
    p.lead.assign(n, 0);
    if (rules.empty()) return p;
    for (std::size_t i = 0; i < n; ++i) {
      const Cell& c = cells_[i];
      const ColumnRule* hit = nullptr;
      for (const auto& r : rules)
        if (r.col == c.col && r.r1 <= c.row && c.row <= r.r2) hit = &r;  // 後勝ち
      if (!hit) continue;
      if (hit->kind == "NONE") {
        p.mode[i] = CellMode::Excluded;
        continue;
      }
      const std::string v = utf8::trim(c.text);  // load で空白だけのセルは落としてあるので非空
      bool skipped = tokenizer::symbol_only(v);  // 「-」「―」のような空欄印はトークンにしない
      for (const auto& s : hit->skip)
        if (utf8::trim(s) == v) { skipped = true; break; }
      if (skipped) continue;
      p.mode[i] = CellMode::Whole;
      p.kind[i] = hit->kind;
      p.value[i] = v;
      const auto pos = c.text.find(v);  // 空白を剥いだ値は原文に必ず 1 回以上ある
      p.lead[i] = utf8::char_len(c.text.substr(0, pos == std::string::npos ? 0 : pos));
    }
    return p;
  }

  /// 弱い値: 1 文字、または数字（半角・全角）だけで 5 文字未満。自分のセルでは伏せるが、
  /// 他のセルへ探しに行かない（波及もゲートもしない）。「1」「0042」が日付や数量の中で当たるため。
  static bool weak_value(const std::string& v) {
    std::size_t chars = 0;
    bool digits_only = true;
    for (std::size_t i = 0; i < v.size();) {
      std::size_t n;
      const char32_t cp = utf8::decode_cp(v, i, &n);
      i += n;
      ++chars;
      if (!((cp >= '0' && cp <= '9') || (cp >= 0xFF10 && cp <= 0xFF19))) digits_only = false;
    }
    return chars <= 1 || (digits_only && chars < 5);
  }

  /// 列指定の値の集合: 波及させる値（strong）と、自分のセルでだけ伏せる値（weak）。
  /// confirmed に載っている語はアドインが辞書として扱っているので、どちらにも入れない。
  struct ColumnTerms {
    std::vector<tokenizer::ConfirmedTerm> strong;  // whole_word=true で confirmed に足す
    std::vector<std::string> weak;                 // ゲートの総当たりから外す
  };

  static ColumnTerms column_terms(const Plan& plan, const std::vector<tokenizer::ConfirmedTerm>& confirmed) {
    ColumnTerms ct;
    std::set<std::string> seen;
    for (const auto& t : confirmed) seen.insert(t.text);
    for (std::size_t i = 0; i < plan.mode.size(); ++i) {
      if (plan.mode[i] != CellMode::Whole) continue;
      const std::string& v = plan.value[i];
      if (!seen.insert(v).second) continue;
      if (weak_value(v)) ct.weak.push_back(v);
      else ct.strong.push_back({v, plan.kind[i], true});
    }
    return ct;
  }

  /// confirmed ＋ 列指定の強い値（語として照合）。
  static std::vector<tokenizer::ConfirmedTerm> with_column_terms(
      const std::vector<tokenizer::ConfirmedTerm>& confirmed, const ColumnTerms& ct) {
    std::vector<tokenizer::ConfirmedTerm> all = confirmed;
    all.insert(all.end(), ct.strong.begin(), ct.strong.end());
    return all;
  }

  /// 安全ゲート: 対応表の値のうち、列指定の強い値は語境界つき、弱い値は検査しない、
  /// それ以外（確定語・番号系）は従来の部分一致。
  static std::vector<std::string> gate(const std::string& text,
                                       const std::vector<std::pair<std::string, std::string>>& mapping,
                                       const ColumnTerms& ct) {
    std::set<std::string> strong, weak;
    for (const auto& t : ct.strong) strong.insert(t.text);
    for (const auto& v : ct.weak) weak.insert(v);
    std::vector<std::string> loose, bounded;
    for (const auto& [v, t] : mapping) {
      if (weak.count(v)) continue;
      (strong.count(v) ? bounded : loose).push_back(v);
    }
    auto leaks = tokenizer::safety_gate(text, loose);
    for (const auto& v : tokenizer::safety_gate_whole(text, bounded)) leaks.push_back(v);
    return leaks;
  }

  /// 照合に掛ける作業テキスト。番兵の位置は mask_text_ と同じ（セル番号を保つ）。
  struct Work {
    std::string text;
    std::vector<std::size_t> begin;  // 各セルの文字オフセット
    std::vector<std::size_t> len;    // 各セルの文字数
  };

  /// tok が null ならプレビュー用（Whole/Excluded のセルは空）。tok があればサニタイズ用で、
  /// Whole のセルは前後の空白を残してトークンに置き換えた形になる。Excluded は空。
  Work build_work(const Plan& plan, tokenizer::Tokenizer* tok) const {
    Work w;
    std::size_t pos = 0;
    for (std::size_t i = 0; i < cells_.size(); ++i) {
      if (i) {
        w.text += CELL_SEP;
        ++pos;
      }
      std::string piece;
      switch (plan.mode[i]) {
        case CellMode::Normal:
          piece = cells_[i].text;
          break;
        case CellMode::Excluded:
          break;
        case CellMode::Whole:
          if (tok) {
            const std::string& t = cells_[i].text;
            const auto b = t.find(plan.value[i]);
            const std::string token = tok->assign(plan.value[i], tokenizer::TOKEN_PREFIX.at(plan.kind[i]));
            piece = t.substr(0, b) + token + t.substr(b + plan.value[i].size());
          }
          break;
      }
      const std::size_t n = utf8::char_len(piece);
      w.begin.push_back(pos);
      w.len.push_back(n);
      w.text += piece;
      pos += n;
    }
    return w;
  }

  /// 通常セルのハイライト。マスクと同じマッチャを作業テキストに掛け、スパンをセル相対の
  /// 文字オフセットへ移す。セルをまたぐスパンが出たら不変条件違反なので例外
  /// （＝プレビューを出さない。ずれたハイライトを見せるより止める）。
  std::map<std::size_t, json> normal_spans(const Work& w,
                                           const std::vector<tokenizer::ConfirmedTerm>& conf) const {
    std::map<std::size_t, json> per_cell;
    if (cells_.empty()) return per_cell;
    const auto spans = tokenizer::find_mask_spans(w.text, conf, sd_.get());  // 昇順
    std::size_t i = 0;
    for (const auto& s : spans) {
      while (i + 1 < cells_.size() && w.begin[i + 1] <= s.begin) ++i;
      const std::size_t b = w.begin[i], e = b + w.len[i];
      if (s.begin < b || s.end > e)
        throw std::runtime_error("内部不整合: マスク範囲がセル境界をまたぎました");
      auto it = per_cell.find(i);
      if (it == per_cell.end()) it = per_cell.emplace(i, json::array()).first;
      it->second.push_back({s.begin - b, s.end - b, s.category, s.term});
    }
    return per_cell;
  }

  json cells_with_spans(const std::map<std::size_t, json>& per_cell) const {
    json arr = json::array();
    for (const auto& [i, spans] : per_cell)
      arr.push_back({{"r", cells_[i].row}, {"c", cells_[i].col}, {"spans", spans}});
    return arr;
  }

  static std::vector<tokenizer::ConfirmedTerm> candidates_as_confirmed(
      const std::vector<step2::Candidate>& cands) {
    std::vector<tokenizer::ConfirmedTerm> conf;
    conf.reserve(cands.size());
    for (const auto& c : cands) conf.push_back({c.text, c.entity_type});
    return conf;
  }

  static json candidates_json(const std::vector<step2::Candidate>& cands) {
    json arr = json::array();
    for (const auto& c : cands)
      arr.push_back({{"text", c.text}, {"entity_type", c.entity_type}, {"count", c.count}});
    return arr;
  }

  std::string data_root_;
  bool inited_ = false;
  std::string err_;
  std::unique_ptr<step2::Patterns> patterns_;
  std::unique_ptr<sudachi::Analyzer> sd_;
  std::unique_ptr<hf::Ner> ner_;

  std::vector<Cell> cells_;  // 非空セルのみ・行→列順。照合用の連結テキストは呼び出しごとに build_work で組む
  std::vector<step2::Candidate> last_candidates_;
  std::vector<mapping_io::Entry> last_mapping_;  // (実値, トークン) 挿入順。可逆時のみ
};

}  // namespace helper
