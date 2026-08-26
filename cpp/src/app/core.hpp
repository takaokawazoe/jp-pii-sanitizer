// Phase 5: WebView2 UI から呼ぶコア facade。
//
// app.py（Streamlit）の画面①〜④のバックエンドを C++ コアへ配線する薄い層。
// UI（winmain.cpp）は JSON メッセージでこの facade を叩き、facade は
// step2/hf_ner/tokenizer/step5/extractors を組み合わせて JSON を返す。
//
// この段の割り切り（最小疎通の次段）:
//   - .msg は対応済み（Aspose は app/msg_bridge.cpp に隔離）。**添付の中身は展開しない**
//     ——添付は利用者が自分で取り出して読み込む方針（docs/cli.md の設計判断）。
//   - .eml は未対応（C++ に MIME 抽出器が無い）。
//   - csv は列指定 UI を省き prose 扱い（Python は既定データ表だが、まず end-to-end 優先）。
// 検知・マスク・逆置換のロジック自体は Phase 0-5 で Python と一致済み（10/10 green）。
#pragma once

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "extractors.hpp"
#include "file_io.hpp"
#include "hf_ner.hpp"
#include "json.hpp"
#include "app/msg_bridge.hpp"
#include "mapping_io.hpp"
#include "ooxml.hpp"
#include "pdf.hpp"
#include "step2.hpp"
#include "step5.hpp"
#include "sudachi_shim.hpp"
#include "tokenizer.hpp"
#include "utf8.hpp"

