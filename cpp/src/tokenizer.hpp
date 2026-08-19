// tokenizer.py の移植。決定論・AI不使用。
//
// **Python の len()/sort は文字（コードポイント）単位**。確定リストは「長い表記から先に置換」
// するので、バイト長で並べると順序が変わりトークン番号がズレる。utf8::char_len を使う。
#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "furigana.hpp"
#include "re.hpp"
#include "sudachi_shim.hpp"
#include "utf8.hpp"

namespace tokenizer {

/// 種別 → トークンの接頭辞。組織は会社を想定し COMPANY。
inline const std::map<std::string, std::string> TOKEN_PREFIX = {
    {"PERSON", "PERSON"}, {"ORGANIZATION", "COMPANY"}, {"LOCATION", "ADDRESS"}};

/// 不可逆（墨消し）モードの日本語ラベル。種別＋連番で社内AIが人物を追える。
inline const std::map<std::string, std::string> LABEL_JA = {
    {"PERSON", "人名"}, {"COMPANY", "会社名"}, {"ADDRESS", "住所"},
    {"EMAIL", "メール"}, {"PHONE", "電話"}, {"POSTAL", "郵便番号"}};

/// 空白・改行を無視して照合する最小文字数（PDFの行末分断対策）。
/// 短い語は誤マッチ（「山田\n中村」→「田中」）を避けるため完全一致のみ。
inline constexpr std::size_t WS_MATCH_MIN_CHARS = 4;

struct ConfirmedTerm {
  std::string text;
  std::string entity_type;
};

/// Python の re.escape 相当（PCRE2 用）。
inline std::string re_escape(const std::string& s) {
  static const std::string special = R"(\^$.|?*+()[]{}-#&~)";
  std::string out;
  for (char c : s) {
    if (static_cast<unsigned char>(c) < 0x80 && special.find(c) != std::string::npos) out += '\\';
    out += c;
  }
  return out;
}

/// 1コードポイントが Python の str.isspace() で真か。
inline bool is_space_char(const std::string& c) {
  return furigana::is_space_cp(c, 0, c.size());
}

/// 確定語を「文字間の空白・改行を無視して照合する」正規表現にする。
/// 例「株式会社サンプル商事」→ 本文の「株式会社サン\nプル商事」にもマッチ。
inline std::string ws_insensitive_pattern(const std::string& term) {
  const auto off = utf8::char_offsets(term);
  std::vector<std::string> chars;
  for (std::size_t i = 0; i + 1 < off.size(); ++i) {
    const auto c = utf8::slice(term, off, i, i + 1);
    if (!is_space_char(c)) chars.push_back(re_escape(c));
  }
  std::string pat;
  for (std::size_t i = 0; i < chars.size(); ++i) {
    if (i) pat += R"(\s*)";
    pat += chars[i];
  }
  return pat;
}

/// 空白を除いた芯（Python の "".join(ch for ch in t if not ch.isspace())）。
inline std::string strip_spaces(const std::string& s) {
  const auto off = utf8::char_offsets(s);
  std::string out;
  for (std::size_t i = 0; i + 1 < off.size(); ++i) {
    const auto c = utf8::slice(s, off, i, i + 1);
    if (!is_space_char(c)) out += c;
  }
  return out;
}

/// 確定リスト＋正規表現でテキストをマスクする。対応表を保持する。
class Tokenizer {
 public:
  explicit Tokenizer(bool reversible = true) : reversible_(reversible) {}

  const std::map<std::string, std::string>& mapping() const { return mapping_; }
  /// 挿入順の対応表（Python の dict は順序保持。逆置換や表示で順序を合わせるのに使う）。
  const std::vector<std::pair<std::string, std::string>>& mapping_ordered() const {
    return order_;
  }

  std::string assign(const std::string& value, const std::string& kind) {
    const auto it = mapping_.find(value);
    if (it != mapping_.end()) return it->second;  // 既出の値は同じもの（一貫性）
    const int n = ++counter_[kind];
    const std::string ph = placeholder(kind, n);
    mapping_[value] = ph;
    order_.emplace_back(value, ph);
    return ph;
  }

  /// 外部の対応表（値→トークンの並び）を読み込む。逆置換(restore)用で、
  /// Python の restore_ui が `tok.mapping[original] = token` で対応表を直接組むのに相当する。
  /// 既存の状態は置き換える。reverse() は order_ を見るので、これで逆置換が効く。
  void load_mapping(const std::vector<std::pair<std::string, std::string>>& value_token) {
    mapping_.clear();
    order_.clear();
    for (const auto& [value, token] : value_token) {
      mapping_[value] = token;
      order_.emplace_back(value, token);
    }
  }

