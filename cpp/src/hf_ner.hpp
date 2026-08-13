// HF transformer NER（ONNX）＋ aggregation_strategy="simple" の移植。
//
// Python 側の `step2._hf_spans` に対応。tokenizer と ONNX は Phase 0 で等価性を実測済み
// （ids 39/39・offsets 39/39・ラベル 4873/4873）なので、ここは「集約」の写経が本体。
#pragma once

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include "json.hpp"
#include "ort_path.hpp"
#include "tokenizer_shim.hpp"
#include "utf8.hpp"

namespace hf {

struct Span {
  std::string type;  // PERSON / ORGANIZATION / LOCATION
  std::size_t begin;  // 原文の文字（コードポイント）位置
  std::size_t end;
  float score;
};

/// HF の entity_group 相当。`aggregation_strategy="simple"` の group_sub_entities は
/// `entity.split("-", 1)[-1]` を通すので、**ORG-P → "P" / ORG-O → "O"** になる。
/// （実測で確認。この挙動のせいで "ORG-P"/"ORG-O" をマップに書いても発火しない）
inline std::string entity_group(const std::string& label) {
  const auto i = label.find('-');
  return (i == std::string::npos) ? label : label.substr(i + 1);
}

class Ner {
 public:
  Ner(const std::string& model_path, const std::string& tokenizer_json,
      const std::string& labels_json, std::map<std::string, std::string> label_map)
      : tk_(tokenizer_json), label_map_(std::move(label_map)), env_(ORT_LOGGING_LEVEL_FATAL, "hf") {
    {
      std::ifstream f(labels_json);
      nlohmann::json j;
      f >> j;
      for (auto it = j.begin(); it != j.end(); ++it)
        id2label_[std::stoi(it.key())] = it.value().get<std::string>();
    }
    // fp16 は ORT_ENABLE_ALL だと SimplifiedLayerNormFusion と衝突してロードできない。
    // 通る戦略を上から探す（cpp/README.md「実測メモ」）。
    for (auto lvl : {GraphOptimizationLevel::ORT_ENABLE_ALL,
                     GraphOptimizationLevel::ORT_ENABLE_EXTENDED,
                     GraphOptimizationLevel::ORT_ENABLE_BASIC}) {
      Ort::SessionOptions so;
      so.SetGraphOptimizationLevel(lvl);
      try {
        sess_ = std::make_unique<Ort::Session>(env_, ortpath::to_ort(model_path).c_str(), so);
        break;
      } catch (const Ort::Exception&) {
      }
    }
    if (!sess_) throw std::runtime_error("ONNX セッションを初期化できません: " + model_path);
  }

  /// テキスト全体から NER スパンを返す（Python の `_hf_spans` 相当）。
  std::vector<Span> spans(const std::string& text) {
    std::vector<Span> out;
    for (const auto& [base, chunk] : utf8::chunk_by_chars(text, 250)) {
      for (auto s : chunk_spans(chunk)) {
        s.begin += base;
        s.end += base;
        out.push_back(s);
      }
    }
    return out;
  }

 private:
  /// 1チャンク分。HF postprocess（softmax → argmax → group → "O" 除去）を写す。
  std::vector<Span> chunk_spans(const std::string& chunk) {
    const auto enc = tk_.encode(chunk);
    const std::size_t n = enc.ids.size();
    std::vector<std::int64_t> mask(n, 1);
    const int64_t shape[2] = {1, static_cast<int64_t>(n)};

    auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<std::int64_t> ids = enc.ids;
    std::vector<Ort::Value> inputs;
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(mem, ids.data(), n, shape, 2));
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(mem, mask.data(), n, shape, 2));
    const char* in_names[] = {"input_ids", "attention_mask"};
    const char* out_names[] = {"logits"};
    auto res = sess_->Run(Ort::RunOptions{nullptr}, in_names, inputs.data(), 2, out_names, 1);

    const auto oshape = res[0].GetTensorTypeAndShapeInfo().GetShape();
    const float* logits = res[0].GetTensorData<float>();
    const int64_t seq = oshape[1], nlab = oshape[2];

    // トークンごとに softmax → argmax（HF postprocess と同じ）
    struct Tok {
      std::string label;
      float score;
      std::size_t begin, end;
    };
    std::vector<Tok> toks;
    for (int64_t t = 0; t < seq && t < static_cast<int64_t>(n); ++t) {
      if (enc.special[t]) continue;  // <s>/</s> は読み飛ばす（special_tokens_mask）
      const float* row = logits + t * nlab;
      const float mx = *std::max_element(row, row + nlab);
      float sum = 0.f;
      int64_t best = 0;
      for (int64_t c = 0; c < nlab; ++c) {
        sum += std::exp(row[c] - mx);
        if (row[c] > row[best]) best = c;
      }
      const float score = std::exp(row[best] - mx) / sum;
      toks.push_back({id2label_.at(static_cast<int>(best)), score, enc.begins[t], enc.ends[t]});
    }

    // group_entities: 連続する同一タグをまとめる。ラベルに B-/I- 接頭辞が無いモデルなので
    // get_tag は全て bi="I" 扱い → 「同じタグが続く限り結合」になる。
    std::vector<Span> out;
    std::size_t i = 0;
    while (i < toks.size()) {
      std::size_t j = i + 1;
      while (j < toks.size() && toks[j].label == toks[i].label) ++j;
      const auto group = entity_group(toks[i].label);
      if (group != "O") {  // ignore_labels
        const auto it = label_map_.find(group);
        if (it != label_map_.end()) {
          float sum = 0.f;
          for (std::size_t k = i; k < j; ++k) sum += toks[k].score;
          out.push_back({it->second, toks[i].begin, toks[j - 1].end,
                         sum / static_cast<float>(j - i)});
        }
      }
      i = j;
    }
    return out;
  }

  tok::Tokenizer tk_;
  std::map<int, std::string> id2label_;
  std::map<std::string, std::string> label_map_;
  Ort::Env env_;
  std::unique_ptr<Ort::Session> sess_;
};

}  // namespace hf
