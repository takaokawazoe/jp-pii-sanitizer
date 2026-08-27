// Phase 4d: .eml 抽出の回帰試験。
//
// **期待値はここに手で書いてある**（オラクル JSON を持たない）。理由は 2 つ:
//   - .eml のフィクスチャは自分で組み立てたので、正解が分かっている。スナップショットより
//     「何を保証しているか」がコードに残る方が強い。
//   - Python 実装は休止中で、期待値を生成してくれる参照実装がもう無い（python-parked.md）。
//     フィクスチャは testdata/eml/*.eml のテキストなので、Python 無しで作り直せる。
//     （生成スクリプトの役目は文字コード変換と base64/QP の符号化だけ。）
//
// 使い方: phase4_eml [testdata/eml のディレクトリ]

#include <cstdio>
#include <string>
#include <vector>

#include "eml.hpp"
#include "utf8.hpp"

namespace {

int failures = 0;
std::string current;

void fail(const std::string& what, const std::string& detail) {
  std::printf("  NG: [%s] %s\n", current.c_str(), what.c_str());
  if (!detail.empty()) std::printf("      %s\n", detail.c_str());
  ++failures;
}

void expect_contains(const std::string& hay, const std::string& needle) {
  if (hay.find(needle) == std::string::npos)
    fail("含まれるはずの文字列が無い: " + needle, hay.substr(0, 400));
}

void expect_absent(const std::string& hay, const std::string& needle) {
  if (hay.find(needle) != std::string::npos)
    fail("含まれてはいけない文字列がある: " + needle, hay.substr(0, 400));
}

void expect_eq(const std::string& got, const std::string& want, const char* what) {
  if (got != want) fail(std::string(what) + " が違う", "got=[" + got + "] want=[" + want + "]");
}

eml::Result load(const std::string& dir, const std::string& name) {
  current = name;
  return eml::extract_eml(dir + "/" + name);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = (argc > 1) ? argv[1] : "testdata/eml";

  // ---- 01: 7bit / UTF-8。encoded-word のヘッダと素の本文 ----
  {
    const auto r = load(dir, "01_plain_utf8.eml");
    expect_eq(r.error, "", "error");
    expect_contains(r.text, "件名: 【ご連絡】請求書の確認について");
    expect_contains(r.text, "差出人: 山田 太郎 <yamada@example.co.jp>");
    expect_contains(r.text, "宛先: 田中 健一 <tanaka@example.com>");
    // dewrap_prose が「。」を含むブロックの改行を詰める（.msg と同じ扱い）
    expect_contains(r.text, "営業部の山田太郎です。請求書の件、田中健一様にご確認をお願いします。");
    // CRLF が残っていると下流の文字位置が狂う
    expect_absent(r.text, "\r");
    if (!r.attachments.empty()) fail("添付が無いはず", "");
  }

  // ---- 02: ISO-2022-JP ＋ quoted-printable。**今回の本命** ----
  // charset を読み飛ばしていた頃は、件名も本文も丸ごと U+FFFD に潰れていた。
  {
    const auto r = load(dir, "02_iso2022jp_qp.eml");
    expect_eq(r.error, "", "error");
    expect_contains(r.text, "件名: 【至急】ご相談の件");
    expect_contains(r.text, "差出人: 佐藤 美咲 <sato@aozora.co.jp>");
    expect_contains(r.text, "Cc: 渡辺 隆志 <watanabe@example.co.jp>");
    // ソフト改行（行末の "=" ＋ 改行）が正しく落ちていれば語が繋がる
    expect_contains(r.text, "先日ご相談いただいた件、担当の渡辺隆志よりご連絡いたします。");
    expect_contains(r.text, "株式会社あおぞら物産の佐藤美咲です。");
    expect_absent(r.text, "\xEF\xBF\xBD");  // U+FFFD が 1 つでもあれば復号に失敗している
    expect_absent(r.text, "=1B");           // QP が復号されずに残っていないか
  }

  // ---- 03: multipart/alternative は平文を採る（HTML 側の別人を拾わない） ----
  {
    const auto r = load(dir, "03_multipart_alt.eml");
    expect_eq(r.error, "", "error");
    expect_contains(r.text, "平文の本文です。担当は高橋一郎です。");
    expect_absent(r.text, "別人 太郎");
    expect_absent(r.text, "<p>");
  }

  // ---- 04: multipart/mixed の添付。**中身は展開せずファイル名だけ** ----
  {
    const auto r = load(dir, "04_multipart_attach.eml");
    expect_eq(r.error, "", "error");
    expect_contains(r.text, "添付のとおりご確認ください。担当: 中村さくら");
    const std::vector<std::string> want = {"請求書_2026年03月.pdf", "顧客名簿.xlsx"};
    if (r.attachments != want) {
      std::string got;
      for (const auto& a : r.attachments) got += "[" + a + "]";
      fail("添付ファイル名", got);
    }
    // 添付の中身（base64 を復号したもの）が本文に混ざっていないこと
    expect_absent(r.text, "%PDF");
    expect_absent(r.text, "dummy");
    // 本文へは名前だけを足す
    const std::string with = eml::body_with_attachment_names(r);
    expect_contains(with, "【添付ファイル】");
    expect_contains(with, "請求書_2026年03月.pdf");
  }

  // ---- 05: HTML のみ。テキスト化して使う（script/style は落とす） ----
  {
    const auto r = load(dir, "05_html_only.eml");
    expect_eq(r.error, "", "error");
    expect_contains(r.text, "株式会社みらいテクノロジーズの伊藤陽子です。");
    expect_contains(r.text, "03-1234-5678");
    expect_absent(r.text, "color:red");
    expect_absent(r.text, "<b>");
  }

  // ---- 06: 扱えない charset。**黙って空を返さず理由を出す** ----
  // ここが空のまま error も空だと、利用者には「PII が無かった」としか見えない。
  {
    const auto r = load(dir, "06_unknown_charset.eml");
    if (r.error.empty()) fail("読めない charset なのに error が空", "");
    expect_contains(r.text, "件名: テスト");  // ヘッダは取れている
  }

  // ---- 07: Shift_JIS の本文（8bit そのまま） ----
  {
    const auto r = load(dir, "07_shiftjis_8bit.eml");
    expect_eq(r.error, "", "error");
    expect_contains(r.text, "件名: 連絡事項");
    expect_contains(r.text, "担当は小林大輔です。");
    expect_absent(r.text, "\xEF\xBF\xBD");
  }

  if (failures) {
    std::printf("\n  === Phase 4d（.eml 抽出）: FAIL (%d) ===\n", failures);
    return 1;
  }
  std::printf("\n  === Phase 4d（.eml 抽出）: PASS (7 件) ===\n");
  return 0;
}
