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
#include <string_view>
#include <utility>
#include <vector>

#include <sodium.h>

#include "json.hpp"

namespace mapping_io {

/// 平文の書式バージョン。
inline constexpr int FORMAT_VERSION = 1;
/// パスフレーズで暗号化した書式バージョン（設計は docs/mapping-encryption.md）。
///
/// **平文を version 1 に据え置くのが要点。** 新しい版が書いた平文は、この機能を知らない
/// 旧ビルドでもそのまま読める＝この追加は純粋な上乗せになる。gate がかかるのは
/// 暗号化ファイルだけで、旧ビルドはそれを「新しい形式です」と明示的に撥ねる。
inline constexpr int FORMAT_VERSION_ENCRYPTED = 2;

// ---- KDF パラメータ（書き出し時は固定・読み込み時はファイルの値に従う） ----
inline constexpr unsigned long long KDF_MEM_KIB = 262144;  // 256 MiB
inline constexpr unsigned long long KDF_OPS = 3;
inline constexpr int KDF_PARALLELISM = 1;  // libsodium の crypto_pwhash は p=1 固定
/// 読み込み時のメモリ上限。悪意あるファイルが `m = 4 TiB` を指定して読み手の
/// メモリを枯渇させるのを、確保前に止める。
inline constexpr unsigned long long KDF_MEM_KIB_MAX = 1048576;  // 1 GiB
inline constexpr unsigned long long KDF_OPS_MAX = 16;

/// 暗号化されていてパスフレーズが要る、と呼び出し側に伝えるための型。
/// パーサは対話できないので、UI/CLI 側で入力を促してから再度渡してもらう。
struct NeedsPassphrase : std::runtime_error {
  NeedsPassphrase() : std::runtime_error("この対応表はパスフレーズで保護されています。") {}
};

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

/// libsodium を1回だけ初期化する。
inline void ensure_sodium() {
  static const int rc = sodium_init();  // 関数ローカル static なので初期化は一度きり
  if (rc < 0) throw std::runtime_error("libsodium の初期化に失敗しました。");
}

inline std::string b64_encode(const unsigned char* p, std::size_t n) {
  std::string out(sodium_base64_ENCODED_LEN(n, sodium_base64_VARIANT_ORIGINAL), '\0');
  sodium_bin2base64(out.data(), out.size(), p, n, sodium_base64_VARIANT_ORIGINAL);
  out.resize(std::char_traits<char>::length(out.c_str()));  // 末尾 NUL を落とす
  return out;
}

inline std::vector<unsigned char> b64_decode(const std::string& s, const char* what) {
  std::vector<unsigned char> out(s.size());  // base64 は必ず元より長いので足りる
  std::size_t n = 0;
  if (sodium_base642bin(out.data(), out.size(), s.data(), s.size(), nullptr, &n, nullptr,
                        sodium_base64_VARIANT_ORIGINAL) != 0)
    throw std::runtime_error(std::string("対応表の ") + what + " を base64 として読めません。");
  out.resize(n);
  return out;
}

/// パスフレーズ → 鍵（Argon2id）。
inline void derive_key(unsigned char* key, std::size_t key_len, std::string_view passphrase,
                       const unsigned char* salt, unsigned long long mem_kib,
                       unsigned long long ops) {
  if (crypto_pwhash(key, key_len, passphrase.data(), passphrase.size(), salt, ops,
                    static_cast<std::size_t>(mem_kib * 1024ULL),
                    crypto_pwhash_ALG_ARGON2ID13) != 0)
    // crypto_pwhash が失敗するのは実質メモリ確保の失敗。fail closed で落とす。
    throw std::runtime_error("鍵の導出に失敗しました（メモリ不足の可能性があります）。");
}

/// 最初の意味のある行が持つ `_meta`（無ければ null）。書式の振り分けにだけ使う。
/// 壊れた行でも投げない——その場合は平文パーサ側が行番号つきの良いエラーを出す。
inline nlohmann::json first_meta(const std::string& body) {
  std::istringstream in(body);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (is_blank_or_comment(line)) continue;
    try {
      const auto j = nlohmann::json::parse(line);
      if (j.is_object() && j.contains("_meta")) return j["_meta"];
    } catch (const std::exception&) {
    }
    return nlohmann::json();  // 最初の意味行を見たら、そこで判断は終わり
  }
  return nlohmann::json();
}

}  // namespace detail

