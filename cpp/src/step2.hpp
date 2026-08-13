// step2_ner_candidates.py の移植。
//
// **Python は文字（コードポイント）単位、C++ はバイト列**という差が全体に効く。
// s[a:b] / len(s) / find(sub, start, end) / strip(chars) はすべて文字単位なので、
// utf8.hpp の変換を通す。ここを怠ると日本語で静かに壊れる。
//
// Unicode 依存の判定（\s / \w / \d）は PCRE2 を UCP 付きで使い、Python re と揃える
// （Python の `\s` は全角空白に、`\d` は全角数字にマッチする。実測済み）。
#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "furigana.hpp"
#include "hf_ner.hpp"
#include "re.hpp"
#include "sudachi_shim.hpp"
#include "utf8.hpp"

namespace step2 {

inline const std::vector<std::string> CANDIDATE_ENTITY_TYPES = {"PERSON", "ORGANIZATION",
                                                                "LOCATION"};

// 会社/部署の分類マーカー。**furigana の _COMPANY_MARKERS とは別物**（あちらは ㈱/㈲ を含む）。
// Python 側も step2 と furigana で別々に持っている。
inline const std::vector<std::string> COMPANY_MARKERS = {
    "株式会社", "有限会社", "合同会社", "合名会社", "合資会社",
    "一般社団法人", "一般財団法人", "公益社団法人", "公益財団法人",
    "協同組合", "（株）", "(株)", "（有）", "(有)"};

inline const std::vector<std::string> DEPT_SUFFIXES = {"部", "課", "室", "局", "本部",
                                                       "事業部", "係"};

// 部署名は個人情報でないため既定でマスク対象外（会社名は対象）。
inline constexpr bool MASK_DEPARTMENTS = false;

/// 社名トリムで名前の切れ目とみなす区切り文字。
inline bool is_org_delim(const std::string& ch) {
  static const std::vector<std::string> d = {
      " ", "\t", "\n", "\r", "　", "、", "。", "，", ",", "／", "/", "（", "）", "(", ")",
      "「", "」", "『", "』", "【", "】", "《", "》", "〈", "〉", "×", "＊", "*", "：", ":",
      "；", ";"};
  return std::find(d.begin(), d.end(), ch) != d.end();
}

/// 共有の正規表現。パターンは Python 側と同一の文字列を渡す（phase2_regex で 240/240 実測）。
struct Patterns {
  re::Regex company_mae, company_ato, address, email, phone, postal, pref, pii_split;
  re::Regex has_letter{R"([^\W\d_])"};                       // 文字（Unicode）を含むか
  re::Regex person_addr_noise{R"([0-9０-９]|丁目|番地)"};      // 人名の住所誤ラベル判定
  re::Regex loc_digit{R"([0-9０-９〇一二三四五六七八九十百千])"};  // 住所の数字フィルタ
  re::Regex ws{R"(\s)"};                                     // Unicode 空白（全角含む）
  re::Regex org_head_junk{R"(^[\d\s\-−ー－,.、／/¥￥%（）()]+)"};

