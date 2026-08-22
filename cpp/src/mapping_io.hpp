// 対応表（トークン ↔ 実値）の唯一のシリアライザ。GUI と CLI が共有する。
//
// **なぜ1本にするか**: 以前は GUI が JS で CSV を組み立て、CLI が C++ で JSONL を書いて
// いたため、GUI が保存した対応表を `cli restore` に食わせると 1 行目で落ちた。書式を
// 二重に持つ限りこの手のズレは再発するので、読み書きをここに集約する。
//
// **なぜ JSONL か**（docs/cli.md 設計判断 2 と同じ根拠が GUI にも当てはまる）:
//   - 1行1オブジェクトなので手編集に耐える（行を消せば1件消える）。
//   - 値にカンマ・引用符・改行が入っても JSON のエスケープで済み、CSV の引用規則を
//     自前で持たなくてよい。対応表の「実値」は文書由来の任意文字列なので、これは効く。
//
// **書式**:
//   {"_meta":{"version":1}}
//   {"token":"{{PERSON_1}}","original":"佐藤健一"}
//   ...
// 先頭の _meta 行は token/original を持たないので、これを知らない読み手は素通りする
// （＝後方互換）。将来パスフレーズ暗号化を足すときは、この _meta に封筒の情報
// （version:2・KDF・nonce 等）を載せて中身を1行の封筒に差し替える。
//
// **例外メッセージに実値を載せないこと。** ここはサニタイザなので、メッセージは UI や
// ログに出る＝マスク前の値が外へ出る。行番号とトークン（＝プレースホルダ）までに留める。
#pragma once

#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "json.hpp"

namespace mapping_io {

/// 現在の書式バージョン。読み手はこれより新しいものを**明示的に拒否**する。
inline constexpr int FORMAT_VERSION = 1;

/// 対応表1件。並びは (実値, トークン)＝tokenizer::Tokenizer::load_mapping が要求する順。
using Entry = std::pair<std::string, std::string>;

/// JSONL に直す。先頭に _meta 行を置く。
inline std::string to_jsonl(const std::vector<Entry>& value_token) {
  std::ostringstream out;
  out << nlohmann::json{{"_meta", {{"version", FORMAT_VERSION}}}}.dump() << "\n";
  for (const auto& [value, token] : value_token)
    out << nlohmann::json{{"token", token}, {"original", value}}.dump() << "\n";
  return out.str();
}

namespace detail {

/// UTF-8 BOM を落とす。
///
/// 利用者が対応表をエディタで開いて保存し直すと BOM が付くことがあり、そのままだと
/// 1行目の json::parse が落ちる。拡張子が .jsonl なので当方は BOM を書かないが、
/// 読む側では必ず剥がしておく。
inline std::string strip_bom(const std::string& s) {
  if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
      static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF)
    return s.substr(3);
  return s;
}

inline bool is_blank_or_comment(const std::string& line) {
  const std::size_t s = line.find_first_not_of(" \t");
  return s == std::string::npos || line[s] == '#';
}

}  // namespace detail

/// JSONL を読む。**厳格**: 壊れた行・片側だけの行・空値・重複トークンは例外にする。
///
/// 黙って読み飛ばすと、その分だけ復元漏れになる（「実名に戻した」テキストに
/// {{PERSON_3}} が残る）。落ちた事実に気づけないのが一番まずいので、行番号を添えて落とす。
/// 素通りさせるのは「空行・# コメント・token も original も**キーが無い**行」だけ。
/// 最後のものが _meta 行と将来の拡張レコードを通す穴になっている。
inline std::vector<Entry> parse(const std::string& body) {
  std::vector<Entry> out;
  std::map<std::string, std::string> seen_token;  // token → original（重複検出）
  std::istringstream in(detail::strip_bom(body));
  std::string line;
  std::size_t ln = 0;
  bool any_line = false;

  while (std::getline(in, line)) {
    ++ln;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (detail::is_blank_or_comment(line)) continue;
    any_line = true;

    nlohmann::json j;
    try {
      j = nlohmann::json::parse(line);
    } catch (const std::exception&) {
      // 旧 GUI が書いた CSV（token,original）を貼られたときに、生の JSON エラーだけ
      // 返すと原因が分からない。書式違いだと分かる言い方にする。
      throw std::runtime_error("対応表 " + std::to_string(ln) +
                               " 行目を JSON として読めません。JSONL 形式の対応表"
                               "（1行1件の {\"token\":…,\"original\":…}）を指定してください。");
    }
    if (!j.is_object())
      throw std::runtime_error("対応表 " + std::to_string(ln) +
                               " 行目がオブジェクトではありません。");

    const bool has_token = j.contains("token");
    const bool has_original = j.contains("original");

    if (!has_token && !has_original) {
      // _meta 行・将来の拡張レコード。version だけ見て、知らない書式なら**明示的に**落とす。
      // ここを黙って読み飛ばすと、暗号化された対応表を「0 件の対応表」として
      // 正常終了させてしまう（＝復元したつもりで何も戻っていない）。
      if (j.contains("_meta")) {
        const auto& meta = j["_meta"];
        const int v = (meta.is_object() && meta.contains("version") &&
                       meta["version"].is_number_integer())
                          ? meta["version"].get<int>()
                          : 0;
        if (v <= 0)
          throw std::runtime_error("対応表 " + std::to_string(ln) +
                                   " 行目の _meta.version が読めません。");
        if (v > FORMAT_VERSION)
          throw std::runtime_error(
              "この対応表は新しい形式です（version " + std::to_string(v) + "、本バージョンは " +
              std::to_string(FORMAT_VERSION) + " まで対応）。ツールを更新してください。");
      }
      continue;
    }

    if (!has_token || !has_original)
      throw std::runtime_error("対応表 " + std::to_string(ln) + " 行目に " +
                               (has_token ? "original" : "token") + " がありません。");
    if (!j["token"].is_string() || !j["original"].is_string())
      throw std::runtime_error("対応表 " + std::to_string(ln) +
                               " 行目の token / original が文字列ではありません。");

    const std::string token = j["token"].get<std::string>();
    const std::string original = j["original"].get<std::string>();
    if (token.empty() || original.empty())
      throw std::runtime_error("対応表 " + std::to_string(ln) + " 行目の " +
                               (token.empty() ? "token" : "original") + " が空です。");

    const auto it = seen_token.find(token);
    if (it != seen_token.end()) {
      if (it->second == original) continue;  // 完全な重複は無害なので落とすだけ
      // 同じトークンに別の実値。どちらで戻すかが決まらず、現行の逆置換は先勝ちで
      // 後を無言で捨てるので、対応表が壊れているサインとして落とす。
      throw std::runtime_error("対応表 " + std::to_string(ln) + " 行目: トークン " + token +
                               " が異なる値で重複しています。");
    }
    seen_token.emplace(token, original);
    out.emplace_back(original, token);
  }

  if (!any_line) throw std::runtime_error("対応表が空です。");
  return out;
}

}  // namespace mapping_io
