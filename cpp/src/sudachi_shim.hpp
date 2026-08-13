// sudachi-shim（Rust cdylib）の C++ ラッパ。
//
// Rust の C ABI を直接宣言して叩く（Phase 0 の tokenizers で確立した流儀）。
// RAII でハンドルと結果を管理し、呼び出し側が free を忘れられないようにする。
#pragma once

#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
struct SudachiHandle;
struct SudachiMorphemeC {
  char* surface;
  char* pos;  // 6要素の品詞をタブ区切りで連結したもの
  char* reading;
  std::size_t begin;  // 文字（コードポイント）単位
  std::size_t end;    // 文字（コードポイント）単位
};
struct SudachiResultC {
  SudachiMorphemeC* morphemes;
  std::size_t len;
};
SudachiHandle* sudachi_open(const char* config_path, const char* resource_dir,
                            const char* system_dic);
int sudachi_tokenize(SudachiHandle* handle, const char* text, int mode, SudachiResultC* out);
void sudachi_free_result(SudachiResultC* res);
void sudachi_close(SudachiHandle* handle);
}

namespace sudachi {

struct Morpheme {
  std::string surface;
  std::vector<std::string> pos;  // SudachiPy の part_of_speech() と同じ6要素
  std::string reading;
  std::size_t begin;  // 文字（コードポイント）単位。Python の m.begin() と同じ意味
  std::size_t end;
};

/// 辞書＋トークナイザ。
///
/// **リソースは SudachiPy が使っているものと同じ実体を渡すこと。** 設定
/// （sudachi.json）に ProlongedSoundMark / IgnoreYomigana / OOV 等のプラグインが並んでおり、
/// これが違うと分割結果が変わって Python 版と一致しなくなる。
class Analyzer {
 public:
  Analyzer(const std::string& config, const std::string& resource_dir,
           const std::string& system_dic) {
    h_ = sudachi_open(config.c_str(), resource_dir.c_str(), system_dic.c_str());
    if (!h_) throw std::runtime_error("sudachi_open に失敗: " + config + " / " + system_dic);
  }
  ~Analyzer() {
    if (h_) sudachi_close(h_);
  }
  Analyzer(const Analyzer&) = delete;
  Analyzer& operator=(const Analyzer&) = delete;

  /// mode: 0=A, 1=B, 2=C。furigana.py は SplitMode.C を使うので既定は C。
  std::vector<Morpheme> tokenize(const std::string& text, int mode = 2) {
    SudachiResultC res{};
    if (sudachi_tokenize(h_, text.c_str(), mode, &res) != 0) {
      throw std::runtime_error("sudachi_tokenize に失敗: " + text);
    }
    std::vector<Morpheme> out;
    out.reserve(res.len);
    for (std::size_t i = 0; i < res.len; ++i) {
      Morpheme m;
      m.surface = res.morphemes[i].surface;
      m.reading = res.morphemes[i].reading;
      m.begin = res.morphemes[i].begin;
      m.end = res.morphemes[i].end;
      std::istringstream ss(res.morphemes[i].pos);
      std::string part;
      while (std::getline(ss, part, '\t')) m.pos.push_back(part);
      out.push_back(std::move(m));
    }
    sudachi_free_result(&res);
    return out;
  }

 private:
  SudachiHandle* h_ = nullptr;
};

}  // namespace sudachi
