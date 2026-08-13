// tokenizer-shim（Rust cdylib）の C++ ラッパ。
//
// tokenizers-cpp を使わない理由（cpp/README.md「実測メモ」）:
//   - 公開 Encode(text) が add_special_tokens=false 決め打ち（<s>/</s> が付かない）。
//   - **offsets を FFI が公開していない。** HF の集約は文字オフセットを使うので必須。
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
struct TokHandle;
struct TokEncodingC {
  std::uint32_t* ids;
  std::size_t* begins;
  std::size_t* ends;
  std::uint8_t* special;
  std::size_t len;
};
TokHandle* tok_open(const char* path);
int tok_encode(TokHandle* handle, const char* text, int add_special_tokens, TokEncodingC* out);
void tok_free_encoding(TokEncodingC* enc);
void tok_close(TokHandle* handle);
}

namespace tok {

struct Encoding {
  std::vector<std::int64_t> ids;
  std::vector<std::size_t> begins;  // 文字（コードポイント）単位
  std::vector<std::size_t> ends;
  std::vector<bool> special;        // <s>/</s> なら true（HF の集約は読み飛ばす）
};

class Tokenizer {
 public:
  explicit Tokenizer(const std::string& tokenizer_json) {
    h_ = tok_open(tokenizer_json.c_str());
    if (!h_) throw std::runtime_error("tok_open に失敗: " + tokenizer_json);
  }
  ~Tokenizer() {
    if (h_) tok_close(h_);
  }
  Tokenizer(const Tokenizer&) = delete;
  Tokenizer& operator=(const Tokenizer&) = delete;

  /// add_special_tokens は既定 true。xlm-roberta は <s>/</s> を前提に学習されており、
  /// false だと推論が変わる（Phase 0 で tokenizers-cpp の決め打ちにより 0/39 になった）。
  Encoding encode(const std::string& text, bool add_special_tokens = true) {
    TokEncodingC c{};
    if (tok_encode(h_, text.c_str(), add_special_tokens ? 1 : 0, &c) != 0) {
      throw std::runtime_error("tok_encode に失敗");
    }
    Encoding e;
    e.ids.assign(c.ids, c.ids + c.len);
    e.begins.assign(c.begins, c.begins + c.len);
    e.ends.assign(c.ends, c.ends + c.len);
    e.special.reserve(c.len);
    for (std::size_t i = 0; i < c.len; ++i) e.special.push_back(c.special[i] != 0);
    tok_free_encoding(&c);
    return e;
  }

 private:
  TokHandle* h_ = nullptr;
};

}  // namespace tok
