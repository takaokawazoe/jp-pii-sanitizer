// Phase 0 の完成条件を判定する適合試験。
//
// testdata/ref.json は Python 側(torch/transformers)が出した「正解」:
//   39チャンク / 4873トークン ぶんの input_ids と予測ラベルID列。
// 同じ input_ids を ONNX Runtime C++ に食わせ、ラベル列が完全一致するかを見る。
// 一致すれば「C++で日本語NERの推論が動く」が確定する。
//
// 使い方: phase0_parity <model.onnx> [ref.json] [labels.json]

#include <onnxruntime_cxx_api.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "json.hpp"
#include "ort_path.hpp"

using json = nlohmann::json;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <model.onnx> [ref.json] [labels.json]\n", argv[0]);
    return 2;
  }
  const std::string model_path = argv[1];
  const std::string ref_path = (argc > 2) ? argv[2] : "testdata/ref.json";
  const std::string labels_path = (argc > 3) ? argv[3] : "models/labels.json";

  json ref;
  {
    std::ifstream f(ref_path);
    if (!f) {
      std::fprintf(stderr, "ref.json を開けません: %s\n", ref_path.c_str());
      return 2;
    }
    f >> ref;
  }
  std::map<int, std::string> labels;
  {
    std::ifstream f(labels_path);
    if (f) {
      json j;
      f >> j;
      for (auto it = j.begin(); it != j.end(); ++it)
        labels[std::stoi(it.key())] = it.value().get<std::string>();
    }
  }

  std::printf("model : %s\n", model_path.c_str());
  std::printf("ref   : %s (%zu チャンク)\n", ref_path.c_str(), ref.size());

  Ort::Env env(ORT_LOGGING_LEVEL_FATAL, "phase0");

  // fp16 モデルは SimplifiedLayerNormFusion が fp16変換で挿入された Cast と衝突して
  // ロードに失敗する。回避策を上から順に試し、最初に通ったものを使う（どれが効くかを
  // 実測で確定させるため。リスク項目）。
  struct Strategy {
    const char* name;
    GraphOptimizationLevel level;
    const char* disable_list;  // nullptr なら config entry を付けない
  };
  const Strategy strategies[] = {
      {"ENABLE_ALL + disable_specified_optimizers", GraphOptimizationLevel::ORT_ENABLE_ALL,
       "SimplifiedLayerNormFusion,LayerNormFusion"},
      {"ENABLE_ALL (素)", GraphOptimizationLevel::ORT_ENABLE_ALL, nullptr},
      {"ENABLE_EXTENDED", GraphOptimizationLevel::ORT_ENABLE_EXTENDED, nullptr},
      {"ENABLE_BASIC", GraphOptimizationLevel::ORT_ENABLE_BASIC, nullptr},
      {"DISABLE_ALL", GraphOptimizationLevel::ORT_DISABLE_ALL, nullptr},
  };

  std::unique_ptr<Ort::Session> session;
  std::string used;
  for (const auto& s : strategies) {
    Ort::SessionOptions so;
    so.SetGraphOptimizationLevel(s.level);
    if (s.disable_list)
      so.AddConfigEntry("optimization.disable_specified_optimizers", s.disable_list);
    try {
      session = std::make_unique<Ort::Session>(env, ortpath::to_ort(model_path).c_str(), so);
      used = s.name;
      std::printf("  最適化戦略: %s  → ロード成功\n", s.name);
      break;
    } catch (const Ort::Exception&) {
      std::printf("  最適化戦略: %s  → ロード失敗\n", s.name);
    }
  }
  if (!session) {
    std::fprintf(stderr, "どの戦略でもセッションを初期化できませんでした\n");
    return 1;
  }

  auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  const char* in_names[] = {"input_ids", "attention_mask"};
  const char* out_names[] = {"logits"};

  size_t same = 0, diff = 0;
  int shown = 0;
  const auto t0 = std::chrono::steady_clock::now();

  for (const auto& r : ref) {
    auto ids = r["input_ids"].get<std::vector<int64_t>>();
    auto am = r["attention_mask"].get<std::vector<int64_t>>();
    auto pred = r["pred"].get<std::vector<int64_t>>();
    const int64_t shape[2] = {1, static_cast<int64_t>(ids.size())};

    std::vector<Ort::Value> inputs;
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(mem, ids.data(), ids.size(), shape, 2));
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(mem, am.data(), am.size(), shape, 2));

    auto out = session->Run(Ort::RunOptions{nullptr}, in_names, inputs.data(), 2, out_names, 1);
    const auto oshape = out[0].GetTensorTypeAndShapeInfo().GetShape();  // [1, seq, nlab]
    const float* logits = out[0].GetTensorData<float>();
    const int64_t seq = oshape[1], nlab = oshape[2];

    for (int64_t t = 0; t < seq && t < static_cast<int64_t>(pred.size()); ++t) {
      int64_t best = 0;
      float bv = logits[t * nlab];
      for (int64_t c = 1; c < nlab; ++c) {
        const float v = logits[t * nlab + c];
        if (v > bv) { bv = v; best = c; }
      }
      if (best == pred[t]) {
        ++same;
      } else {
        ++diff;
        if (shown < 8) {
          ++shown;
          std::printf("    差分 %s#%d tok%lld: torch=%s onnx=%s\n",
                      r["doc"].get<std::string>().c_str(), r["chunk"].get<int>(),
                      static_cast<long long>(t), labels.count(pred[t]) ? labels[pred[t]].c_str() : "?",
                      labels.count(best) ? labels[best].c_str() : "?");
        }
      }
    }
  }

  const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const size_t total = same + diff;
  std::printf("\n  ラベル一致 %zu/%zu (不一致 %zu)\n", same, total, diff);
  std::printf("  推論 %.1fs / %zu チャンク = %.0f ms/chunk\n", dt, ref.size(),
              dt / ref.size() * 1000.0);
  std::printf("\n  === Phase 0 完成条件: %s ===\n",
              (diff == 0 && total > 0) ? "PASS (torch と完全一致)" : "FAIL");
  return (diff == 0 && total > 0) ? 0 : 1;
}