  Patterns(const std::map<std::string, std::string>& p)
      : company_mae(p.at("company_mae")), company_ato(p.at("company_ato")),
        address(p.at("address")), email(p.at("email")), phone(p.at("phone")),
        postal(p.at("postal")), pref(p.at("pref")), pii_split(p.at("pii_split")) {}
};

inline bool has_letter(const Patterns& P, const std::string& s) {
  return !P.has_letter.finditer(s).empty();
}

/// Python の re.sub(r"\s", "", s) 相当（Unicode 空白を全部落とす）。
inline std::string strip_all_ws(const Patterns& P, const std::string& s) {
  std::string out;
  std::size_t prev = 0;
  for (const auto& m : P.ws.finditer(s)) {
    out += s.substr(prev, m.begin - prev);
    prev = m.end;
  }
  out += s.substr(prev);
  return out;
}

/// Python の str.strip(chars) 相当（chars はコードポイントの集合）。
inline std::string strip_set(const std::string& s, const std::vector<std::string>& chars) {
  const auto off = utf8::char_offsets(s);
  std::size_t b = 0, e = off.size() - 1;
  auto in = [&](std::size_t i) {
    const auto c = utf8::slice(s, off, i, i + 1);
    return std::find(chars.begin(), chars.end(), c) != chars.end();
  };
  while (b < e && in(b)) ++b;
  while (e > b && in(e - 1)) --e;
  return utf8::slice(s, off, b, e);
}

inline bool contains(const std::string& s, const std::string& sub) {
  return s.find(sub) != std::string::npos;
}

inline bool ends_with(const std::string& s, const std::string& suf) {
  return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

/// 組織名が（会社でなく）部署とみなせるか。会社マーカーがあれば会社扱い。
inline bool is_department(const std::string& name) {
  for (const auto& m : COMPANY_MARKERS)
    if (contains(name, m)) return false;
  for (const auto& d : DEPT_SUFFIXES)
    if (ends_with(name, d)) return true;
  return false;
}

/// メール/電話/郵便/会社マーカーのうち最初に現れる手前まで（文字単位）。
inline std::string cut_before_pii(const Patterns& P, const std::string& s) {
  const auto off = utf8::char_offsets(s);
  std::size_t cut = s.size();  // バイト位置で持つ
  for (const auto* rx : {&P.email, &P.phone, &P.postal}) {
    const auto ms = rx->finditer(s);
    if (!ms.empty() && ms[0].begin < cut) cut = ms[0].begin;
  }
  for (const auto& marker : COMPANY_MARKERS) {
    const auto i = s.find(marker);
    if (i != std::string::npos && i < cut) cut = i;
  }
  return furigana::trim(s.substr(0, cut));
}

/// 住所スパンから住所断片を取り出す（複数可）。Python の _trim_location。
inline std::vector<std::string> trim_location(const Patterns& P, const std::string& s) {
  static const std::vector<std::string> STRIP = {" ", "\t", "\n", "\r", "　", "、", "。",
                                                 "／", "/"};
  if (P.pii_split.finditer(s).empty()) {
    const auto t = furigana::trim(s);
    return t.empty() ? std::vector<std::string>{} : std::vector<std::string>{t};
  }
  // Python の re.split: 分割点で切り、区切り自体は捨てる（キャプチャ無しパターンのため）
  std::vector<std::string> segs;
  std::size_t prev = 0;
  for (const auto& m : P.pii_split.finditer(s)) {
    segs.push_back(s.substr(prev, m.begin - prev));
    prev = m.end;
  }
  segs.push_back(s.substr(prev));

  std::vector<std::string> out;
  for (const auto& seg : segs) {
    if (seg.empty()) continue;
    const auto ms = P.pref.finditer(seg);
    if (ms.empty()) continue;  // 住所は都道府県から始まる
    std::string piece = seg.substr(ms[0].begin);
    for (const auto& marker : COMPANY_MARKERS) {  // 末尾の会社名を落とす
      const auto i = piece.find(marker);
      if (i != std::string::npos) piece = piece.substr(0, i);
    }
    piece = strip_set(piece, STRIP);
    if (!piece.empty()) out.push_back(piece);
  }
  return out;
}

/// 組織スパンから会社名だけ残す。Python の _trim_org。
inline std::string trim_org(const Patterns& P, const std::string& s) {
  const auto off = utf8::char_offsets(s);
  const std::size_t n = off.size() - 1;
  auto ch = [&](std::size_t i) { return utf8::slice(s, off, i, i + 1); };

  for (const auto& marker : COMPANY_MARKERS) {
    const auto bi = s.find(marker);
    if (bi == std::string::npos) continue;
    // バイト位置 → 文字位置
    const std::size_t i =
        std::lower_bound(off.begin(), off.end(), bi) - off.begin();
    const std::size_t mlen = utf8::char_len(marker);

    std::size_t j = i;
    while (j > 0 && !is_org_delim(ch(j - 1))) --j;
    std::size_t k = i + mlen;
    while (k < n) {
      if (is_org_delim(ch(k))) {
        // マーカー直後などの単一空白は、次が名前文字（漢字/かな/英字）なら橋渡し。
        const std::string cur = ch(k);
        const std::string nxt = (k + 1 < n) ? ch(k + 1) : "";
        if ((cur == " " || cur == "　") && !nxt.empty() && !is_org_delim(nxt) &&
            has_letter(P, nxt)) {
          ++k;
          continue;
        }
        break;
      }
      ++k;
    }
    return furigana::trim(utf8::slice(s, off, j, k));
  }
  // マーカー無し: メール/電話の手前まで＋先頭の数字・記号を除去
  const auto cut = cut_before_pii(P, s);
  const auto ms = P.org_head_junk.finditer(cut);
  const std::string rest = ms.empty() ? cut : cut.substr(ms[0].end);
  return furigana::trim(rest);
}

/// メール/電話/郵便番号のregexヒット範囲（文字単位）。
inline std::vector<std::pair<std::size_t, std::size_t>> protected_spans(const Patterns& P,
                                                                        const std::string& text) {
  const auto off = utf8::char_offsets(text);
  auto b2c = [&](std::size_t b) {
    return static_cast<std::size_t>(std::lower_bound(off.begin(), off.end(), b) - off.begin());
  };
  std::vector<std::pair<std::size_t, std::size_t>> spans;
  for (const auto* rx : {&P.email, &P.phone, &P.postal})
    for (const auto& m : rx->finditer(text)) spans.emplace_back(b2c(m.begin), b2c(m.end));
  return spans;
}

struct Candidate {
  std::string text;
  std::string entity_type;
  int count = 0;
  float score = 0.f;
  std::vector<std::pair<std::size_t, std::size_t>> offsets;  // 文字単位
};

struct Span {
  std::string type;
  std::size_t begin, end;  // 文字単位
  float score;
};

/// 自作 recognizer（社名/住所）のスパン。Presidio 相当だが、実測で
/// **Presidio は何も削っていない**（生regexのマッチ数と一致）ので、regex の union を
/// (begin,end) 昇順に並べるだけでよい。重なりもそのまま残す。
inline std::vector<Span> regex_spans(const Patterns& P, const std::string& text) {
  const auto off = utf8::char_offsets(text);
  auto b2c = [&](std::size_t b) {
    return static_cast<std::size_t>(std::lower_bound(off.begin(), off.end(), b) - off.begin());
  };
  std::vector<Span> spans;
  for (const auto& m : P.company_mae.finditer(text))
    spans.push_back({"ORGANIZATION", b2c(m.begin), b2c(m.end), 0.8f});
  for (const auto& m : P.company_ato.finditer(text))
    spans.push_back({"ORGANIZATION", b2c(m.begin), b2c(m.end), 0.8f});
  for (const auto& m : P.address.finditer(text))
    spans.push_back({"LOCATION", b2c(m.begin), b2c(m.end), 0.8f});
  std::stable_sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) {
    return a.begin != b.begin ? a.begin < b.begin : a.end < b.end;
  });
  return spans;
}