  /// 確定リスト（辞書）→ 正規表現 の順でトークン化する。
  ///
  /// 番号類（メール/電話/郵便）を**先に**処理する。境界ノイズの名前候補（「8912\n所在地」）が
  /// 電話の一部を先に奪って番号を中途半端に壊すのを防ぐため。
  std::string tokenize(const std::string& text_in, const std::vector<ConfirmedTerm>& confirmed_in,
                       sudachi::Analyzer* sd = nullptr, bool link_furigana = true) {
    std::string text = text_in;
    std::vector<ConfirmedTerm> confirmed = confirmed_in;
    std::map<std::string, std::string> aliases;

    if (link_furigana && sd) {
      // 確定した漢字氏名の読み（カナ/かな）を同一トークンに寄せる（方法A）。
      const auto base = confirmed;  // 走査中に append するのでコピーを回す
      for (const auto& t : base) {
        if (t.entity_type != "PERSON") continue;
        const auto ait = aliases.find(t.text);
        const std::string canonical = (ait != aliases.end()) ? ait->second : t.text;
        for (const auto& r : furigana::readings(*sd, t.text)) {
          confirmed.push_back({r, "PERSON"});
          aliases[r] = canonical;  // 読み → 漢字名（同一トークン）
        }
      }
    }

    text = sub_all(email_, text, "EMAIL");
    text = sub_all(phone_, text, "PHONE");
    text = sub_all(postal_, text, "POSTAL");

    // 長い表記から先に置換（「田中一郎」を「田中」より先に）。
    // **Python の sorted は安定**なので、同じ長さは元の順序を保つ。std::stable_sort を使う。
    std::stable_sort(confirmed.begin(), confirmed.end(),
                     [](const ConfirmedTerm& a, const ConfirmedTerm& b) {
                       return utf8::char_len(a.text) > utf8::char_len(b.text);
                     });

    for (const auto& term : confirmed) {
      const auto pit = TOKEN_PREFIX.find(term.entity_type);
      const std::string prefix = (pit != TOKEN_PREFIX.end()) ? pit->second : "MASKED";
      const auto ait = aliases.find(term.text);
      const std::string canonical = (ait != aliases.end()) ? ait->second : term.text;
      const std::string core = strip_spaces(term.text);

      if (term.text.empty()) continue;
      std::vector<std::pair<std::size_t, std::size_t>> hits;
      if (utf8::char_len(core) >= WS_MATCH_MIN_CHARS) {
        re::Regex pat(ws_insensitive_pattern(term.text));
        for (const auto& m : pat.finditer(text)) hits.emplace_back(m.begin, m.end);
      } else {
        std::size_t pos = 0, p;
        while ((p = text.find(term.text, pos)) != std::string::npos) {
          hits.emplace_back(p, p + term.text.size());
          pos = p + term.text.size();
        }
      }
      // 既に挿入したトークンの内側にマッチしたものは捨てる。捨てないと、短い確定語
      // （手動追記の "ON" 等）が {{PERSON_1}} の内部に当たってトークンを壊し、逆置換が
      // できなくなる。**置換対象が全滅したら assign もしない**（トークン番号を進めない）。
      hits = drop_inside_placeholders(text, hits);
      if (hits.empty()) continue;
      const std::string token = assign(canonical, prefix);
      text = splice(text, hits, token);
    }
    return text;
  }

  /// 対応表でトークンを実名に戻す（AI不使用の単純置換）。
  /// 長いトークンから先に（{{PERSON_1}} と {{PERSON_10}} の取り違え防止）。
  std::string reverse(const std::string& text_in) const {
    std::vector<std::pair<std::string, std::string>> inv;  // token → value
    for (const auto& [value, token] : order_) inv.emplace_back(token, value);
    std::stable_sort(inv.begin(), inv.end(), [](const auto& a, const auto& b) {
      return utf8::char_len(a.first) > utf8::char_len(b.first);
    });
    std::string text = text_in;
    for (const auto& [token, value] : inv) text = replace_literal(text, token, value);
    return text;
  }

 private:
  std::string placeholder(const std::string& kind, int n) const {
    if (reversible_) return "{{" + kind + "_" + std::to_string(n) + "}}";
    const auto it = LABEL_JA.find(kind);
    return "[" + (it != LABEL_JA.end() ? it->second : kind) + std::to_string(n) + "]";
  }

  /// 現在のテキスト中の「挿入済みトークン」の位置（可逆 {{X_n}} / 不可逆 [ラベルn]）。
  std::vector<std::pair<std::size_t, std::size_t>> placeholder_spans(const std::string& s) const {
    static const re::Regex rev{R"(\{\{[A-Z]+_\d+\}\})"};
    static const re::Regex irr{R"(\[(?:人名|会社名|住所|メール|電話|郵便番号)\d+\])"};
    std::vector<std::pair<std::size_t, std::size_t>> out;
    for (const auto& m : (reversible_ ? rev : irr).finditer(s)) out.emplace_back(m.begin, m.end);
    return out;
  }

