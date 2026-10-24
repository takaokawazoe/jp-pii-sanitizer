// ヘルパーの通信プロトコル（docs/excel-helper.md）。トランスポート非依存。
//
// フレーム = 4 バイトのリトルエンディアン長さ ＋ その長さぶんの UTF-8 JSON。
// 要求は {"id":n,"cmd":"...",...}、応答は {"id":n,"type":"<cmd>","ok":bool,...}。
// コマンドと応答の形は GUI（winmain.cpp の dispatch_command）に揃えてある——
// 違うのは「ファイルのパス」の代わりに「セルの配列」を受け取ることだけ。
//
// JSONL ではなく長さ付きにしたのは、.NET 側の読み手を `ReadInt32` ＋ `ReadBytes` の
// 2 呼び出しにするため。中身が JSON である以上、改行区切りでも動くが、「改行を含む
// 文字列が来たら壊れる」という種類の心配を最初から消しておく。
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <sodium.h>

#include "file_io.hpp"
#include "helper/session.hpp"
#include "json.hpp"

namespace helper::proto {

using nlohmann::json;

inline constexpr int PROTOCOL_VERSION = 1;

/// 1 フレームの上限。シート 1 枚ぶんのセルが収まれば十分で、それ以上は読み手の
/// メモリを食い潰す以外の意味を持たない（パイプは本人限定だが、上限は無料）。
inline constexpr std::uint32_t MAX_FRAME_BYTES = 256u * 1024u * 1024u;

/// バイト列の読み書き。トランスポート（stdio／名前付きパイプ／メモリ）が実装する。
struct Stream {
  virtual ~Stream() = default;
  /// n バイトちょうど読めたら true。EOF・エラーなら false。
  virtual bool read_exact(void* buf, std::size_t n) = 0;
  /// n バイト全部書けたら true。
  virtual bool write_all(const void* buf, std::size_t n) = 0;
};

/// 1 フレーム読む。EOF（正常終了）・上限超過・途中切断で false。
inline bool read_frame(Stream& s, std::string& out) {
  unsigned char hdr[4];
  if (!s.read_exact(hdr, 4)) return false;
  const std::uint32_t n = static_cast<std::uint32_t>(hdr[0]) |
                          (static_cast<std::uint32_t>(hdr[1]) << 8) |
                          (static_cast<std::uint32_t>(hdr[2]) << 16) |
                          (static_cast<std::uint32_t>(hdr[3]) << 24);
  if (n > MAX_FRAME_BYTES) return false;
  out.assign(n, '\0');
  return n == 0 || s.read_exact(out.data(), n);
}

inline bool write_frame(Stream& s, const std::string& payload) {
  if (payload.size() > MAX_FRAME_BYTES) return false;
  const auto n = static_cast<std::uint32_t>(payload.size());
  const unsigned char hdr[4] = {static_cast<unsigned char>(n & 0xFF),
                                static_cast<unsigned char>((n >> 8) & 0xFF),
                                static_cast<unsigned char>((n >> 16) & 0xFF),
                                static_cast<unsigned char>((n >> 24) & 0xFF)};
  return s.write_all(hdr, 4) && (n == 0 || s.write_all(payload.data(), n));
}

/// 使い終わったパスフレーズを消す。スワップ・休止までは届かない best-effort。
inline void wipe(std::string& s) {
  if (!s.empty()) sodium_memzero(s.data(), s.size());
  s.clear();
}

inline std::vector<tokenizer::ConfirmedTerm> parse_confirmed(const json& arr) {
  std::vector<tokenizer::ConfirmedTerm> conf;
  if (arr.is_array())
    for (const auto& x : arr)
      if (x.is_object())
        conf.push_back({x.value("text", std::string()), x.value("type", std::string())});
  return conf;
}

inline std::vector<Cell> parse_cells(const json& arr) {
  std::vector<Cell> cells;
  if (arr.is_array())
    for (const auto& x : arr) {
      if (!x.is_object()) continue;
      Cell c;
      c.row = x.value("r", 0);
      c.col = x.value("c", 0);
      c.text = x.value("text", std::string());
      cells.push_back(std::move(c));
    }
  return cells;
}

/// 配列フィールドを安全に取り出す（欠けていれば空配列）。
/// 素の `req["key"]` は欠けたキーで null を作り、それを回すと type_error を投げる。
inline const json& array_or_empty(const json& req, const char* key) {
  static const json kEmpty = json::array();
  const auto it = req.find(key);
  return (it != req.end() && it->is_array()) ? *it : kEmpty;
}

/// 要求を Session に配線する。1 接続 = 1 Server。
class Server {
 public:
  explicit Server(Session& session) : s_(session) {}

  bool shutdown_requested() const { return shutdown_; }