namespace appcore {

using nlohmann::json;

inline std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

inline std::string ext_of(const std::string& path) {
  const auto dot = path.find_last_of('.');
  return dot == std::string::npos ? "" : to_lower(path.substr(dot + 1));
}

inline std::string basename_of(const std::string& path) {
  const auto slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// find_mask_spans の結果を [[begin,end,category], ...] にする。
inline json spans_to_json(const std::vector<tokenizer::MaskSpan>& spans) {
  json arr = json::array();
  for (const auto& s : spans) arr.push_back({s.begin, s.end, s.category});
  return arr;
}

/// UI 一セッション分の状態を持つ facade。Streamlit の session_state 相当。
class Core {
 public:
  // data_root は models/（model_fp16.onnx・tokenizer.json・labels.json・patterns.json）と
  // sudachi/（resources・system.dic）を含むディレクトリ。exe から解決して渡す。
  explicit Core(std::string data_root) : data_root_(std::move(data_root)) {}

  bool ready() const { return inited_; }
  const std::string& last_error() const { return err_; }

  // モデル・辞書・パターンを読み込む（重い・初回のみ）。成否を返す。
  bool ensure_init() {
    if (inited_) return true;
    try {
      // patterns.json → 正規表現 8 本 ＋ ラベル写像
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

      // Sudachi（形態素）
      const std::string sres = data_root_ + "/sudachi/resources";
      sd_ = std::make_unique<sudachi::Analyzer>(sres + "/sudachi.json", sres,
                                                data_root_ + "/sudachi/system.dic");

      // HF NER（ONNX）
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

  // ① アップロード→抽出 ＋ ② 候補検知。paths は絶対パス。
  // 返り値: {ok, candidates:[{text,entity_type,count}], blocks:[{source,kind,text,spans}]}
  json extract(const std::vector<std::string>& paths) {
    if (!ensure_init()) return err_json();
    results_.clear();
    for (const auto& p : paths) {
      step5::FileResult fr;
      fr.source = basename_of(p);
      try {
        const std::string e = ext_of(p);
        if (e == "docx") { fr.kind = "docx"; fr.text = ooxml::extract_docx(p); }
        else if (e == "pptx") { fr.kind = "pptx"; fr.text = ooxml::extract_pptx(p); }
        else if (e == "xlsx") { fr.kind = "xlsx-prose"; fr.text = ooxml::read_xlsx_prose(p); }
        else if (e == "pdf") { fr.kind = "pdf"; fr.text = pdf::extract_pdf(p); }
        else if (e == "txt" || e == "csv" || e == "md") { fr.kind = "text"; fr.text = extractors::extract_plain(p); }
        else if (e == "msg") {
          // 添付は**中身を展開しない**（設計判断・docs/cli.md）。本文＋添付ファイル名までが対象で、
          // 添付を処理したい利用者は自分で保存して個別に読み込む。attachments は警告表示用。
          const auto r = msgbridge::extract(p);
          fr.kind = "msg";
          fr.text = r.text;
          fr.attachments = r.attachments;
          fr.error = r.error;
          if (!r.error.empty()) fr.kind = "skipped";
        }
        else {
          // 未対応拡張子は**理由を出す**。以前は無言で空ブロックになり、利用者からは
          // 「読み込んだのに何も出ない」としか見えなかった（.msg/.eml は CLI のみ対応）。
          fr.kind = "skipped";
          fr.text = "";
          fr.error = e.empty() ? "拡張子が判別できません" : ("未対応の形式です: ." + e);
        }
      } catch (const std::exception& e) {
        fr.kind = "skipped";
        fr.text.clear();
        // **失敗理由を text に入れないこと。** text は bundle_blocks でそのまま
        // AI 送信テキストへ束ねられるので、例外メッセージ（＝生テキストを含みうる）が
        // 本文に紛れ込む。表示用に error へ分ける。
        fr.error = e.what();
      }
      // ---- テキスト化の出口: ここから下流は「正当な UTF-8」を前提にしてよい ----
      // 入口の復号（extractors::decode_text_bytes）だけでは足りない。PDF の孤立サロゲート
      // （CESU-8 になる）や壊れた OOXML は復号を通らずここへ来る。不正なまま流すと
      // PCRE2 が黙って0件を返し（fail-open）、Rust シムは例外を投げてプロセスが落ちる。
      fr.text = utf8::repair(fr.text);
      results_.push_back(std::move(fr));
    }

    // 検知は1ファイルごとに行い候補をマージする。全ファイルを連結して1回にかけると、
    // NER の 250 文字ハードチャンク境界で語が割れて断片候補が出る（かつ結果がファイルの
    // 読み込み順・組み合わせで変わる）。文書は独立なので跨いで解析する意味も無く、
    // per-doc は検証済みオラクル（phase2_pipeline）と同じ経路になる。
    // 集約は「無空白キー×種別」で、単一バンドルにかけたときの集約と揃える。
    struct Agg { step2::Candidate cand; int best; };  // best = 表示表記を選んだファイルの出現数
    std::vector<Agg> agg;
    std::map<std::pair<std::string, std::string>, std::size_t> aidx;
    std::vector<std::string> warnings;

    // 検知に掛けるテキスト: 各ファイルの本文 ＋ **ファイル名**。
    // ファイル名を入れるのは、束ね（bundle_blocks）が【ファイル名】ブロックと
    // 複数ファイル時の【source】見出しで**ファイル名を AI 送信テキストに載せる**ため。
    // ここで候補にしないと「社員名簿_山田太郎_確認用.csv」の氏名が素通りする。
    std::string names;  // targets が指すので寿命をループの外に置く
    {
      std::set<std::string> seen;
      for (const auto& fr : results_)
        if (!fr.source.empty() && seen.insert(fr.source).second) names += fr.source + "\n";
    }
    // 本文はコピーせず参照で回す（PDF 等で数MBになるため）
    std::vector<std::pair<std::string, const std::string*>> targets;  // (表示名, テキスト)
    for (const auto& fr : results_)
      if (!fr.text.empty()) targets.emplace_back(fr.source, &fr.text);
    if (!names.empty()) targets.emplace_back("（ファイル名）", &names);

    for (const auto& [label, text] : targets) {
      std::vector<step2::Candidate> cands;
      try {
        cands = step2::extract_candidates(*patterns_, *sd_, *ner_, *text);
      } catch (const std::exception& e) {
        // 1本の失敗で全体を落とさない（以前はここが無防備で、Shift_JIS の CSV が
        // 混ざるだけでプロセスごと死に、読み込み済みの他ファイルの作業も消えた）。
        warnings.push_back(label + ": 検知に失敗しました（" + e.what() + "）");
        continue;
      }
      for (const auto& c : cands) {
        const auto key = std::make_pair(step2::strip_all_ws(*patterns_, c.text), c.entity_type);
        auto it = aidx.find(key);
        if (it == aidx.end()) {
          aidx[key] = agg.size();
          agg.push_back({c, c.count});
        } else {
          auto& a = agg[it->second];
          a.cand.count += c.count;
          // 表示表記は「出現数が多い方、同数なら短い方」（within-file の規則に合わせる）
          if (c.count > a.best ||
              (c.count == a.best && utf8::char_len(c.text) < utf8::char_len(a.cand.text))) {
            a.cand.text = c.text;
            a.best = c.count;
          }
        }
      }
    }
    // 並び順を extract_candidates と揃える（種別順 → 出現数降順 → 表記昇順）
    std::map<std::string, int> torder;
    for (std::size_t i = 0; i < step2::CANDIDATE_ENTITY_TYPES.size(); ++i)
      torder[step2::CANDIDATE_ENTITY_TYPES[i]] = static_cast<int>(i);
    last_candidates_.clear();
    for (auto& a : agg) last_candidates_.push_back(std::move(a.cand));
    std::sort(last_candidates_.begin(), last_candidates_.end(),
              [&](const step2::Candidate& x, const step2::Candidate& y) {
                const int tx = torder.count(x.entity_type) ? torder[x.entity_type] : 99;
                const int ty = torder.count(y.entity_type) ? torder[y.entity_type] : 99;
                if (tx != ty) return tx < ty;
                if (x.count != y.count) return x.count > y.count;
                return x.text < y.text;
              });

    // 初期プレビュー = 全候補をマスク対象として各ブロックをハイライト
    const auto conf = candidates_as_confirmed(last_candidates_);

    json out;
    out["ok"] = true;
    out["candidates"] = candidates_json(last_candidates_);
    try {
      out["blocks"] = blocks_json(conf, true);
    } catch (const std::exception& e) {
      return fail_json(std::string("プレビューの生成に失敗しました: ") + e.what());
    }
    out["warnings"] = warnings;
    return out;
  }

  // ② プレビュー更新（確定リストの opt-out を反映して再ハイライト）。
  // confirmed: [{text,type}] 返り値: {ok, blocks:[{source, spans}]}
  json preview(const std::vector<tokenizer::ConfirmedTerm>& confirmed) {
    if (!ensure_init()) return err_json();
    try {
      json out;
      out["ok"] = true;
      out["blocks"] = blocks_json(confirmed, false);
      return out;
    } catch (const std::exception& e) {
      return fail_json(std::string("プレビューの更新に失敗しました: ") + e.what());
    }
  }

  // ③ サニタイズ。返り値: {ok, sanitized, mapping:[{token,original}], leaks:[...]}
  json sanitize(const std::vector<tokenizer::ConfirmedTerm>& confirmed, bool reversible) {
    if (!ensure_init()) return err_json();
    // リセット後や読み込み前に実行されたら、古い results_ で束ねない。
    // （以前は空チェックが無く、UI の「リセット」が native に伝わらないため、
    //   消したはずの文書がほぼ素のまま結果画面に出ていた）
    if (results_.empty()) return fail_json("読み込まれたファイルがありません。");
    last_mapping_.clear();
    try {
      tokenizer::Tokenizer tok(reversible);
      const std::string bundle = step5::bundle_blocks(tok, results_, sd_.get());
      const std::string sanitized = tok.tokenize(bundle, confirmed, sd_.get());

      std::vector<std::string> raw;
      for (const auto& [v, t] : tok.mapping_ordered()) raw.push_back(v);
      const auto leaks = tokenizer::safety_gate(sanitized, raw);

      // 対応表は Core が持つ。保存は saveMapping で native が直接書き出すので、
      // JS 側に文字列を組み立てさせない（シリアライザは mapping_io の 1 本だけ）。
      // 不可逆モードは対応表を残さない＝ここで捨てる。
      if (reversible) last_mapping_ = tok.mapping_ordered();

      json out;
      out["ok"] = true;
      out["sanitized"] = sanitized;
      // 結果画面の表示用。書式は保存されるファイルと同一（GUI と CLI で 1 バイトも違わない）。
      out["mapping_jsonl"] = last_mapping_.empty() ? std::string() : mapping_jsonl();
      out["mapping_count"] = static_cast<int>(last_mapping_.size());
      out["reversible"] = reversible;
      out["leaks"] = leaks;             // 空なら安全ゲート通過
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

  // ④ 逆置換。text＋貼り付けられた対応表(JSONL) → {ok, restored, leftovers:[...]}
  //
  // 対応表の解釈は mapping_io に任せる。JS でパースしていた頃は GUI と CLI で読み手が
  // 分かれていて、片方で読めるものが他方で読めなかった。
  json reverse(const std::string& text, const std::string& mapping_text,
               std::string_view passphrase = {}) {
    std::vector<mapping_io::Entry> value_token;
    try {
      value_token = mapping_io::parse(mapping_text, passphrase);
    } catch (const mapping_io::NeedsPassphrase& e) {
      // パスフレーズ付きの対応表。UI に入力欄を出させて、同じ要求を再送してもらう。
      json out = fail_json(e.what());
      out["needsPassphrase"] = true;
      return out;
    } catch (const std::exception& e) {
      return fail_json(e.what());  // 行番号つきの書式エラーをそのまま見せる
    }
    try {
      tokenizer::Tokenizer tok;  // 可逆版（既定）
      tok.load_mapping(value_token);
      const std::string restored = tok.reverse(utf8::repair(text));
      const auto left = tokenizer::find_unreplaced_tokens(restored);
      json out;
      out["ok"] = true;
      out["restored"] = restored;
      out["leftovers"] = left;
      return out;
    } catch (const std::exception& e) {
      return fail_json(std::string("逆置換に失敗しました: ") + e.what());
    }
  }

  /// ファイルピッカーで選ばれた対応表を native 側で保持する。
  ///
  /// **この経路で開いた対応表は WebView に一切乗らない。** 保存側（saveMapping）を
  /// native に寄せたのと対称にするための仕組みで、逆置換のときは JS ではなくこれを使う。
  void set_picked_mapping(std::string body) { picked_mapping_ = std::move(body); }
  bool has_picked_mapping() const { return !picked_mapping_.empty(); }
  const std::string& picked_mapping() const { return picked_mapping_; }

  /// 画面の「リセット」。読み込み済みの文書・候補・**対応表**を native 側からも消す。
  /// 対応表は本ツールが作る最も機微な成果物なので、リセットで確実に落とす。
  void clear() {
    results_.clear();
    last_candidates_.clear();
    last_mapping_.clear();
    wipe_picked_mapping();
  }

  void wipe_picked_mapping() {
    if (!picked_mapping_.empty()) sodium_memzero(picked_mapping_.data(), picked_mapping_.size());
    picked_mapping_.clear();
  }

 private:
  json err_json() {
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

  /// 検知・プレビューで返すブロック一覧。
  ///
  /// 先頭は【ファイル名】ブロック（束ねが実際に AI へ送るのと同じ本文）。ここを出さないと
  /// 「送られる文面」がプレビューと食い違い、ファイル名の氏名を利用者が確認できない。
  /// `id` は results_ の位置に固定する。**source（basename）をキーにしてはいけない**——
  /// 別フォルダの同名ファイルを2本読むと衝突してハイライトが取り違わる。
  json blocks_json(const std::vector<tokenizer::ConfirmedTerm>& conf, bool with_text) {
    json blocks = json::array();
    const std::string names = step5::filenames_block(results_);
    if (!names.empty()) {
      json b;
      b["id"] = "names";
      b["source"] = "（ファイル名）";
      b["kind"] = "filenames";
      if (with_text) b["text"] = names;
      b["spans"] = spans_to_json(tokenizer::find_mask_spans(names, conf, sd_.get()));
      blocks.push_back(b);
    }
    for (std::size_t i = 0; i < results_.size(); ++i) {
      const auto& fr = results_[i];
      json b;
      b["id"] = "f" + std::to_string(i);
      b["source"] = fr.source;
      b["kind"] = fr.kind;
      if (!fr.error.empty()) b["error"] = fr.error;
      if (!fr.attachments.empty()) b["attachments"] = fr.attachments;  // 中身は未展開
      if (with_text) b["text"] = fr.text;
      b["spans"] = spans_to_json(tokenizer::find_mask_spans(fr.text, conf, sd_.get()));
      blocks.push_back(b);
    }
    return blocks;
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
  std::vector<step5::FileResult> results_;
  std::vector<step2::Candidate> last_candidates_;
  std::vector<mapping_io::Entry> last_mapping_;  // (実値, トークン) 挿入順。可逆時のみ
  std::string picked_mapping_;  // ピッカーで選ばれた対応表の中身（JS には渡さない）
};

}  // namespace appcore
