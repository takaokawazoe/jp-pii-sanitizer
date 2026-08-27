// 単体試験（オラクル比較の phase* とは別種）。
//
// ここが押さえるのは「モデルも辞書も要らないが、間違うと静かに壊れる」層:
//   - utf8      : 不正な UTF-8 の検出と修復（下流の PCRE2 / Rust シムと同じ厳しさか）
//   - numparse  : 壊れた入力で例外を投げないこと
//   - encoding  : cp932 → UTF-8
//   - fileio    : 日本語パスでの読み書き
//   - mapping_io: 対応表の読み書き（平文・暗号化）と、厳格リーダーのエラー
//
// Linux(GCC) と MSVC の両方で走らせること。プラットフォーム差が出る層なので、
// 片方だけ通っても意味が薄い。
#include <cstdio>
#include <string>
#include <vector>

#include "encoding.hpp"
#include "file_io.hpp"
#include "mime.hpp"
#include "mapping_io.hpp"
#include "numparse.hpp"
#include "utf8.hpp"

namespace {

int failures = 0;
void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("  NG: %s\n", what);
    ++failures;
  }
}

/// parse が投げることを確かめる。
void check_throws(const std::string& body, const char* what) {
  try {
    mapping_io::parse(body);
    std::printf("  NG: %s（例外が出なかった）\n", what);
    ++failures;
  } catch (const std::exception&) {
  }
}

void check_throws_pass(const std::string& body, const std::string& pass, const char* what) {
  try {
    mapping_io::parse(body, pass);
    std::printf("  NG: %s（例外が出なかった）\n", what);
    ++failures;
  } catch (const std::exception&) {
  }
}

void test_utf8() {
  check(utf8::is_valid(""), "empty");
  check(utf8::is_valid("abc"), "ascii");
  check(utf8::is_valid("\xE6\x97\xA5\xE6\x9C\xAC"), "kanji (3byte)");
  check(utf8::is_valid("\xF0\x9F\x90\xB1"), "emoji (4byte)");

  // PCRE2(PCRE2_UTF) と Rust の CStr::to_str() が拒否するものを同じ厳しさで落とす
  check(!utf8::is_valid("\x82\xA0"), "cp932 bytes (lone continuation)");
  check(!utf8::is_valid("\xC0\xAF"), "overlong 2byte");
  check(!utf8::is_valid("\xE0\x80\xAF"), "overlong 3byte");
  check(!utf8::is_valid("\xF0\x80\x80\xAF"), "overlong 4byte");
  check(!utf8::is_valid("\xED\xA0\x80"), "lone surrogate (PDF 由来の CESU-8)");
  check(!utf8::is_valid("\xF4\x90\x80\x80"), "beyond U+10FFFF");
  check(!utf8::is_valid("\xE6\x97"), "truncated 3byte");
  check(!utf8::is_valid("\xFF"), "0xFF");

  const char* bad[] = {"\x82\xA0", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xE6\x97",
                       "abc\xFF\xE6\x97\xA5"};
  for (const char* b : bad) check(utf8::is_valid(utf8::repair(b)), "repair -> valid");
  check(utf8::repair("abc\xFF\xE6\x97\xA5") == "abc\xEF\xBF\xBD\xE6\x97\xA5",
        "repair keeps valid parts");
  check(utf8::repair("\xE6\x97\xA5\xE6\x9C\xAC") == "\xE6\x97\xA5\xE6\x9C\xAC",
        "repair is identity on valid input");

  // 不正入力でも境界表が s.size() を飛び越えない
  const std::string s = "\xE6\x97";
  const auto off = utf8::char_offsets(s);
  check(off.back() == s.size(), "char_offsets ends at size()");
  for (std::size_t i = 1; i < off.size(); ++i)
    check(off[i - 1] <= off[i] && off[i] <= s.size(), "char_offsets monotonic & in range");
  check(utf8::char_len("\xE6\x97\xA5\xE6\x9C\xAC") == 2, "char_len kanji");
  check(utf8::char_len("\xF0\x9F\x90\xB1") == 1, "char_len emoji");
}