  /// **ここが最後の例外の受け皿。** 何があっても応答を返して生き延びる。
  /// 壊れた JSON は id 無しのエラー応答にする（黙って捨てると相手が待ち続ける）。
  std::string handle_raw(const std::string& raw) {
    json req;
    try {
      req = json::parse(raw);
      if (!req.is_object()) throw std::runtime_error("要求はオブジェクトである必要があります");
    } catch (const std::exception& e) {
      json resp;
      resp["id"] = 0;
      resp["type"] = "error";
      resp["ok"] = false;
      resp["error"] = std::string("要求を解釈できません: ") + e.what();
      return resp.dump();
    }
    const std::string cmd = req.value("cmd", std::string());
    const int id = req.value("id", 0);
    json resp;
    try {
      resp = handle(req, cmd);
    } catch (const std::exception& e) {
      resp = json{{"ok", false}, {"error", std::string("処理中にエラーが発生しました: ") + e.what()}};
    } catch (...) {
      resp = json{{"ok", false}, {"error", "処理中に不明なエラーが発生しました"}};
    }
    // 要求に乗ってきたパスフレーズは届く範囲で消す（応答を組んだ後）。
    if (auto it = req.find("passphrase"); it != req.end() && it->is_string())
      wipe(it->get_ref<std::string&>());
    resp["id"] = id;
    resp["type"] = cmd.empty() ? std::string("error") : cmd;
    return resp.dump();
  }

  /// 1 接続ぶんの要求を順に処理する。EOF・壊れたフレーム・shutdown で戻る。
  /// 戻り値は shutdown が要求されたか（＝プロセスを終えてよいか）。
  bool serve(Stream& st) {
    std::string raw;
    while (read_frame(st, raw)) {
      const std::string resp = handle_raw(raw);
      wipe(raw);  // パスフレーズを含みうる生バッファ
      if (!write_frame(st, resp)) break;
      if (shutdown_) return true;
    }
    return shutdown_;
  }

 private:
  json handle(const json& req, const std::string& cmd) {
    if (cmd == "hello") {
      // 接続確認。モデルは読まない（init で明示的に読む——重いので相手が進捗を出せるように）。
      return json{{"ok", true}, {"protocol", PROTOCOL_VERSION}, {"ready", s_.ready()}};
    }
    if (cmd == "init") {
      if (!s_.ensure_init()) return json{{"ok", false}, {"error", s_.last_error()}};
      return json{{"ok", true}};
    }
    if (cmd == "load") return s_.load(parse_cells(array_or_empty(req, "cells")));
    if (cmd == "preview") return s_.preview(parse_confirmed(array_or_empty(req, "confirmed")));
    if (cmd == "sanitize")
      return s_.sanitize(parse_confirmed(array_or_empty(req, "confirmed")),
                         req.value("reversible", true));
    if (cmd == "saveMapping") return save_mapping(req);
    if (cmd == "reverse") return reverse(req);
    if (cmd == "reset") {
      s_.clear();
      return json{{"ok", true}};
    }
    if (cmd == "shutdown") {
      s_.clear();
      shutdown_ = true;
      return json{{"ok", true}};
    }
    return json{{"ok", false}, {"error", cmd.empty() ? std::string("cmd がありません")
                                                     : "未知のコマンド: " + cmd}};
  }

  /// 対応表はヘルパーが直接シリアライズして書く。アドインには中身を渡さない
  /// （GUI の saveMapping と同じ: 書式を 1 本に保ち、対応表を Excel のプロセスに乗せない）。
  /// 保存先はアドインがダイアログで選び、パスだけが来る。
  json save_mapping(const json& req) {
    std::string pass = req.value("passphrase", std::string());
    const std::string path = req.value("path", std::string());
    json resp;
    if (!s_.has_mapping()) {
      resp["ok"] = false;
      resp["error"] = "保存できる対応表がありません（可逆モードでサニタイズしてください）。";
    } else if (path.empty()) {
      resp["ok"] = false;
      resp["error"] = "保存先 path がありません。";
    } else {
      try {
        // fail closed: 暗号化に失敗したら平文で書き出さない。
        const std::string body = pass.empty() ? s_.mapping_jsonl() : s_.mapping_jsonl_encrypted(pass);
        std::ofstream out = fileio::open_write_private(path);
        if (!out) throw std::runtime_error("書き込めません: " + path);
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!out.good()) throw std::runtime_error("書き込みに失敗しました: " + path);
        resp["ok"] = true;
        resp["encrypted"] = !pass.empty();
      } catch (const std::exception& e) {
        resp["ok"] = false;
        resp["error"] = std::string("対応表を保存できませんでした: ") + e.what();
      }
    }
    wipe(pass);
    return resp;
  }

  /// 対応表はパス（ヘルパーが読む。中身はアドインに渡らない）か貼り付けテキストで受ける。
  /// 戻す対象はテキスト（AI の返答）かセル配列（AI が返した表）。
  json reverse(const json& req) {
    std::string pass = req.value("passphrase", std::string());
    std::string body;
    const std::string mpath = req.value("mappingPath", std::string());
    if (!mpath.empty()) {
      body = fileio::read_all(mpath);
      if (body.empty()) {
        wipe(pass);
        return json{{"ok", false}, {"error", "対応表を読めませんでした: " + mpath}};
      }
    } else {
      body = req.value("mappingText", std::string());
    }
    json resp;
    if (req.contains("cells"))
      resp = s_.reverse_cells(parse_cells(array_or_empty(req, "cells")), body, pass);
    else
      resp = s_.reverse_text(req.value("text", std::string()), body, pass);
    wipe(pass);
    wipe(body);  // 対応表の平文
    return resp;
  }

  Session& s_;
  bool shutdown_ = false;
};

}  // namespace helper::proto
