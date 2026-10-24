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

    // マスク用テキスト（番兵連結）と、各セルの文字オフセット。
    std::size_t pos = 0;
    for (std::size_t i = 0; i < cells_.size(); ++i) {
      if (i) {
        mask_text_ += CELL_SEP;
        ++pos;
      }
      const std::size_t n = utf8::char_len(cells_[i].text);
      cell_begin_.push_back(pos);
      cell_len_.push_back(n);
      mask_text_ += cells_[i].text;
      pos += n;
    }

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
      } catch (const std::exception& e) {
        // 検知が落ちても読み込んだセルは残す（番号系の自動マスクと手動追記は使える）。
        warnings.push_back(std::string("検知に失敗しました（") + e.what() + "）");
      }
    }

    json out;
    out["ok"] = true;
    out["cell_count"] = cells_.size();
    out["candidates"] = candidates_json(last_candidates_);
    try {
      out["cells"] = cells_spans_json(candidates_as_confirmed(last_candidates_));
    } catch (const std::exception& e) {
      return fail_json(std::string("プレビューの生成に失敗しました: ") + e.what());
    }
    out["warnings"] = warnings;
    return out;
  }

  // ② プレビュー更新（opt-out を反映して再ハイライト）。返り値: {ok, cells:[{r,c,spans}]}
  json preview(const std::vector<tokenizer::ConfirmedTerm>& confirmed) {
    if (!ensure_init()) return err_json();
    try {
      json out;
      out["ok"] = true;
      out["cells"] = cells_spans_json(confirmed);
      return out;
    } catch (const std::exception& e) {
      return fail_json(std::string("プレビューの更新に失敗しました: ") + e.what());
    }
  }

  // ③ サニタイズ。返り値: {ok, cells:[{r,c,text}], cell_count, changed, mapping_count,
  //                        reversible, leaks:[...]}
  // cells に載るのは**値が変わったセルだけ**。アドインは手元の値をこれで上書きして別ブックを作る。
  json sanitize(const std::vector<tokenizer::ConfirmedTerm>& confirmed, bool reversible) {
    if (!ensure_init()) return err_json();
    if (cells_.empty()) return fail_json("読み込まれたセルがありません。");
    last_mapping_.clear();
    try {
      tokenizer::Tokenizer tok(reversible);
      const std::string sanitized = tok.tokenize(mask_text_, confirmed, sd_.get());

      std::vector<std::string> raw;
      for (const auto& [v, t] : tok.mapping_ordered()) raw.push_back(v);
      const auto leaks = tokenizer::safety_gate(sanitized, raw);

      // 番兵で割って元のセルへ戻す。数が合わなければ不変条件が破れている——黙って
      // ずらして書き戻すと、別のセルに別人の値が入る最悪の形になるので、ここで止める。
      const auto parts = split_cells(sanitized);
      if (parts.size() != cells_.size())
        return fail_json("内部不整合: サニタイズ後のセル数が一致しません（" +
                         std::to_string(parts.size()) + " / " + std::to_string(cells_.size()) + "）");

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
    mask_text_.clear();
    cell_begin_.clear();
    cell_len_.clear();
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

  /// 確定リストでハイライトするセル一覧。マスクと同じマッチャを番兵連結テキストに掛け、
  /// スパンをセル相対の文字オフセットへ移す。セルをまたぐスパンが出たら不変条件違反
  /// なので例外（＝プレビューを出さない。ずれたハイライトを見せるより止める）。
  json cells_spans_json(const std::vector<tokenizer::ConfirmedTerm>& conf) const {
    json arr = json::array();
    if (cells_.empty()) return arr;
    const auto spans = tokenizer::find_mask_spans(mask_text_, conf, sd_.get());  // 昇順
    std::size_t i = 0;
    json cur;
    bool have_cur = false;
    std::size_t cur_i = 0;
    for (const auto& s : spans) {
      while (i + 1 < cells_.size() && cell_begin_[i + 1] <= s.begin) ++i;
      const std::size_t b = cell_begin_[i], e = b + cell_len_[i];
      if (s.begin < b || s.end > e)
        throw std::runtime_error("内部不整合: マスク範囲がセル境界をまたぎました");
      if (!have_cur || cur_i != i) {
        if (have_cur) arr.push_back(std::move(cur));
        cur = json{{"r", cells_[i].row}, {"c", cells_[i].col}, {"spans", json::array()}};
        have_cur = true;
        cur_i = i;
      }
      cur["spans"].push_back({s.begin - b, s.end - b, s.category, s.term});
    }
    if (have_cur) arr.push_back(std::move(cur));
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

  std::vector<Cell> cells_;              // 非空セルのみ・行→列順
  std::string mask_text_;                // cells_ を CELL_SEP で連結したもの
  std::vector<std::size_t> cell_begin_;  // 各セルの mask_text_ 内 文字オフセット
  std::vector<std::size_t> cell_len_;    // 各セルの文字数
  std::vector<step2::Candidate> last_candidates_;
  std::vector<mapping_io::Entry> last_mapping_;  // (実値, トークン) 挿入順。可逆時のみ
};

}  // namespace helper
