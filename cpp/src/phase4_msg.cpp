// Phase 4c の完成条件を判定する適合試験（判断）。
//
// **完成条件は「PII の取りこぼしゼロ」**（candidate 集合の完全一致でも byte 一致でもない）。
//
// 経緯（実測）: msg 本文は Python(extract_msg) と **byte 完全一致**する（dewrap_prose も一致）。
// だが extract_msg の m.sender/m.to は独自ロジックで整形され Aspose の生プロパティと一致せず
// （09 は sender の email を落とし、08 は to が email のみ）、この**ヘッダ整形の差が NER の
// 250字チャンク文脈をずらして候補の分節を変える**（同じ本文でも 山田↔山田製作所 等）。
// さらに Python 候補には 田中角栄(記念館=施設) のような Python 側の誤検知も混じる。
// → 候補の完全一致は「Python の NER ノイズ＋チャンク依存」を追う無意味な基準。
//
// 本質は「**C++ の抽出テキストが Python の検出した PII を1つも落としていないか**」。
// Python が候補にした各表層が C++ テキストに（空白無視で）現れれば、抽出は情報を保っている。
// NER の再分節は本文一致＋phase2(155/155)で別途検証済み。
//
// 使い方: phase4_msg [msg_ref.json]

#include <cstdio>
#include <fstream>
#include <string>

#include "file_io.hpp"
#include "json.hpp"
#include "msg.hpp"
#include "re.hpp"

using json = nlohmann::json;

// 空白（ASCII/全角/改行）を除いた文字列（oracle のキーと同じ正規化）。
static std::string strip_ws(const std::string& s) {
  static const re::Regex ws{R"(\s)"};
  std::string out;
  std::size_t prev = 0;
  for (const auto& m : ws.finditer(s)) {
    out += s.substr(prev, m.begin - prev);
    prev = m.end;
  }
  out += s.substr(prev);
  return out;
}