/// 平文 JSONL を読む。**厳格**: 壊れた行・片側だけの行・空値・重複トークンは例外にする。
///
/// 黙って読み飛ばすと、その分だけ復元漏れになる（「実名に戻した」テキストに
/// {{PERSON_3}} が残る）。落ちた事実に気づけないのが一番まずいので、行番号を添えて落とす。
/// 素通りさせるのは「空行・# コメント・token も original も**キーが無い**行」だけ。
/// 最後のものが _meta 行と将来の拡張レコードを通す穴になっている。
inline std::vector<Entry> parse_plaintext(const std::string& body) {
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
      // _meta 行・将来の拡張レコード。書式の振り分けは呼び出し元（parse）が
      // 先頭行で済ませているので、ここでは「平文 version 1 以外が紛れ込んでいないか」
      // だけを見る。黙って読み飛ばすと、暗号化された対応表を「0 件の対応表」として
      // 正常終了させてしまう（＝復元したつもりで何も戻っていない）。
      if (j.contains("_meta")) {
        const auto& meta = j["_meta"];
        const int v = (meta.is_object() && meta.contains("version") &&
                       meta["version"].is_number_integer())
                          ? meta["version"].get<int>()
                          : 0;
        if (v != FORMAT_VERSION)
          throw std::runtime_error("対応表 " + std::to_string(ln) +
                                   " 行目: 平文として読めない _meta.version です（" +
                                   std::to_string(v) + "）。");
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

/// パスフレーズで保護した JSONL を作る（version 2 の封筒）。
///
/// **中身は「完全な version 1 文書」**。復号すればそのまま平文パーサに渡せるので、
/// 暗号化は純粋な外側の層に留まり、層が混ざらない。
inline std::string to_jsonl_encrypted(const std::vector<Entry>& value_token,
                                      std::string_view passphrase) {
  detail::ensure_sodium();
  // 空パスフレーズは唯一の固い規則。空でも鍵は導出でき、形式的に正しい暗号化ファイルが
  // できてしまう＝「保護されているように見えて誰でも開ける」ファイルになるため。
  if (passphrase.empty()) throw std::runtime_error("パスフレーズが空です。");

  unsigned char salt[crypto_pwhash_SALTBYTES];
  unsigned char nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES];
  randombytes_buf(salt, sizeof salt);
  randombytes_buf(nonce, sizeof nonce);

  // _meta 行は AAD に使うので、鍵導出より先に確定させる。
  const nlohmann::json meta = {
      {"_meta",
       {{"version", FORMAT_VERSION_ENCRYPTED},
        {"kdf",
         {{"alg", "argon2id"},
          {"v", 19},
          {"m", KDF_MEM_KIB},
          {"t", KDF_OPS},
          {"p", KDF_PARALLELISM},
          {"salt", detail::b64_encode(salt, sizeof salt)}}},
        {"aead", "xchacha20poly1305-ietf"},
        {"nonce", detail::b64_encode(nonce, sizeof nonce)}}}};
  const std::string meta_line = meta.dump();

  unsigned char key[crypto_aead_xchacha20poly1305_ietf_KEYBYTES];
  detail::derive_key(key, sizeof key, passphrase, salt, KDF_MEM_KIB, KDF_OPS);

  std::string plain = to_jsonl(value_token);
  std::vector<unsigned char> ct(plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
  unsigned long long ct_len = 0;
  const int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(
      ct.data(), &ct_len, reinterpret_cast<const unsigned char*>(plain.data()), plain.size(),
      reinterpret_cast<const unsigned char*>(meta_line.data()), meta_line.size(),  // AAD
      nullptr, nonce, key);
  sodium_memzero(key, sizeof key);
  sodium_memzero(plain.data(), plain.size());  // 平文の対応表をメモリに残さない
  if (rc != 0) throw std::runtime_error("対応表の暗号化に失敗しました。");
  ct.resize(static_cast<std::size_t>(ct_len));

  // base64 を1行に収めるので、貼り付けでの復元がそのまま成立する。
  return meta_line + "\n" + detail::b64_encode(ct.data(), ct.size()) + "\n";
}

namespace detail {

/// version 2 の封筒を復号し、中身（version 1 文書）を返す。
inline std::string decrypt_envelope(const std::string& body, const nlohmann::json& meta,
                                    std::string_view passphrase) {
  ensure_sodium();
  const auto& kdf = meta["kdf"];
  if (!kdf.is_object()) throw std::runtime_error("対応表の _meta.kdf が読めません。");

  // 既知のアルゴリズムか（未知のものはメモリ確保より前に落とす）
  if (kdf.value("alg", std::string()) != "argon2id")
    throw std::runtime_error("対応表の KDF が未対応です: " + kdf.value("alg", std::string("(無)")));
  if (meta.value("aead", std::string()) != "xchacha20poly1305-ietf")
    throw std::runtime_error("対応表の暗号方式が未対応です: " +
                             meta.value("aead", std::string("(無)")));
  if (kdf.value("p", 0) != KDF_PARALLELISM)
    throw std::runtime_error("対応表の KDF 並列度が未対応です（p=1 のみ対応）。");

  const auto mem_kib = kdf.value("m", 0ULL);
  const auto ops = kdf.value("t", 0ULL);
  // 上限を設けないと、細工したファイルで読み手のメモリを枯渇させられる。
  if (mem_kib == 0 || mem_kib > KDF_MEM_KIB_MAX || ops == 0 || ops > KDF_OPS_MAX)
    throw std::runtime_error("対応表の KDF パラメータが範囲外です。");

  const auto salt = b64_decode(kdf.value("salt", std::string()), "salt");
  const auto nonce = b64_decode(meta.value("nonce", std::string()), "nonce");
  if (salt.size() != crypto_pwhash_SALTBYTES ||
      nonce.size() != crypto_aead_xchacha20poly1305_ietf_NPUBBYTES)
    throw std::runtime_error("対応表の salt / nonce の長さが不正です。");

  // 封筒の本体＝_meta 行の次の、最初の意味のある行
  std::istringstream in(body);
  std::string line, payload, meta_line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (is_blank_or_comment(line)) continue;
    if (meta_line.empty()) { meta_line = line; continue; }
    payload = line;
    break;
  }
  if (payload.empty()) throw std::runtime_error("対応表の本体がありません。");
  const auto ct = b64_decode(payload, "本体");
  if (ct.size() < crypto_aead_xchacha20poly1305_ietf_ABYTES)
    throw std::runtime_error("対応表の本体が短すぎます。");

  unsigned char key[crypto_aead_xchacha20poly1305_ietf_KEYBYTES];
  derive_key(key, sizeof key, passphrase, salt.data(), mem_kib, ops);

  std::string plain(ct.size() - crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
  unsigned long long plain_len = 0;
  const int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
      reinterpret_cast<unsigned char*>(plain.data()), &plain_len, nullptr, ct.data(), ct.size(),
      reinterpret_cast<const unsigned char*>(meta_line.data()), meta_line.size(),  // AAD
      nonce.data(), key);
  sodium_memzero(key, sizeof key);
  if (rc != 0)
    // Poly1305 の検証失敗は「鍵が違う」と「改竄・破損」を区別できない。これは
    // 仕様上そうなるもので、区別できる仕組み（照合用ハッシュ等）を足すと
    // Argon2id のコストを無効化する高速オラクルを攻撃者に渡すことになる。
    throw std::runtime_error("パスフレーズが違うか、対応表が壊れています。");
  plain.resize(static_cast<std::size_t>(plain_len));
  return plain;
}

}  // namespace detail

/// 対応表を読む。平文でも暗号化でも受ける。
///
/// 暗号化されていてパスフレーズが空なら NeedsPassphrase を投げる。呼び出し側（UI/CLI）が
/// 入力を促してから渡し直す。
inline std::vector<Entry> parse(const std::string& body, std::string_view passphrase) {
  const std::string s = detail::strip_bom(body);
  const nlohmann::json meta = detail::first_meta(s);
  if (meta.is_object() && meta.contains("version") && meta["version"].is_number_integer()) {
    const int v = meta["version"].get<int>();
    if (v > FORMAT_VERSION_ENCRYPTED)
      throw std::runtime_error("この対応表は新しい形式です（version " + std::to_string(v) +
                               "、本バージョンは " + std::to_string(FORMAT_VERSION_ENCRYPTED) +
                               " まで対応）。ツールを更新してください。");
    if (v == FORMAT_VERSION_ENCRYPTED) {
      if (passphrase.empty()) throw NeedsPassphrase();
      std::string plain = detail::decrypt_envelope(s, meta, passphrase);
      auto out = parse_plaintext(plain);
      sodium_memzero(plain.data(), plain.size());
      return out;
    }
    if (v <= 0) throw std::runtime_error("対応表の _meta.version が読めません。");
  }
  return parse_plaintext(s);
}

inline std::vector<Entry> parse(const std::string& body) { return parse(body, std::string_view()); }

/// 中身を復号せずに「パスフレーズが要るか」だけを判定する（UI の出し分け用）。
/// 壊れたファイルでも投げない——実際の診断は parse() に任せる。
inline bool is_encrypted(const std::string& body) {
  const nlohmann::json meta = detail::first_meta(detail::strip_bom(body));
  return meta.is_object() && meta.value("version", 0) == FORMAT_VERSION_ENCRYPTED;
}

}  // namespace mapping_io
