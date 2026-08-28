// furigana.py の移植: 氏名・社名の「芯」の切り出し。
//
// Python 版は SudachiPy の品詞で境界ノイズを削っている。同じ核（sudachi.rs）に繋いでいるので
// 形態素は一致する（phase1_sudachi で 398/398 実測済み）。あとはロジックを写すだけ。
//
// **要注意**: Python の `name[a:b]` は**文字（コードポイント）単位**。C++ の std::string は
// バイト列なので、そのまま添字を使うと日本語で壊れる。ここでは begin/end（コードポイント）を
// バイト位置に変換してから切り出す。
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "sudachi_shim.hpp"
#include "utf8.hpp"

namespace furigana {

// UTF-8 の文字↔バイト変換は utf8.hpp に集約（step2 側と共有する）。
using utf8::char_offsets;

// 会社名の芯を判定するときに「これを含む名詞連続が社名」とみなすマーカー。
// furigana.py の _COMPANY_MARKERS と対応（順序も合わせる）。
inline const std::vector<std::string>& company_markers() {
  static const std::vector<std::string> m = {
      "株式会社", "有限会社", "合同会社", "合名会社", "合資会社",
      "一般社団法人", "一般財団法人", "公益社団法人", "公益財団法人",
      "協同組合", "（株）", "(株)", "（有）", "(有)", "㈱", "㈲",
  };
  return m;
}

/// 文字単位 [begin, end) を UTF-8 のバイト範囲で切り出す（Python の name[a:b] 相当）。
inline std::string slice_chars(const std::string& s, const std::vector<std::size_t>& off,
                               std::size_t begin, std::size_t end) {
  return utf8::slice(s, off, begin, end);
}

// 空白判定・trim は utf8.hpp に集約（sudachi 非依存の extractors 側と共有するため）。
using utf8::is_space_cp;
using utf8::trim;

/// 氏名スパンから氏名部分だけを取り出す（furigana.person_name_core の移植）。
///
/// SudachiPy の品詞（固有名詞/人名）を使い、最長の「人名トークン連続」を氏名とみなす。
/// 例「佐藤 美咲 DX推進室」→「佐藤 美咲」。
/// **人名トークンが無ければ元の表記をそのまま返す**（未知の姓などを削りすぎない）。
inline std::string person_name_core(sudachi::Analyzer& sd, const std::string& name) {
  const auto morphs = sd.tokenize(name);

  struct Run {
    std::size_t begin, end, count;
  };
  std::vector<Run> runs;
  bool in_run = false;
  for (const auto& m : morphs) {
    const bool is_name = m.pos.size() > 2 && m.pos[2] == "人名";
    const bool is_ws = !m.pos.empty() && m.pos[0] == "空白";
    if (is_name) {
      if (in_run) {
        runs.back().end = m.end;
        runs.back().count += 1;
      } else {
        runs.push_back({m.begin, m.end, 1});
        in_run = true;
      }
    } else if (is_ws && in_run) {
      continue;  // 氏名内の空白は橋渡し（「佐藤 美咲」）
    } else {
      in_run = false;
    }
  }
  if (runs.empty()) return name;

  // Python の max() は同点なら**最初**を採る。std::max_element も同じ（strict less）。
  const auto best = std::max_element(
      runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.count < b.count; });
  const auto off = char_offsets(name);
  return trim(slice_chars(name, off, best->begin, best->end));
}