int main(int argc, char** argv) {
  const std::string msg_path = (argc > 1) ? argv[1] : "testdata/msg_ref.json";
  std::ifstream f(msg_path);
  if (!f) {
    std::fprintf(stderr, "開けません: %s\n", msg_path.c_str());
    return 2;
  }
  json ref;
  f >> ref;

  std::size_t ok = 0, ng = 0;
  int html_ok = 0, html_total = 0;
  for (const auto& c : ref["cases"]) {
    const auto path = c["path"].get<std::string>();
    const bool is_html = c["html_body_path"].get<bool>();
    const auto res = msg::extract_msg(path);
    const std::string ntext = strip_ws(res.text);

    // Python が検出した各 PII 表層が C++ テキストに現れるか
    std::vector<std::string> missing;
    for (const auto& kv : c["candidates"]) {
      const std::string surface = kv[0].get<std::string>();  // 既に空白無視キー
      if (ntext.find(surface) == std::string::npos)
        missing.push_back(surface + "/" + kv[1].get<std::string>());
    }

    // 添付の数・ファイル名も確認（Python の children と照合）
    const auto& want_children = c["children"];
    bool child_ok = res.children.size() == want_children.size();
    for (std::size_t i = 0; child_ok && i < want_children.size(); ++i)
      if (res.children[i].filename != want_children[i]["source"].get<std::string>())
        child_ok = false;

    const char* name = path.c_str();
    const auto slash = path.find_last_of('/');
    if (slash != std::string::npos) name = path.c_str() + slash + 1;

    const bool same = missing.empty() && child_ok;
    if (is_html) {
      ++html_total;
      const std::size_t got = c["candidates"].size() - missing.size();
      std::printf("  [HTML] %-22s PII被覆 %zu/%zu\n", name, got, c["candidates"].size());
      if (same) ++html_ok;
    } else {
      same ? ++ok : ++ng;
      std::printf("  %-26s %s (PII %zu件・添付%zu)\n", name, same ? "OK" : "NG",
                  c["candidates"].size(), res.children.size());
      for (const auto& x : missing) std::printf("      取りこぼし: %s\n", x.c_str());
      if (!child_ok) std::printf("      添付不一致: C++=%zu Python=%zu\n", res.children.size(),
                                 want_children.size());
    }
  }


  // ---- 日本語ファイル名で開けるか（回帰試験）----
  //
  // Aspose の `mapi_message::from_file` に narrow のパスを渡していたため、**日本語の
  // ファイル名・フォルダ名の .msg は一度も開けていなかった**（MSVC は narrow をロケールの
  // ANSI コードページ＝日本語 Windows では cp932 として解釈する）。利用者からは
  // 「Unable to open CFB file: ...」または「No mapping for the Unicode character exists
  // in the target multi-byte code page.」として見えた。UTF-8 バイト列がたまたま cp932 と
  // して妥当かどうかでどちらになるかが変わるだけで、原因は同じ。
  // 今は自前でバイト列を読んで from_stream に渡している。ASCII パスと同じ結果になること。
  int jp_ng = 0;
  if (!ref["cases"].empty()) {
    const std::string src = ref["cases"][0]["path"].get<std::string>();
    const std::string dst = "phase4_msg_tmp_\xE4\xBC\x9A\xE8\xAD\xB0.msg";  // 会議.msg
    if (!fileio::write_all(dst, fileio::read_all(src))) {
      std::printf("  NG: 日本語名の一時ファイルを作れない\n");
      ++jp_ng;
    } else {
      try {
        if (msg::extract_msg(src).text != msg::extract_msg(dst).text) {
          std::printf("  NG: 日本語ファイル名で本文が違う\n");
          ++jp_ng;
        }
      } catch (const std::exception& e) {
        std::printf("  NG: 日本語ファイル名の .msg を開けない: %s\n", e.what());
        ++jp_ng;
      }
      fileio::remove_file(dst);
    }
    if (jp_ng == 0) std::printf("\n  日本語ファイル名（会議.msg）: OK\n");
  }

  // ---- 非 Unicode(PT_STRING8) の文字列を復号できるか（回帰試験）----
  //
  // 日本語 Windows の Outlook は .msg を非 Unicode で保存することがある。そのとき
  // Aspose の decode_string8 は**バイト列をそのまま返す**（コードページ変換をしない）ので、
  // 本文が cp932 のまま dewrap_prose の正規表現に届き
  //   「pcre2 match 失敗: UTF-8 error: isolated byte with 0x80 bit set」
  // で抽出ごと落ちていた（利用者からの報告）。**出口の utf8::repair では遅い**——
  // その手前で正規表現が走るため。今は msg::mapi_text が入口で UTF-8 に揃える。
  //
  // .msg のフィクスチャを組み立てて端から端まで通す方が強いが、Aspose の書き出し
  // （mapi_message::create + save）が落ちるため断念した。ここでは境界の関数を直接見る:
  // extract_msg 内の全ての Aspose 由来文字列がこの関数を通ることは、実装側で担保する。
  int s8_ng = 0;
  {
    // cp932 のバイト列。ソースは /utf-8 でコンパイルされるので、非 UTF-8 は \xNN で書く。
    const std::string subj_cp932 = "\x90\xBF\x8B\x81\x8F\x91\x82\xCC\x8C\x8F";  // 請求書の件
    const std::string body_cp932 =
        "\x8E\x52\x93\x63\x91\xBE\x98\x59\x82\xC5\x82\xB7\x81\x42"  // 山田太郎です。
        "\n\n"                                                      // dewrap_prose の区切り
        "\x8A\x94\x8E\xAE\x89\xEF\x8E\xD0\x82\xA0\x82\xA8\x82\xBC\x82\xE7\x95\xA8\x8E\x59"
        "\x82\xCC\x8C\x8F\x81\x41\x82\xE6\x82\xEB\x82\xB5\x82\xAD\x82\xA8\x8A\xE8\x82\xA2"
        "\x82\xB5\x82\xDC\x82\xB7\x81\x42";  // 株式会社あおぞら物産の件、よろしくお願いします。

    // 前提の確認: このバイト列は UTF-8 としては不正（＝ PCRE2 が拒む形）
    if (utf8::is_valid(body_cp932)) {
      std::printf("  NG: 試験データが cp932 になっていない（UTF-8 として妥当だった）\n");
      ++s8_ng;
    }
    const std::string subj = msg::mapi_text(subj_cp932);
    const std::string body = msg::mapi_text(body_cp932);
    if (!utf8::is_valid(subj) || !utf8::is_valid(body)) {
      std::printf("  NG: mapi_text が不正な UTF-8 を返した\n");
      ++s8_ng;
    }
    if (subj != "請求書の件") {
      std::printf("  NG: 件名の復号が違う: %s\n", subj.c_str());
      ++s8_ng;
    }
    for (const char* want : {"山田太郎", "株式会社あおぞら物産"}) {
      if (body.find(want) == std::string::npos) {
        std::printf("  NG: 本文に %s が無い\n", want);
        ++s8_ng;
      }
    }
    // **本番と同じ順序で正規表現に通す。** ここが落ちていたのが報告された症状。
    try {
      const std::string dewrapped = extractors::dewrap_prose(body);
      if (dewrapped.find("山田太郎") == std::string::npos) {
        std::printf("  NG: dewrap_prose 後に本文が失われた\n");
        ++s8_ng;
      }
    } catch (const std::exception& e) {
      std::printf("  NG: dewrap_prose が落ちた: %s\n", e.what());
      ++s8_ng;
    }
    // 既に UTF-8 のものは素通し（Unicode の .msg の挙動を変えていないこと）
    if (msg::mapi_text("山田太郎") != "山田太郎") {
      std::printf("  NG: UTF-8 の文字列が素通しされていない\n");
      ++s8_ng;
    }
    if (s8_ng == 0) std::printf("  非 Unicode(cp932) の文字列: OK\n");
  }
  std::printf("\n  平文本文の msg: PII取りこぼしゼロ %zu/%zu\n", ok, ok + ng);
  std::printf("  HTML本文の msg（bs4近似）: 完全被覆 %d/%d（参考）\n", html_ok, html_total);
  const bool pass = (ng == 0 && ok > 0 && jp_ng == 0 && s8_ng == 0);
  std::printf("\n  === Phase 4c（msg・PII取りこぼしゼロ）: %s ===\n",
              pass ? "PASS (Python の検出PIIを1つも落とさない)" : "FAIL");
  return pass ? 0 : 1;
}