void test_numparse_encoding_fileio() {
  check(numparse::to_int("12") == 12, "to_int");
  check(numparse::to_int("", -1) == -1, "to_int empty");
  check(numparse::to_int("zz", -1) == -1, "to_int garbage");
  check(numparse::to_int("99999999999999999999", -1) == -1, "to_int overflow");
  check(numparse::to_int_hex("4A") == 0x4A, "to_int_hex");
  check(numparse::to_int_hex("ZZ", 0) == 0, "to_int_hex garbage");
  std::string out;
  // cp932 の「あ」。**POSIX でも通るようになった**（以前はここで !ok を期待していた）。
  // iconv は glibc 本体の一部なので追加依存は無い。
  check(encoding::cp932_to_utf8("\x82\xA0", out) && out == "\xE3\x81\x82", "cp932 -> UTF-8");
  check(encoding::cp932_to_utf8("", out) && out.empty(), "cp932 empty");

  // ISO-2022-JP の「日本」。ESC $ B ... ESC ( B。日本語メールの件名で今も普通に来る。
  // バイト列は素の \xNN で書く（「本」の JIS 下位バイトが 0x5C ＝ バックスラッシュなので、
  // 文字として書くと C++ のエスケープと紛れる）。
  const std::string jis_nihon = "\x1B\x24\x42\x46\x7C\x4B\x5C\x1B\x28\x42";
  check(encoding::charset_to_utf8("ISO-2022-JP", jis_nihon, out) &&
            out == "\xE6\x97\xA5\xE6\x9C\xAC",
        "ISO-2022-JP -> UTF-8");
  // **実メールの形**: ASCII → JIS → ASCII。Windows のコードページ 50220/50221/50222 は
  // 戻りのエスケープを解釈できず U+E12A にして、以降の ASCII を JIS の 2 バイトとして
  // 食い潰す（実測）。自前で剥がしているのはこれが理由。
  check(encoding::charset_to_utf8("iso-2022-jp", "Re: " + jis_nihon + " ok", out) &&
            out == "Re: \xE6\x97\xA5\xE6\x9C\xAC ok",
        "ISO-2022-JP ascii-jis-ascii");
  // 半角カナ（ESC ( I）→ ｱｲ
  check(encoding::charset_to_utf8("iso-2022-jp", "\x1B\x28\x49\x31\x32\x1B\x28\x42", out) &&
            out == "\xEF\xBD\xB1\xEF\xBD\xB2",
        "ISO-2022-JP 半角カナ");
  // 非対応の指示子（ISO-2022-JP-2 の GB2312）は黙って化けさせず false
  check(!encoding::charset_to_utf8("iso-2022-jp", "\x1B\x24\x41\x41\x42\x1B\x28\x42", out),
        "ISO-2022-JP 非対応の指示子は false");

  // EUC-JP の「日本」
  check(encoding::charset_to_utf8("euc-jp", "\xC6\xFC\xCB\xDC", out) &&
            out == "\xE6\x97\xA5\xE6\x9C\xAC",
        "EUC-JP -> UTF-8");
  // 表記ゆれ（大小・引用符・区切り）を吸収する
  check(encoding::charset_to_utf8("\"Shift_JIS\"", "\x82\xA0", out) && out == "\xE3\x81\x82",
        "charset 名の表記ゆれ");
  // UTF-8 と未指定は素通し（変換しない）
  check(encoding::charset_to_utf8("utf-8", "\xE6\x97\xA5", out) && out == "\xE6\x97\xA5",
        "utf-8 passthrough");
  check(encoding::charset_to_utf8("", "abc", out) && out == "abc", "charset 未指定は素通し");
  // **未知の charset は false**。黙って化けさせず「読めなかった」と言えるようにする。
  check(!encoding::known_charset("koi8-r"), "未知の charset は known_charset=false");
  check(!encoding::charset_to_utf8("koi8-r", "abc", out), "未知の charset は変換しない");

  // ---- RFC 2047 ヘッダ復号（mime.hpp）----
  // **これが今まで壊れていた**: charset を読み飛ばして UTF-8 前提だったので、
  // ISO-2022-JP の件名は復号後に不正な UTF-8 になり utf8::repair で丸ごと U+FFFD になった。
  check(mime::decode_mime_header("=?ISO-2022-JP?B?GyRCRnxLXBsoQg==?=") ==
            "\xE6\x97\xA5\xE6\x9C\xAC",
        "ISO-2022-JP の encoded-word");
  check(mime::decode_mime_header("=?utf-8?B?5pel5pys?=") == "\xE6\x97\xA5\xE6\x9C\xAC",
        "utf-8 の encoded-word");
  // 隣り合う encoded-word の間の空白は捨てる（RFC 2047 §6.2）。長い日本語の件名は
  // 76 バイト制限で折り返されるので、残すと語の途中に空白が入る。
  check(mime::decode_mime_header("=?utf-8?B?5pel?= =?utf-8?B?5pys?=") ==
            "\xE6\x97\xA5\xE6\x9C\xAC",
        "隣接 encoded-word の空白を捨てる");
  // encoded-word でない部分の空白は残す
  check(mime::decode_mime_header("Re: =?utf-8?B?5pel?= <a@b.jp>") ==
            "Re: \xE6\x97\xA5 <a@b.jp>",
        "生テキストの空白は残す");
  check(mime::decode_mime_header("plain subject") == "plain subject", "encoded-word 無しは素通し");
  // 言語タグ付き（RFC 2231）
  check(mime::decode_mime_header("=?utf-8*ja?B?5pel?=") == "\xE6\x97\xA5", "言語タグ付き charset");

  const std::string p = "unit_tests_tmp_\xE6\x97\xA5\xE6\x9C\xAC.txt";  // 日本語ファイル名
  check(fileio::write_all(p, "\xE6\x97\xA5\xE6\x9C\xAC"), "write_all (日本語パス)");
  check(fileio::exists(p), "exists");
  check(fileio::read_all(p) == "\xE6\x97\xA5\xE6\x9C\xAC", "read_all round-trip");
  check(!fileio::exists("no_such_file_xyz"), "exists false");
  // std::remove では日本語名を消せない（ナローパスが cp932 扱いになるため）
  check(fileio::remove_file(p), "remove_file (日本語パス)");
  check(!fileio::exists(p), "削除後は存在しない");
}