  /// トークンの内側に食い込むマッチを落とす。
  std::vector<std::pair<std::size_t, std::size_t>> drop_inside_placeholders(
      const std::string& s, const std::vector<std::pair<std::size_t, std::size_t>>& hits) const {
    if (hits.empty()) return hits;
    const auto prot = placeholder_spans(s);
    if (prot.empty()) return hits;
    std::vector<std::pair<std::size_t, std::size_t>> out;
    for (const auto& h : hits) {
      bool clash = false;
      for (const auto& p : prot)
        if (!(h.second <= p.first || h.first >= p.second)) { clash = true; break; }
      if (!clash) out.push_back(h);
    }
    return out;
  }

  /// `spans`（昇順・非重複）を `to` で置き換える。
  static std::string splice(const std::string& s,
                            const std::vector<std::pair<std::size_t, std::size_t>>& spans,
                            const std::string& to) {
    std::string out;
    std::size_t prev = 0;
    for (const auto& [b, e] : spans) {
      if (b < prev) continue;
      out += s.substr(prev, b - prev);
      out += to;
      prev = e;
    }
    out += s.substr(prev);
    return out;
  }

  static std::string replace_literal(const std::string& s, const std::string& from,
                                     const std::string& to) {
    if (from.empty()) return s;
    std::string out;
    std::size_t pos = 0, p;
    while ((p = s.find(from, pos)) != std::string::npos) {
      out += s.substr(pos, p - pos);
      out += to;
      pos = p + from.size();
    }
    out += s.substr(pos);
    return out;
  }

  static std::string replace_matches(const re::Regex& rx, const std::string& s,
                                     const std::string& to) {
    std::string out;
    std::size_t prev = 0;
    for (const auto& m : rx.finditer(s)) {
      out += s.substr(prev, m.begin - prev);
      out += to;
      prev = m.end;
    }
    out += s.substr(prev);
    return out;
  }

  std::string sub_all(const re::Regex& rx, const std::string& s, const std::string& kind) {
    std::string out;
    std::size_t prev = 0;
    for (const auto& m : rx.finditer(s)) {
      out += s.substr(prev, m.begin - prev);
      out += assign(m.text, kind);
      prev = m.end;
    }
    out += s.substr(prev);
    return out;
  }