/// 組織スパンから会社名の芯だけを取り出す（furigana.org_name_core の移植）。
///
/// 「名詞的トークンの連続」を求め、**会社マーカーを含む最長の連続**を社名とみなす。
/// マーカーを含む連続が無ければ元表記を返す（マーカー無しの社名を壊さない）。
inline std::string org_name_core(sudachi::Analyzer& sd, const std::string& name) {
  const auto morphs = sd.tokenize(name);

  struct Run {
    std::size_t begin, end;
  };
  std::vector<Run> runs;
  bool in_run = false;
  for (const auto& m : morphs) {
    // 名詞・接頭辞・形状詞（グローバル等のカタカナ形容名詞も社名に入る）を名前部とみなす。
    const bool is_name_part =
        (!m.pos.empty() &&
         (m.pos[0] == "名詞" || m.pos[0] == "接頭辞" || m.pos[0] == "形状詞")) ||
        m.surface == "・" || m.surface == "&" || m.surface == "＆";
    if (is_name_part) {
      if (in_run) {
        runs.back().end = m.end;
      } else {
        runs.push_back({m.begin, m.end});
        in_run = true;
      }
    } else if (!m.pos.empty() && m.pos[0] == "空白" && in_run) {
      continue;  // 社名内の空白は橋渡し（「有限会社 蛇喰産業」→1社名）
    } else {
      in_run = false;
    }
  }

  const auto off = char_offsets(name);
  std::vector<Run> marker_runs;
  for (const auto& r : runs) {
    const auto seg = slice_chars(name, off, r.begin, r.end);
    for (const auto& mk : company_markers()) {
      if (seg.find(mk) != std::string::npos) {
        marker_runs.push_back(r);
        break;
      }
    }
  }
  if (marker_runs.empty()) {
    // マーカー（株式会社 等）が無いときは、以前はスパンをそのまま返していた。だが NER の
    // スパンは前後に食み出すことがあり、文字起こしだと**フィラーごと 1 スパンになる**
    // （実測: 句読点の無い書き起こしで「えっと弊社」「えっと大手」が ORGANIZATION 候補に
    // なった）。マーカーがある場合は下の最長連続の取り方で自然に落ちるので、抜けていたのは
    // このマーカー無しの経路だけ。
    //
    // **落とすのは先頭のフィラー（感動詞）だけ。** 「前後の非名詞を削る」まで広げると
    // 実在の名前を削る（実測）:
    //   曲淵テクノサービス㈱ → 曲淵テクノサービス  （㈱ は記号なので末尾で落ちる）
    //   簓田                 → 簓                  （田 が接尾辞なので末尾で落ちる）
    // 過剰マスクは品質の問題で済むが、名前を削るのは取りこぼし側に効く。狭く倒す。
    std::size_t skip = 0;
    while (skip < morphs.size() && !morphs[skip].pos.empty() &&
           (morphs[skip].pos[0] == "感動詞" || morphs[skip].pos[0] == "空白" ||
            (morphs[skip].pos.size() > 1 && morphs[skip].pos[1] == "フィラー")))
      ++skip;
    if (skip == 0 || skip >= morphs.size()) return name;
    return trim(slice_chars(name, off, morphs[skip].begin, morphs.back().end));
  }

  // マーカーを含む最長連続。Python の max(key=len) は同点なら最初を採る。
  const auto best = std::max_element(
      marker_runs.begin(), marker_runs.end(),
      [](const Run& a, const Run& b) { return (a.end - a.begin) < (b.end - b.begin); });
  return trim(slice_chars(name, off, best->begin, best->end));
}

/// 文字列が「名詞を1つも含まない」か（＝感動詞・副詞・助詞などだけで出来ているか）。
///
/// 人名は必ず名詞なので、これが真なら人名として扱ってはいけない、という判断に使う。
/// 使い所は2つ:
///   1. 検知の候補フィルタ（step2）— 文字起こしのフィラーが PERSON 候補になるのを防ぐ
///   2. 名寄せに使う読みの選別（tokenizer）— 常用語と同音の読みで本文を壊すのを防ぐ
inline bool has_no_noun(sudachi::Analyzer& sd, const std::string& s) {
  bool any = false;
  for (const auto& m : sd.tokenize(s)) {
    if (m.pos.empty() || m.pos[0] == "空白") continue;
    any = true;
    if (m.pos[0] == "名詞") return false;
  }
  return any;  // 形態素が取れなければ判断材料が無いので落とさない
}