void test_mapping_plaintext() {
  using V = std::vector<mapping_io::Entry>;
  {
    const V in = {{"\xE4\xBD\x90\xE8\x97\xA4", "{{PERSON_1}}"}, {"03-1234-5678", "{{PHONE_1}}"}};
    const std::string s = mapping_io::to_jsonl(in);
    check(s.rfind("{\"_meta\":", 0) == 0, "先頭に _meta 行");
    check(mapping_io::parse(s) == in, "往復で一致");
    check(!mapping_io::is_encrypted(s), "平文は is_encrypted=false");
  }
  // CSV が壊す文字を通す（JSONL に寄せた理由そのもの）
  {
    const V in = {{"\xE5\xB1\xB1\xE7\x94\xB0, \xE5\xA4\xAA\xE9\x83\x8E", "{{PERSON_1}}"},
                  {"he is \"boss\"", "{{PERSON_2}}"},
                  {"line1\nline2", "{{ADDRESS_1}}"},
                  {"tab\there", "{{PERSON_3}}"}};
    check(mapping_io::parse(mapping_io::to_jsonl(in)) == in, "カンマ/引用符/改行/タブが往復");
  }
  // BOM・CRLF・空行・コメント
  {
    const auto got = mapping_io::parse(
        "\xEF\xBB\xBF{\"_meta\":{\"version\":1}}\r\n\r\n# comment\r\n  \r\n"
        "{\"token\":\"{{PERSON_1}}\",\"original\":\"x\"}\r\n");
    check(got.size() == 1, "BOM/CRLF/空行/コメントを跨いで読める");
  }
  // _meta 無し（旧 CLI 出力）／未知の拡張レコードは素通り
  check(mapping_io::parse("{\"token\":\"{{PERSON_1}}\",\"original\":\"x\"}\n").size() == 1,
        "_meta 無しでも読める");
  check(mapping_io::parse("{\"_meta\":{\"version\":1}}\n{\"note\":\"future\"}\n"
                          "{\"token\":\"{{PERSON_1}}\",\"original\":\"x\"}\n")
                .size() == 1,
        "token/original を持たない行は素通り");

  // 厳格: 黙って落とさない
  check_throws("{\"token\":\"{{PERSON_1}}\"}\n", "original が無い行");
  check_throws("{\"original\":\"x\"}\n", "token が無い行");
  check_throws("{\"token\":\"\",\"original\":\"x\"}\n", "token が空");
  check_throws("{\"token\":\"{{PERSON_1}}\",\"original\":\"\"}\n", "original が空");
  check_throws("{\"token\":1,\"original\":\"x\"}\n", "token が文字列でない");
  check_throws("[1,2,3]\n", "オブジェクトでない行");
  check_throws("{\"token\":\"{{PERSON_1}}\",\"original\":\"a\"}\n"
               "{\"token\":\"{{PERSON_1}}\",\"original\":\"b\"}\n",
               "同一トークンが別の値で重複");
  check_throws("", "空の対応表");
  check_throws("# comment only\n\n", "実体の無い対応表");
  // 完全な重複は 1 件に畳む
  check(mapping_io::parse("{\"token\":\"{{PERSON_1}}\",\"original\":\"a\"}\n"
                          "{\"token\":\"{{PERSON_1}}\",\"original\":\"a\"}\n")
                .size() == 1,
        "完全重複は 1 件");

  // 旧 CSV は非対応。ただし書式違いと分かるメッセージで落ちること
  {
    bool helpful = false;
    try {
      mapping_io::parse("token,original\n{{PERSON_1}},x\n");
    } catch (const std::exception& e) {
      helpful = std::string(e.what()).find("JSONL") != std::string::npos;
    }
    check(helpful, "旧 CSV は JSONL 形式である旨を示して落ちる");
  }
  // ヘッダを消した CSV。データ行が '{' 始まりなので、行頭1文字での判定では誤る
  check_throws("{{PERSON_1}},x\n", "ヘッダ無し CSV も落ちる");
}

