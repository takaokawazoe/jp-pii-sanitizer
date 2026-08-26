// 検知精度の測定（eval ハーネス）。
//
// **適合試験（phase*）とは目的が違う。** phase* は「意図せず挙動が変わっていないか」を見る
// 回帰試験で、eval は「そもそもどれだけ捕まえられているか」を測る物差し。Python 実装を
// 休止した時点でこの物差しが失われていたので、C++ 側へ移した。
//
// 正解は samples/eval.jsonl（機械可読）。人間向けの説明は samples/*_ground_truth.md に残す。
//
// 測る軸は 3 つ。**1 だけでは足りない**ことは実地で分かっている——直近に直した誤検知 2 件
// （文字起こしのフィラー／同音の姓による往復破壊）は、どちらもマスク率では 100% のままで、
// むしろ「よく覆えている」と評価されてしまう類だった。
//
//   1. マスク率(recall)  : 正解 PII が「種別を問わず」いずれかの候補か番号 regex に覆われるか。
//                          opt-out 運用では覆われた時点で伏せられるので、これが実運用の安全指標。
//   2. 誤検知(precision) : 検出してはいけない語（type=NEGATIVE）が候補に出ていないか。
//   3. 往復健全性        : マスク→逆置換で原文に戻るか。同音の姓の件を捕まえたのがこれ。
//                          安全ゲートも未置換トークン検査も通ってしまうので、別軸が要る。
//
// scope=out は README の免責で対象外と宣言している構造化識別子（社員番号・口座・生年月日等）。
// マスク率には数えないが、**どれだけ素通りしているかは出す**——免責の裏づけになる。
//
// 使い方: eval [eval.jsonl] [--data DIR] [--samples DIR] [--min-mask N]
//   --min-mask を指定すると、対象内のマスク数がそれを下回ったとき非ゼロ終了する（ラチェット）。

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "extractors.hpp"
#include "file_io.hpp"
#include "json.hpp"
#include "numparse.hpp"
#include "ooxml.hpp"
#include "pdf.hpp"
#include "step2.hpp"
#include "tokenizer.hpp"
#include "utf8.hpp"

using json = nlohmann::json;

namespace {

struct Item {
  std::string type, text;
  bool out_of_scope = false;
  bool negative = false;
};

std::string ext_of(const std::string& p) {
  const auto dot = p.find_last_of('.');
  if (dot == std::string::npos) return "";
  std::string e = p.substr(dot + 1);
  for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e;
}

/// 照合キー。ここで吸収するのは**同じ実体の綴り違い**だけで、検知の甘さではない。
///
///  - 空白: 帳票の「佐藤 美咲」と正解「佐藤美咲」を同一視（ツール自身も空白無視でマスクする）。
///  - ㈱/㈲: 正解ラベルは再構成テキスト基準で「曲淵テクノサービス株式会社」だが、pptx の
///    実体は「曲淵テクノサービス㈱」。**ツールは正しくマスクしている**ので、ここで畳まないと
///    照合側の都合で「漏れ」に見えてしまう。
std::string norm(const std::string& s) {
  std::string t = tokenizer::strip_spaces(s);
  for (const auto& [from, to] :
       {std::pair<const char*, const char*>{"\xE3\x88\xB1", "株式会社"},   // ㈱
        {"\xE3\x88\xB2", "有限会社"}}) {                                    // ㈲
    std::string out;
    std::size_t p = 0, q;
    const std::string f = from;
    while ((q = t.find(f, p)) != std::string::npos) {
      out += t.substr(p, q - p);
      out += to;
      p = q + f.size();
    }
    out += t.substr(p);
    t = out;
  }
  return t;
}

/// 正解項目が検出集合に当たるか。境界ノイズを許すため、どちらかが部分文字列でも当たりとする
/// （「高橋誠一 弁護士」で「高橋誠一」を覆えていればマスクはされる）。
bool covered(const std::string& gt, const std::vector<std::string>& detected) {
  const std::string g = norm(gt);
  if (g.empty()) return false;
  for (const auto& d : detected) {
    const std::string n = norm(d);
    if (n.empty()) continue;
    if (n.find(g) != std::string::npos || g.find(n) != std::string::npos) return true;
  }
  return false;
}

std::string extract_text(const std::string& path) {
  const std::string e = ext_of(path);
  if (e == "docx") return ooxml::extract_docx(path);
  if (e == "pptx") return ooxml::extract_pptx(path);
  if (e == "xlsx") return ooxml::read_xlsx_prose(path);
  if (e == "pdf") return pdf::extract_pdf(path);
  return extractors::extract_plain(path);  // txt / csv / md
}

}  // namespace