/// カタカナ → ひらがな（U+30A1..U+30F6 を -0x60）。furigana._kata_to_hira 相当。
inline std::string kata_to_hira(const std::string& s) {
  std::string out;
  const auto off = char_offsets(s);
  for (std::size_t i = 0; i + 1 < off.size(); ++i) {
    const auto c = utf8::slice(s, off, i, i + 1);
    // UTF-8 3バイトのカタカナ: E3 82 A1(ァ) .. E3 83 B6(ヶ)
    if (c.size() == 3 && static_cast<unsigned char>(c[0]) == 0xE3) {
      const unsigned cp = 0x3000 | ((static_cast<unsigned char>(c[1]) & 0x3F) << 6) |
                          (static_cast<unsigned char>(c[2]) & 0x3F);
      if (cp >= 0x30A1 && cp <= 0x30F6) {
        const unsigned h = cp - 0x60;
        out += static_cast<char>(0xE3);
        out += static_cast<char>(0x80 | ((h >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (h & 0x3F));
        continue;
      }
    }
    out += c;
  }
  return out;
}

/// 全部が仮名（ひらがな・カタカナ・半角カナ）か。空文字は false。
///
/// 1 文字だけの候補を落とす判定に使う。**漢字は含めない**——「林」「森」のような
/// 1 文字の姓が実在するため。仮名 1 文字の人名・社名は無いので、こちらだけ切る。
inline bool is_all_kana(const std::string& s) {
  const auto off = char_offsets(s);
  if (off.size() < 2) return false;
  for (std::size_t i = 0; i + 1 < off.size(); ++i) {
    const auto c = utf8::slice(s, off, i, i + 1);
    if (c.size() != 3) return false;  // ASCII・4バイト文字は仮名ではない
    const unsigned cp = ((static_cast<unsigned char>(c[0]) & 0x0F) << 12) |
                        ((static_cast<unsigned char>(c[1]) & 0x3F) << 6) |
                        (static_cast<unsigned char>(c[2]) & 0x3F);
    const bool hira = cp >= 0x3041 && cp <= 0x309F;  // ぁ..ゟ
    const bool kata = cp >= 0x30A0 && cp <= 0x30FF;  // ゠..ヿ（長音符 ー を含む）
    const bool half = cp >= 0xFF66 && cp <= 0xFF9F;  // ｦ..ﾟ（半角カナ）
    if (!(hira || kata || half)) return false;
  }
  return true;
}

/// 漢字氏名のカナ/かな読みの表記ゆれ候補（furigana.readings の移植）。
///
/// 連結・半角空白区切り・全角空白区切り × カタカナ/ひらがな の計6形（重複除去）。
/// 読めない・元表記と同じものは除外。
///
/// **Python 版は set を返すため順序が実行ごとに変わる**（文字列ハッシュ随機化）が、
/// 同一人物の読みは全て同じ canonical に寄る＝同じトークンになるので結果に影響しない
/// （実文書5本＋全候補でハッシュ3回一致を実測）。C++ 側は決定論的に並べる。
inline std::vector<std::string> readings(sudachi::Analyzer& sd, const std::string& name) {
  std::vector<std::string> parts;
  for (const auto& m : sd.tokenize(name)) {
    if (trim(m.surface).empty()) continue;  // 空白トークンは飛ばす（「佐藤 美咲」対策）
    if (!m.reading.empty()) parts.push_back(m.reading);
  }
  if (parts.empty()) return {};

  std::vector<std::string> out;
  for (const std::string& sep : {std::string(""), std::string(" "), std::string("　")}) {
    std::string kata;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (i) kata += sep;
      kata += parts[i];
    }
    for (const auto& v : {kata, kata_to_hira(kata)}) {
      if (v.empty() || v == name) continue;
      if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
    }
  }
  return out;
}

}  // namespace furigana