void test_mapping_encrypted() {
  using V = std::vector<mapping_io::Entry>;
  const V in = {{"\xE4\xBD\x90\xE8\x97\xA4\xE5\x81\xA5\xE4\xB8\x80", "{{PERSON_1}}"},
                {"kenichi@example.co.jp", "{{EMAIL_1}}"}};
  const std::string pass = "correct horse battery staple";
  const std::string enc = mapping_io::to_jsonl_encrypted(in, pass);

  check(mapping_io::is_encrypted(enc), "is_encrypted=true");
  check(enc.rfind("{\"_meta\":", 0) == 0, "先頭に _meta 行");
  // 平文が本文に残っていないこと（当たり前だが、実装ミスで素通しになると致命的）
  check(enc.find("{{PERSON_1}}") == std::string::npos, "トークンが平文で残らない");
  check(enc.find("kenichi@example.co.jp") == std::string::npos, "実値が平文で残らない");

  check(mapping_io::parse(enc, pass) == in, "暗号化の往復で一致");

  // パスフレーズ無しでは NeedsPassphrase（型で判別できること）
  {
    bool typed = false;
    try {
      mapping_io::parse(enc);
    } catch (const mapping_io::NeedsPassphrase&) {
      typed = true;
    } catch (const std::exception&) {
    }
    check(typed, "パスフレーズ無しは NeedsPassphrase");
  }

  check_throws_pass(enc, "wrong passphrase", "パスフレーズ違い");

  // 暗号文の改竄（本体の 1 文字を差し替える）
  {
    std::string t = enc;
    const std::size_t nl = t.find('\n');
    const std::size_t at = nl + 5;
    t[at] = (t[at] == 'A' ? 'B' : 'A');
    check_throws_pass(t, pass, "暗号文の改竄を検出");
  }
  // _meta の改竄（nonce を別の値に）＝ AAD と鍵の両方が変わるので必ず落ちる
  {
    std::string t = enc;
    const std::size_t p = t.find("\"nonce\":\"");
    const std::size_t at = p + 10;
    t[at] = (t[at] == 'A' ? 'B' : 'A');
    check_throws_pass(t, pass, "_meta の改竄を検出");
  }

  // 空パスフレーズは書き出し側で拒否（唯一の固い規則）
  {
    bool threw = false;
    try {
      mapping_io::to_jsonl_encrypted(in, "");
    } catch (const std::exception&) {
      threw = true;
    }
    check(threw, "空パスフレーズでの暗号化を拒否");
  }

  // 読み込み時のパラメータ上限：鍵導出（＝メモリ確保）より前に落ちること。
  // ここが無いと、細工したファイルで読み手のメモリを枯渇させられる。
  check_throws_pass("{\"_meta\":{\"version\":2,\"kdf\":{\"alg\":\"argon2id\",\"v\":19,"
                    "\"m\":4294967296,\"t\":3,\"p\":1,\"salt\":\"AAAAAAAAAAAAAAAAAAAAAA==\"},"
                    "\"aead\":\"xchacha20poly1305-ietf\",\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}}\nAAAA\n",
                    pass, "m が上限超なら確保前に拒否");
  check_throws_pass("{\"_meta\":{\"version\":2,\"kdf\":{\"alg\":\"scrypt\",\"v\":19,"
                    "\"m\":262144,\"t\":3,\"p\":1,\"salt\":\"AAAAAAAAAAAAAAAAAAAAAA==\"},"
                    "\"aead\":\"xchacha20poly1305-ietf\",\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}}\nAAAA\n",
                    pass, "未知の KDF を拒否");
  check_throws_pass("{\"_meta\":{\"version\":2,\"kdf\":{\"alg\":\"argon2id\",\"v\":19,"
                    "\"m\":262144,\"t\":3,\"p\":4,\"salt\":\"AAAAAAAAAAAAAAAAAAAAAA==\"},"
                    "\"aead\":\"xchacha20poly1305-ietf\",\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}}\nAAAA\n",
                    pass, "未対応の並列度を拒否");

  // 未知バージョンは「0 件の対応表として正常終了」させず明示的に拒否
  check_throws("{\"_meta\":{\"version\":3}}\n{\"token\":\"{{PERSON_1}}\",\"original\":\"x\"}\n",
               "未知の version は拒否");

  // 例外メッセージに実値を載せない（UI やログに出るため）
  {
    std::string msg;
    try {
      mapping_io::parse(enc, "wrong");
    } catch (const std::exception& e) {
      msg = e.what();
    }
    check(!msg.empty() && msg.find("\xE4\xBD\x90\xE8\x97\xA4") == std::string::npos &&
              msg.find("kenichi@example.co.jp") == std::string::npos,
          "復号失敗のメッセージに実値を含めない");
  }
}

}  // namespace

int main() {
  test_utf8();
  test_numparse_encoding_fileio();
  test_mapping_plaintext();
  test_mapping_encrypted();
  std::printf(failures ? "\n  === unit_tests: FAIL (%d) ===\n" : "\n  === unit_tests: PASS ===\n",
              failures);
  return failures ? 1 : 0;
}