int main(int argc, char** argv) {
  std::string gt_path = "../samples/eval.jsonl";
  std::string data_dir = "models";
  std::string samples_dir = "../samples";
  int min_mask = -1, min_rt = -1;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
    if (a == "--data") data_dir = next();
    else if (a == "--samples") samples_dir = next();
    else if (a == "--min-mask") min_mask = numparse::to_int(next(), -1);
    else if (a == "--min-roundtrip") min_rt = numparse::to_int(next(), -1);
    else if (!a.empty() && a[0] != '-') gt_path = a;
  }

  // ---- 正解の読み込み（ファイルの出現順を保つ） ----
  std::vector<std::string> order;
  std::map<std::string, std::vector<Item>> gt;
  {
    const std::string body = fileio::read_all(gt_path);
    if (body.empty()) {
      std::fprintf(stderr, "正解が読めません: %s\n", gt_path.c_str());
      return 2;
    }
    std::size_t pos = 0;
    while (pos < body.size()) {
      const auto nl = body.find('\n', pos);
      std::string line = body.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      pos = (nl == std::string::npos) ? body.size() : nl + 1;
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.empty() || line[0] == '#') continue;
      const json j = json::parse(line);
      if (j.contains("_meta")) continue;
      const std::string f = j.value("file", std::string());
      if (f.empty()) continue;
      if (!gt.count(f)) order.push_back(f);
      Item it;
      it.type = j.value("type", std::string());
      it.text = j.value("text", std::string());
      it.out_of_scope = j.value("scope", std::string()) == "out";
      it.negative = it.type == "NEGATIVE";
      gt[f].push_back(std::move(it));
    }
  }

  // ---- 検知エンジン（CLI/GUI と同じ経路） ----
  const std::string pj = fileio::read_all(data_dir + "/patterns.json");
  if (pj.empty()) {
    std::fprintf(stderr, "patterns.json が読めません: %s\n", data_dir.c_str());
    return 2;
  }
  const json pjson = json::parse(pj);
  std::map<std::string, std::string> pats, lmap;
  for (auto it = pjson["patterns"].begin(); it != pjson["patterns"].end(); ++it)
    pats[it.key()] = it.value().get<std::string>();
  for (auto it = pjson["label_map"].begin(); it != pjson["label_map"].end(); ++it)
    lmap[it.key()] = it.value().get<std::string>();
  step2::Patterns P(pats);
  const std::string sres = data_dir + "/sudachi/resources";
  sudachi::Analyzer sd(sres + "/sudachi.json", sres, data_dir + "/sudachi/system.dic");
  hf::Ner ner(data_dir + "/model_fp16.onnx", data_dir + "/tokenizer.json",
              data_dir + "/labels.json", lmap);

  static const re::Regex email{R"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})"};
  static const re::Regex phone{R"((?<![\d\-])(?:0\d{1,4}-\d{1,4}-\d{3,4}|0\d{9,10})(?![\d\-]))"};
  static const re::Regex postal{R"((?<![\d\-])\d{3}-\d{4}(?![\d\-]))"};

  int in_total = 0, in_masked = 0, out_total = 0, out_masked = 0;
  int neg_total = 0, neg_hit = 0, rt_total = 0, rt_ok = 0, rt_ws_only = 0;
  std::vector<std::string> leaks, extract_gaps, false_positives, rt_broken;

  for (const auto& f : order) {
    const std::string text = utf8::repair(extract_text(samples_dir + "/" + f));
    const auto cands = step2::extract_candidates(P, sd, ner, text);

    // opt-out 時に実際に覆われる範囲＝全候補 ∪ メール/電話/郵便。**種別は問わない**
    // （住所が ORG 扱いでも、覆われていればマスクはされる）。
    std::vector<std::string> spans;
    for (const auto& c : cands) spans.push_back(c.text);
    for (const auto* rx : {&email, &phone, &postal})
      for (const auto& m : rx->finditer(text)) spans.push_back(m.text);

    int fi = 0, fm = 0, fneg = 0;
    for (const auto& it : gt[f]) {
      if (it.negative) continue;  // マスク後に判定する（下の「軸2」）
      const bool ok = covered(it.text, spans);
      if (it.out_of_scope) {
        ++out_total;
        if (ok) ++out_masked;
        continue;
      }
      ++in_total;
      ++fi;
      if (ok) {
        ++in_masked;
        ++fm;
        continue;
      }
      // 覆われなかった理由を分ける。本文に在るのに拾えないのが「本当の漏れ」で、
      // そもそもテキスト化できていないなら抽出側の問題（直す場所が違う）。
      if (norm(text).find(norm(it.text)) != std::string::npos)
        leaks.push_back(f + ": " + it.text + " [" + it.type + "]");
      else
        extract_gaps.push_back(f + ": " + it.text + " [" + it.type + "]");
    }

    // 軸3: 全候補をマスクして逆置換したとき原文に戻るか。
    // 常用語と同音の姓（阿野→あの）で本文が化ける類を捕まえる。安全ゲートは通ってしまう。
    std::vector<tokenizer::ConfirmedTerm> conf;
    for (const auto& c : cands) conf.push_back({c.text, c.entity_type});
    tokenizer::Tokenizer tok;
    const std::string masked = tok.tokenize(text, conf, &sd);

    // ---- 軸2: 検出してはいけない語が、実際に**マスクされて消えていないか** ----
    //
    // **「候補に出たか」で見てはいけない。** 同音の姓による往復破壊（阿野の読み「あの」で
    // 本文のフィラーが masked された件）では、「あの」は一度も候補になっていない——
    // 読みの名寄せで確定リストに入り、候補一覧を経由せずにマスクされた。
    // 候補だけを見ていると、あの種類の誤マスクは丸ごと素通りする。
    // 出力に何回残ったかで判定すれば、経路によらず捕まる。
    const auto count_of = [](const std::string& hay, const std::string& needle) {
      if (needle.empty()) return std::size_t{0};
      std::size_t n = 0, p = 0;
      while ((p = hay.find(needle, p)) != std::string::npos) { ++n; p += needle.size(); }
      return n;
    };
    for (const auto& it : gt[f]) {
      if (!it.negative) continue;
      ++neg_total;
      const std::size_t before = count_of(text, it.text), after = count_of(masked, it.text);
      if (before > 0 && after < before) {
        ++neg_hit;
        ++fneg;
        false_positives.push_back(f + ": " + it.text + "（" + std::to_string(before) + " 回中 " +
                                  std::to_string(before - after) + " 回がマスクされた）");
      }
    }
    tokenizer::Tokenizer back;
    back.load_mapping(tok.mapping_ordered());
    // 判定は 3 段階。設計どおりの損失を NG にしないための切り分け。
    //
    //  exact        : 完全一致
    //  空白差       : 空白無視マッチ（PDF の行末で割れた氏名を拾うための機能）の副作用で、
    //                 「田中健一」が代表表記「田中 健一」で戻る。設計どおり。
    //  表記差       : 読みの名寄せ（同一人物のカナ表記と漢字表記を 1 トークンに寄せる機能）の
    //                 副作用で、「イトウヨウコ」が「伊藤陽子」で戻る。**同じ実体**なので設計どおり。
    //                 判定は「復元後をもう一度マスクしたら同じ結果になるか」＝差が
    //                 マスク対象の内側に閉じているか、で行う。
    //  NG           : それ以外。マスク対象**外**の本文が書き換わっている＝本当の破壊。
    //
    // 誤って別の語がマスクされる件（阿野の読み「あの」）は、ここではなく軸2 が捕まえる。
    // あちらは「マスクされてはいけない語が消えたか」を直接見るので、経路によらず効く。
    const std::string restored = back.reverse(masked);
    const bool exact = restored == text;
    const bool ws_only =
        !exact && tokenizer::strip_spaces(restored) == tokenizer::strip_spaces(text);
    const bool round_trip = exact || ws_only;
    ++rt_total;
    if (round_trip) {
      ++rt_ok;
      if (ws_only) ++rt_ws_only;
    } else {
      // **空白を落とした側で**最初の相違点を探す。素の比較だと、設計どおりの空白差が
      // 先に見つかって本当の原因が隠れる（実際それで xlsx の診断を読み違えた）。
      const std::string a = tokenizer::strip_spaces(text), b = tokenizer::strip_spaces(restored);
      std::size_t d = 0;
      while (d < a.size() && d < b.size() && a[d] == b[d]) ++d;
      const auto ctx = [](const std::string& s, std::size_t at) {
        std::size_t beg = at > 24 ? at - 24 : 0;
        while (beg > 0 && (static_cast<unsigned char>(s[beg]) & 0xC0) == 0x80) --beg;
        std::size_t end = at + 32 < s.size() ? at + 32 : s.size();
        while (end < s.size() && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) ++end;
        return s.substr(beg, end - beg);
      };
      rt_broken.push_back(f + "\n        原文: ..." + ctx(a, d) + "...\n        復元: ..." +
                          ctx(b, d) + "...");
    }

    std::printf("  %-28s マスク %2d/%-2d  誤検知 %d  往復 %s\n", f.c_str(), fm, fi, fneg,
                !round_trip ? "NG" : (exact ? "OK" : "OK(空白差)"));
  }

  std::printf("\n  == 1. マスク率(対象内) : %d/%d ==\n", in_masked, in_total);
  for (const auto& s : leaks) std::printf("     x 漏れ（本文にあるが未検出）: %s\n", s.c_str());
  for (const auto& s : extract_gaps)
    std::printf("     x 抽出漏れ（テキスト化できず）: %s\n", s.c_str());
  std::printf("  == 2. 誤検知(NEGATIVE) : %d/%d がマスクされた ==\n", neg_hit, neg_total);
  for (const auto& s : false_positives) std::printf("     x %s\n", s.c_str());
  std::printf("  == 3. 往復健全性       : %d/%d（うち空白差のみ %d）==\n", rt_ok, rt_total,
              rt_ws_only);
  for (const auto& s : rt_broken) std::printf("     x 原文に戻らない: %s\n", s.c_str());
  std::printf("  -- 参考: 対象外(構造化識別子) %d/%d が偶然覆われた（免責どおり保証はしない）--\n",
              out_masked, out_total);

  const bool pass =
      (neg_hit == 0) && (min_mask < 0 || in_masked >= min_mask) && (min_rt < 0 || rt_ok >= min_rt);
  if (min_mask >= 0 && in_masked < min_mask)
    std::printf("\n  マスク数が下限を下回りました（%d < %d）。\n", in_masked, min_mask);
  std::printf("\n  === eval: %s ===\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
