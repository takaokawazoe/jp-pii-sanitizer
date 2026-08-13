// jp-pii-sanitizer CLI (Phase A: prose documents).
//
// Subcommands:
//   detect  <files...> [-o cand.jsonl]                 candidates -> JSONL
//   mask    <files...> [--candidates cand.jsonl]        sanitize -> text
//                      [--reversible --mapping map.jsonl] [-o out.txt]
//   restore --mapping map.jsonl [-i in.txt] [-o out.txt]   token text -> real names
//
// The candidate JSONL is the authority: `mask --candidates f` masks exactly the
// terms in f (+ the always-automatic email/phone/postal), no model needed.
// `mask` with no candidates auto-detects and masks every candidate (opt-out).
// Human review is the user's workflow (edit the JSONL between detect and mask).
//
// Reuses the library headers directly (not the GUI facade core.hpp), so it also
// ingests .msg (this TU pulls in Aspose) and builds without WebView2 -> the CLI
// is cross-platform. The per-doc candidate merge mirrors appcore::Core::extract.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "extractors.hpp"
#include "file_io.hpp"
#include "hf_ner.hpp"
#include "json.hpp"
#include "msg.hpp"
#include "ooxml.hpp"
#include "pdf.hpp"
#include "step2.hpp"
#include "step5.hpp"
#include "sudachi_shim.hpp"
#include "tokenizer.hpp"
#include "utf8.hpp"

namespace {

using nlohmann::json;

// ---------------------------------------------------------------- utilities
std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}
std::string ext_of(const std::string& path) {
  const auto slash = path.find_last_of("/\\");
  const auto dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
  return to_lower(path.substr(dot + 1));
}
std::string basename_of(const std::string& path) {
  const auto slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string exe_dir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return ".";
  std::wstring w(buf, n);
  const auto slash = w.find_last_of(L"\\/");
  std::wstring dir = (slash == std::wstring::npos) ? L"." : w.substr(0, slash);
  int len = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string out(len > 0 ? len - 1 : 0, '\0');
  if (len > 0) WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, out.data(), len, nullptr, nullptr);
  return out.empty() ? "." : out;
#else
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return ".";
  std::string p(buf, static_cast<std::size_t>(n));
  const auto slash = p.find_last_of('/');
  return slash == std::string::npos ? "." : p.substr(0, slash);
#endif
}

bool file_exists(const std::string& p) {
  std::ifstream f(p);
  return f.good();
}

// models ディレクトリ: --data > <exe_dir>/models > ./models
std::string resolve_data_root(const std::string& override_dir) {
  if (!override_dir.empty()) return override_dir;
  const std::string beside = exe_dir() + "/models";
  if (file_exists(beside + "/patterns.json")) return beside;
  if (file_exists("models/patterns.json")) return "models";
  return beside;  // 失敗時のエラーメッセージ用にとりあえず返す
}