  bool reversible_;
  std::map<std::string, int> counter_;
  std::map<std::string, std::string> mapping_;
  std::vector<std::pair<std::string, std::string>> order_;  // 挿入順
  // パターンは Python 側と同一（phase2_regex で 240/240 実測）。
  re::Regex email_{R"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})"};
  re::Regex phone_{R"((?<![\d\-])(?:0\d{1,4}-\d{1,4}-\d{3,4}|0\d{9,10})(?![\d\-]))"};
  re::Regex postal_{R"((?<![\d\-])\d{3}-\d{4}(?![\d\-]))"};
};

/// トークンの形（{{PREFIX_n}}）。逆置換後の残骸検査に使う。
inline std::vector<std::string> find_unreplaced_tokens(const std::string& text) {
  static const re::Regex rx{R"(\{\{[A-Z]+_\d+\}\})"};
  std::vector<std::string> out;
  for (const auto& m : rx.finditer(text)) out.push_back(m.text);
  return out;
}

/// AIに渡す直前の検査（step6.safety_gate の移植）。
///
/// 対応表の元値が本文に直接残っていないか＋メール/電話/郵便の regex 残りを返す。空なら安全。
inline std::vector<std::string> safety_gate(const std::string& tokenized,
                                            const std::vector<std::string>& raw_values) {
  static const re::Regex email{R"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})"};
  static const re::Regex phone{R"((?<![\d\-])(?:0\d{1,4}-\d{1,4}-\d{3,4}|0\d{9,10})(?![\d\-]))"};
  static const re::Regex postal{R"((?<![\d\-])\d{3}-\d{4}(?![\d\-]))"};
  // 挿入済みトークンの**内側**は「残存」ではない。短いラテン語の確定語（"ON" 等）は
  // {{PERSON_1}} の中に文字列として現れるため、素の find では偽陽性になる。
  static const re::Regex ph{
      R"(\{\{[A-Z]+_\d+\}\}|\[(?:人名|会社名|住所|メール|電話|郵便番号)\d+\])"};
  std::vector<std::pair<std::size_t, std::size_t>> prot;
  for (const auto& m : ph.finditer(tokenized)) prot.emplace_back(m.begin, m.end);
  const auto inside = [&](std::size_t b, std::size_t e) {
    for (const auto& p : prot)
      if (b >= p.first && e <= p.second) return true;  // 完全にトークンの中
    return false;
  };

  std::vector<std::string> leaks;
  for (const auto& v : raw_values) {
    if (v.empty()) continue;
    for (std::size_t pos = 0, p; (p = tokenized.find(v, pos)) != std::string::npos;
         pos = p + v.size()) {
      if (!inside(p, p + v.size())) {  // 1つでも外に出ていれば漏れ
        leaks.push_back(v);
        break;
      }
    }
  }
  for (const auto* rx : {&email, &phone, &postal})
    for (const auto& m : rx->finditer(tokenized))
      if (!inside(m.begin, m.end)) leaks.push_back(m.text);
  return leaks;
}

// ハイライト用カテゴリ（entity_type → 表示カテゴリ）。_HL_CATEGORY の移植。
inline std::string hl_category(const std::string& entity_type) {
  if (entity_type == "PERSON") return "person";
  if (entity_type == "ORGANIZATION") return "company";
  if (entity_type == "LOCATION") return "address";
  return "company";  // 既定
}

struct MaskSpan {
  std::size_t begin, end;  // 文字（コードポイント）オフセット。Python の find_mask_spans と同じ
  std::string category;    // person/company/address/email/phone/postal
};

/// マスク対象箇所を (開始, 終了, カテゴリ) で返す（find_mask_spans の移植・重なりなし）。
///
/// tokenize と同じ照合（番号regex→確定リスト長い順・空白無視マッチ・フリガナ読み）で走査するが
/// 置換せずスパンを集める。overlap/sort は byte 空間で計算しても単調写像で保存されるので、
/// 最後に文字オフセットへ変換する（Python は文字単位）。
inline std::vector<MaskSpan> find_mask_spans(const std::string& text,
                                             const std::vector<ConfirmedTerm>& confirmed_in,
                                             sudachi::Analyzer* sd = nullptr,
                                             bool link_furigana = true) {
  std::vector<ConfirmedTerm> confirmed = confirmed_in;
  if (link_furigana && sd) {
    const auto base = confirmed;
    for (const auto& t : base)
      if (t.entity_type == "PERSON")
        for (const auto& r : furigana::readings(*sd, t.text))
          confirmed.push_back({r, "PERSON"});
  }

  // (begin, end) はバイト位置で保持。
  std::vector<std::pair<std::size_t, std::size_t>> taken;
  std::vector<std::tuple<std::size_t, std::size_t, std::string>> spans;  // byte
  auto overlaps = [&](std::size_t s, std::size_t e) {
    for (const auto& [ts, te] : taken)
      if (!(e <= ts || s >= te)) return true;
    return false;
  };
  auto add = [&](std::size_t s, std::size_t e, const std::string& cat) {
    if (!overlaps(s, e)) {
      taken.emplace_back(s, e);
      spans.emplace_back(s, e, cat);
    }
  };

  // 番号類を先に（tokenize と同じ優先順位）
  static const re::Regex email{R"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})"};
  static const re::Regex phone{R"((?<![\d\-])(?:0\d{1,4}-\d{1,4}-\d{3,4}|0\d{9,10})(?![\d\-]))"};
  static const re::Regex postal{R"((?<![\d\-])\d{3}-\d{4}(?![\d\-]))"};
  for (const auto& [rx, cat] :
       {std::pair<const re::Regex*, const char*>{&email, "email"},
        {&phone, "phone"},
        {&postal, "postal"}})
    for (const auto& m : rx->finditer(text)) add(m.begin, m.end, cat);

  // 確定リスト（長い順・**文字長**で安定ソート）
  std::stable_sort(confirmed.begin(), confirmed.end(),
                   [](const ConfirmedTerm& a, const ConfirmedTerm& b) {
                     return utf8::char_len(a.text) > utf8::char_len(b.text);
                   });
  for (const auto& term : confirmed) {
    if (term.text.empty()) continue;
    const std::string cat = hl_category(term.entity_type);
    const std::string core = strip_spaces(term.text);
    if (utf8::char_len(core) >= WS_MATCH_MIN_CHARS) {
      re::Regex pat(ws_insensitive_pattern(term.text));
      for (const auto& m : pat.finditer(text)) add(m.begin, m.end, cat);
    } else {
      std::size_t start = 0, i;
      while ((i = text.find(term.text, start)) != std::string::npos) {
        add(i, i + term.text.size(), cat);
        start = i + term.text.size();
      }
    }
  }

  std::sort(spans.begin(), spans.end());
  // byte → 文字オフセットに変換
  const auto off = utf8::char_offsets(text);
  auto b2c = [&](std::size_t b) {
    return static_cast<std::size_t>(std::lower_bound(off.begin(), off.end(), b) - off.begin());
  };
  std::vector<MaskSpan> out;
  for (const auto& [b, e, cat] : spans) out.push_back({b2c(b), b2c(e), cat});
  return out;
}

}  // namespace tokenizer