/// Python の extract_candidates。
inline std::vector<Candidate> extract_candidates(const Patterns& P, sudachi::Analyzer& sd,
                                                 hf::Ner& ner, const std::string& text) {
  const auto off = utf8::char_offsets(text);
  const std::size_t nchars = off.size() - 1;

  // Presidio(自作regex) → HF の順（Python の _presidio_spans + _ginza_spans + _hf_spans）
  std::vector<Span> all = regex_spans(P, text);
  for (const auto& s : ner.spans(text)) all.push_back({s.type, s.begin, s.end, s.score});

  // 空白無視キーで集約。**Python の dict は挿入順を保つ**ので、most_common のタイブレークに
  // 効く。C++ の std::map は並べ替えてしまうため、挿入順を別に持つ。
  struct Group {
    Candidate cand;
    std::vector<std::pair<std::string, int>> surfaces;  // 挿入順の (表記, 出現数)
  };
  std::vector<Group> groups;
  std::map<std::pair<std::string, std::string>, std::size_t> index;  // (無空白キー, 種別) → idx

  for (const auto& sp : all) {
    const std::string raw = furigana::trim(utf8::slice(text, off, sp.begin, sp.end));
    std::vector<std::string> surfaces;
    if (sp.type == "PERSON") {
      surfaces = {furigana::person_name_core(sd, raw)};  // 役職・組織を削る
    } else if (sp.type == "LOCATION") {
      surfaces = trim_location(P, raw);  // 電話・社名を巻き込んだ住所を分割
    } else if (sp.type == "ORGANIZATION") {
      surfaces = {furigana::org_name_core(sd, trim_org(P, raw))};
    } else {
      surfaces = {raw};
    }

    for (auto surface : surfaces) {
      surface = furigana::trim(surface);
      if (surface.empty()) continue;
      const auto key = std::make_pair(strip_all_ws(P, surface), sp.type);
      auto it = index.find(key);
      if (it == index.end()) {
        index[key] = groups.size();
        groups.push_back({Candidate{surface, sp.type, 0, 0.f, {}}, {}});
        it = index.find(key);
      }
      auto& g = groups[it->second];
      g.cand.count += 1;
      g.cand.score = std::max(g.cand.score, sp.score);
      // トリム後の表記の実位置をオフセットにする（保護領域判定を正確に）。
      // Python: text.find(surface, start, end) は**文字単位**。
      std::size_t idx = sp.begin;
      const auto window = utf8::slice(text, off, sp.begin, sp.end);
      const auto rel = window.find(surface);
      if (rel != std::string::npos) {
        idx = sp.begin + utf8::char_len(window.substr(0, rel));
      }
      g.cand.offsets.emplace_back(idx, idx + utf8::char_len(surface));
      bool found = false;
      for (auto& [s, c] : g.surfaces)
        if (s == surface) {
          ++c;
          found = true;
          break;
        }
      if (!found) g.surfaces.emplace_back(surface, 1);
    }
  }

  // 表示表記は最頻（同数なら短い方、さらに同点なら挿入順で先）
  for (auto& g : groups) {
    const auto best = std::min_element(
        g.surfaces.begin(), g.surfaces.end(),
        [](const auto& a, const auto& b) {
          if (a.second != b.second) return a.second > b.second;   // 出現数が多い方
          return a.first.size() < b.first.size();                 // 同数なら短い方
        });
    g.cand.text = best->first;
  }

  const auto protectedv = protected_spans(P, text);
  auto overlaps_protected = [&](const Candidate& c) {
    if (c.offsets.empty()) return false;
    return std::all_of(c.offsets.begin(), c.offsets.end(), [&](const auto& o) {
      return std::any_of(protectedv.begin(), protectedv.end(), [&](const auto& p) {
        return !(o.second <= p.first || o.first >= p.second);  // 区間が交差
      });
    });
  };

  auto drop = [&](const Candidate& c) {
    if (overlaps_protected(c)) return true;  // メール等の断片
    if (c.entity_type == "PERSON" && !P.person_addr_noise.finditer(c.text).empty())
      return true;  // 住所を人名と誤ラベル
    if (!has_letter(P, c.text)) return true;  // 数字・記号だけ
    if (!MASK_DEPARTMENTS && c.entity_type == "ORGANIZATION" && is_department(c.text))
      return true;
    if (c.entity_type == "ORGANIZATION") {  // 会社マーカーだけ＝具体的な社名でない
      std::string residual = c.text;
      for (const auto& m : COMPANY_MARKERS) {
        std::size_t p;
        while ((p = residual.find(m)) != std::string::npos) residual.erase(p, m.size());
      }
      if (!has_letter(P, residual)) return true;
    }
    if (c.entity_type == "LOCATION" && P.loc_digit.finditer(c.text).empty())
      return true;  // 番地の数字を含まない＝一般地名
    return false;
  };

  std::vector<Candidate> out;
  for (const auto& g : groups)
    if (!drop(g.cand)) out.push_back(g.cand);

  std::map<std::string, int> type_order;
  for (std::size_t i = 0; i < CANDIDATE_ENTITY_TYPES.size(); ++i)
    type_order[CANDIDATE_ENTITY_TYPES[i]] = static_cast<int>(i);
  std::sort(out.begin(), out.end(), [&](const Candidate& a, const Candidate& b) {
    const int ta = type_order.count(a.entity_type) ? type_order[a.entity_type] : 99;
    const int tb = type_order.count(b.entity_type) ? type_order[b.entity_type] : 99;
    if (ta != tb) return ta < tb;
    if (a.count != b.count) return a.count > b.count;
    return a.text < b.text;  // UTF-8 のバイト順＝コードポイント順なので Python と一致
  });
  (void)nchars;
  return out;
}

}  // namespace step2