std::string read_stream(std::istream& in) {
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// ---------------------------------------------------------------- engine
// 必要なぶんだけ遅延ロードする（restore は何も要らない・mask --candidates は sudachi のみ）。
struct Engine {
  std::string data_root;
  std::map<std::string, std::string> label_map;
  std::unique_ptr<step2::Patterns> patterns;
  std::unique_ptr<sudachi::Analyzer> sd;
  std::unique_ptr<hf::Ner> ner;

  explicit Engine(std::string root) : data_root(std::move(root)) {}

  void load_patterns() {
    if (patterns) return;
    const std::string pj = fileio::read_all(data_root + "/patterns.json");
    if (pj.empty()) throw std::runtime_error("patterns.json が読めません: " + data_root);
    const json pjson = json::parse(pj);
    std::map<std::string, std::string> pats;
    for (auto it = pjson["patterns"].begin(); it != pjson["patterns"].end(); ++it)
      pats[it.key()] = it.value().get<std::string>();
    patterns = std::make_unique<step2::Patterns>(pats);
    for (auto it = pjson["label_map"].begin(); it != pjson["label_map"].end(); ++it)
      label_map[it.key()] = it.value().get<std::string>();
  }
  void load_sudachi() {
    if (sd) return;
    const std::string res = data_root + "/sudachi/resources";
    sd = std::make_unique<sudachi::Analyzer>(res + "/sudachi.json", res,
                                             data_root + "/sudachi/system.dic");
  }
  void load_ner() {
    if (ner) return;
    load_patterns();
    ner = std::make_unique<hf::Ner>(data_root + "/model_fp16.onnx",
                                    data_root + "/tokenizer.json",
                                    data_root + "/labels.json", label_map);
  }
};

// ---------------------------------------------------------------- ingestion
step5::FileResult ingest(const std::string& path) {
  step5::FileResult fr;
  fr.source = basename_of(path);
  try {
    const std::string e = ext_of(path);
    if (e == "docx") { fr.kind = "docx"; fr.text = ooxml::extract_docx(path); }
    else if (e == "pptx") { fr.kind = "pptx"; fr.text = ooxml::extract_pptx(path); }
    else if (e == "xlsx") { fr.kind = "xlsx-prose"; fr.text = ooxml::read_xlsx_prose(path); }
    else if (e == "pdf") { fr.kind = "pdf"; fr.text = pdf::extract_pdf(path); }
    else if (e == "txt" || e == "csv" || e == "md") { fr.kind = "text"; fr.text = extractors::extract_plain(path); }
    else if (e == "msg") {
      const auto r = msg::extract_msg(path);
      fr.kind = "msg";
      fr.text = r.text;
      for (const auto& c : r.children)  // 添付ファイル名の PII を取りこぼさない
        if (!c.filename.empty()) fr.text += "\n【添付】" + c.filename;
    }
    else { fr.kind = "skipped"; fr.text = ""; }
  } catch (const std::exception& ex) {
    fr.kind = "skipped";
    fr.text = std::string("（抽出に失敗: ") + ex.what() + "）";
  }
  return fr;
}

// ---------------------------------------------------------------- detection
// per-doc に extract_candidates し「無空白キー×種別」で集約する。
// appcore::Core::extract の集約と揃える（挙動を変えないこと）。
std::vector<step2::Candidate> detect_candidates(Engine& eng,
                                                const std::vector<step5::FileResult>& files) {
  eng.load_ner();
  eng.load_sudachi();
  struct Agg { step2::Candidate cand; int best; };
  std::vector<Agg> agg;
  std::map<std::pair<std::string, std::string>, std::size_t> aidx;
  for (const auto& fr : files) {
    if (fr.text.empty() || fr.kind == "skipped") continue;
    for (const auto& c : step2::extract_candidates(*eng.patterns, *eng.sd, *eng.ner, fr.text)) {
      const auto key = std::make_pair(step2::strip_all_ws(*eng.patterns, c.text), c.entity_type);
      auto it = aidx.find(key);
      if (it == aidx.end()) {
        aidx[key] = agg.size();
        agg.push_back({c, c.count});
      } else {
        auto& a = agg[it->second];
        a.cand.count += c.count;
        if (c.count > a.best ||
            (c.count == a.best && utf8::char_len(c.text) < utf8::char_len(a.cand.text))) {
          a.cand.text = c.text;
          a.best = c.count;
        }
      }
    }
  }
  std::map<std::string, int> torder;
  for (std::size_t i = 0; i < step2::CANDIDATE_ENTITY_TYPES.size(); ++i)
    torder[step2::CANDIDATE_ENTITY_TYPES[i]] = static_cast<int>(i);
  std::vector<step2::Candidate> out;
  for (auto& a : agg) out.push_back(std::move(a.cand));
  std::sort(out.begin(), out.end(), [&](const step2::Candidate& x, const step2::Candidate& y) {
    const int tx = torder.count(x.entity_type) ? torder[x.entity_type] : 99;
    const int ty = torder.count(y.entity_type) ? torder[y.entity_type] : 99;
    if (tx != ty) return tx < ty;
    if (x.count != y.count) return x.count > y.count;
    return x.text < y.text;
  });
  return out;
}

// ---------------------------------------------------------------- JSONL i/o
void write_candidates_jsonl(std::ostream& out, const std::vector<step2::Candidate>& cands) {
  for (const auto& c : cands) {
    json j = {{"type", c.entity_type}, {"text", c.text}, {"count", c.count}};
    out << j.dump() << "\n";
  }
}

std::vector<tokenizer::ConfirmedTerm> read_candidates_jsonl(const std::string& path) {
  const std::string body = fileio::read_all(path);
  if (body.empty() && !file_exists(path))
    throw std::runtime_error("候補ファイルが読めません: " + path);
  std::vector<tokenizer::ConfirmedTerm> out;
  std::istringstream in(body);
  std::string line;
  std::size_t ln = 0;
  while (std::getline(in, line)) {
    ++ln;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string t = line;
    // 先頭空白を除いて空行・# コメントをスキップ
    std::size_t s = t.find_first_not_of(" \t");
    if (s == std::string::npos) continue;
    if (t[s] == '#') continue;
    try {
      const json j = json::parse(t);
      const std::string type = j.value("type", std::string());
      const std::string text = j.value("text", std::string());
      if (type.empty() || text.empty()) continue;
      out.push_back({text, type});
    } catch (const std::exception& ex) {
      throw std::runtime_error("候補JSONL " + std::to_string(ln) + " 行目を解釈できません: " + ex.what());
    }
  }
  return out;
}

void write_mapping_jsonl(std::ostream& out,
                         const std::vector<std::pair<std::string, std::string>>& value_token) {
  for (const auto& [value, token] : value_token) {
    json j = {{"token", token}, {"original", value}};
    out << j.dump() << "\n";
  }
}

// 返り: value -> token（load_mapping が要求する並び）
std::vector<std::pair<std::string, std::string>> read_mapping_jsonl(const std::string& path) {
  const std::string body = fileio::read_all(path);
  if (body.empty() && !file_exists(path))
    throw std::runtime_error("対応表が読めません: " + path);
  std::vector<std::pair<std::string, std::string>> value_token;
  std::istringstream in(body);
  std::string line;
  std::size_t ln = 0;
  while (std::getline(in, line)) {
    ++ln;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::size_t s = line.find_first_not_of(" \t");
    if (s == std::string::npos || line[s] == '#') continue;
    try {
      const json j = json::parse(line);
      const std::string token = j.value("token", std::string());
      const std::string original = j.value("original", std::string());
      if (token.empty() || original.empty()) continue;
      value_token.emplace_back(original, token);
    } catch (const std::exception& ex) {
      throw std::runtime_error("対応表JSONL " + std::to_string(ln) + " 行目を解釈できません: " + ex.what());
    }
  }
  return value_token;
}

// ---------------------------------------------------------------- output sink
// -o 指定ならファイル、無ければ stdout。
struct OutSink {
  std::ofstream file;
  std::ostream* os = &std::cout;
  explicit OutSink(const std::string& path) {
    if (!path.empty()) {
      file.open(path, std::ios::binary);
      if (!file) throw std::runtime_error("出力を開けません: " + path);
      os = &file;
    }
  }
  std::ostream& stream() { return *os; }
};

// ---------------------------------------------------------------- arg parsing
struct Args {
  std::string sub;
  std::vector<std::string> files;
  std::string out, candidates, mapping, input, data;
  bool reversible = false;
};

[[noreturn]] void die(const std::string& msg) {
  std::fprintf(stderr, "error: %s\n", msg.c_str());
  std::exit(2);
}

Args parse_args(int argc, char** argv) {
  Args a;
  if (argc < 2) die("サブコマンドが必要です（detect | mask | restore）");
  a.sub = argv[1];
  auto need = [&](int& i) -> std::string {
    if (i + 1 >= argc) die(std::string(argv[i]) + " に値が必要です");
    return argv[++i];
  };
  for (int i = 2; i < argc; ++i) {
    std::string t = argv[i];
    if (t == "-o" || t == "--output") a.out = need(i);
    else if (t == "--candidates" || t == "-c") a.candidates = need(i);
    else if (t == "--mapping" || t == "-m") a.mapping = need(i);
    else if (t == "--input" || t == "-i") a.input = need(i);
    else if (t == "--data") a.data = need(i);
    else if (t == "--reversible") a.reversible = true;
    else if (!t.empty() && t[0] == '-') die("不明なオプション: " + t);
    else a.files.push_back(t);
  }
  return a;
}

// ---------------------------------------------------------------- subcommands
int cmd_detect(const Args& a) {
  if (a.files.empty()) die("detect: 対象ファイルを指定してください");
  Engine eng(resolve_data_root(a.data));
  std::vector<step5::FileResult> files;
  for (const auto& p : a.files) {
    auto fr = ingest(p);
    if (fr.kind == "skipped")
      std::fprintf(stderr, "warning: スキップ: %s\n", basename_of(p).c_str());
    files.push_back(std::move(fr));
  }
  const auto cands = detect_candidates(eng, files);
  OutSink sink(a.out);
  write_candidates_jsonl(sink.stream(), cands);
  std::fprintf(stderr, "detect: 候補 %zu 件\n", cands.size());
  return 0;
}

int cmd_mask(const Args& a) {
  if (a.files.empty()) die("mask: 対象ファイルを指定してください");
  Engine eng(resolve_data_root(a.data));
  std::vector<step5::FileResult> files;
  for (const auto& p : a.files) {
    auto fr = ingest(p);
    if (fr.kind == "skipped")
      std::fprintf(stderr, "warning: スキップ: %s\n", basename_of(p).c_str());
    files.push_back(std::move(fr));
  }

  std::vector<tokenizer::ConfirmedTerm> confirmed;
  if (!a.candidates.empty()) {
    confirmed = read_candidates_jsonl(a.candidates);  // 権威ファイル（モデル不要）
  } else {
    for (const auto& c : detect_candidates(eng, files))  // ワンショット自動（全マスク）
      confirmed.push_back({c.text, c.entity_type});
  }

  eng.load_sudachi();
  tokenizer::Tokenizer tok(a.reversible);
  const std::string bundle = step5::bundle_blocks(tok, files, eng.sd.get());
  const std::string sanitized = tok.tokenize(bundle, confirmed, eng.sd.get());

  std::vector<std::string> raw;
  for (const auto& [v, t] : tok.mapping_ordered()) raw.push_back(v);
  const auto leaks = tokenizer::safety_gate(sanitized, raw);

  { OutSink sink(a.out); sink.stream() << sanitized; }

  if (a.reversible) {
    if (a.mapping.empty()) {
      std::fprintf(stderr, "warning: --reversible ですが --mapping が無いので対応表を書き出しません（復元不可）\n");
    } else {
      std::ofstream mf(a.mapping, std::ios::binary);
      if (!mf) die("対応表を開けません: " + a.mapping);
      write_mapping_jsonl(mf, tok.mapping_ordered());
    }
  }

  if (!leaks.empty()) {
    std::fprintf(stderr, "error: 安全ゲート: 固有情報が残っています（%zu 件）:\n", leaks.size());
    for (const auto& s : leaks) std::fprintf(stderr, "  %s\n", s.c_str());
    return 3;
  }
  return 0;
}

int cmd_restore(const Args& a) {
  if (a.mapping.empty()) die("restore: --mapping が必要です");
  const auto value_token = read_mapping_jsonl(a.mapping);
  std::string text;
  if (!a.input.empty()) {
    text = fileio::read_all(a.input);
    if (text.empty() && !file_exists(a.input)) die("入力が読めません: " + a.input);
  } else {
    text = read_stream(std::cin);
  }
  tokenizer::Tokenizer tok;  // 可逆版
  tok.load_mapping(value_token);
  const std::string restored = tok.reverse(text);
  const auto left = tokenizer::find_unreplaced_tokens(restored);

  { OutSink sink(a.out); sink.stream() << restored; }

  if (!left.empty()) {
    std::fprintf(stderr, "warning: 未置換トークンが残っています（%zu 件）:\n", left.size());
    for (const auto& s : left) std::fprintf(stderr, "  %s\n", s.c_str());
  }
  return 0;
}

void print_usage() {
  std::fprintf(stderr,
    "jp-pii-sanitizer CLI\n"
    "  detect  <files...> [-o cand.jsonl] [--data DIR]\n"
    "  mask    <files...> [--candidates cand.jsonl] [--reversible --mapping map.jsonl]\n"
    "                     [-o out.txt] [--data DIR]\n"
    "  restore --mapping map.jsonl [-i in.txt] [-o out.txt]\n");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Args a = parse_args(argc, argv);
    if (a.sub == "detect") return cmd_detect(a);
    if (a.sub == "mask") return cmd_mask(a);
    if (a.sub == "restore") return cmd_restore(a);
    if (a.sub == "-h" || a.sub == "--help" || a.sub == "help") { print_usage(); return 0; }
    std::fprintf(stderr, "error: 不明なサブコマンド: %s\n", a.sub.c_str());
    print_usage();
    return 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
