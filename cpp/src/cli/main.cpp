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
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW（Windows は argv が cp932 なので UTF-16 から作り直す）
#else
#include <unistd.h>
#endif

#include "extractors.hpp"
#include "file_io.hpp"
#include "hf_ner.hpp"
#include "json.hpp"
#include "msg.hpp"
#include "numparse.hpp"
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

#ifdef _WIN32
std::string wide_to_utf8(const wchar_t* w) {
  if (!w) return {};
  const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 0) return {};
  std::string out(static_cast<std::size_t>(len - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
  return out;
}
#endif

std::string exe_dir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return ".";
  std::wstring w(buf, n);
  const auto slash = w.find_last_of(L"\\/");
  const std::wstring dir = (slash == std::wstring::npos) ? L"." : w.substr(0, slash);
  const std::string out = wide_to_utf8(dir.c_str());
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

// **素の std::ifstream で存在確認しないこと。** MSVC はナローパスを ANSI(cp932) と
// して解釈するため、日本語を含むパスで必ず false になる。fileio はワイド版で開く。
bool file_exists(const std::string& p) { return fileio::exists(p); }

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
    fr.text.clear();
    // **失敗理由を text に入れない。** text は bundle_blocks でそのまま出力へ束ねられるので、
    // 例外メッセージ（生テキストを含みうる）がマスク済みの本文に紛れ込む。
    fr.error = ex.what();
  }
  // テキスト化の出口で UTF-8 を保証する。入口の復号だけでは PDF の孤立サロゲートや
  // 壊れた OOXML を拾えず、下流で PCRE2 が黙って0件を返し Rust シムが例外を投げる。
  fr.text = utf8::repair(fr.text);
  return fr;
}

// ---------------------------------------------------------------- tables (Phase B)
// 表マスクは列単位（tokenize_table）。prose と同じ 1 つの Tokenizer に流すので対応表は
// 共有され、docx の田中と xlsx の田中が同じトークンになる（別呼び出しなら衝突していた）。
struct TableSpec {
  std::string sheet;   // xlsx: シート名（空=先頭）
  int header_row = 0;  // 1始まり・0=自動
  std::vector<std::string> name_cols, company_cols;  // 見出し名 or 1始まり列番号
};

bool all_digits(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(),
                                   [](unsigned char c) { return std::isdigit(c) != 0; });
}

// 見出し名 or 1始まり列番号 → 見出し名のリスト（tokenize_table は名前で照合する）
std::vector<std::string> resolve_cols(const std::vector<std::string>& headers,
                                      const std::vector<std::string>& specs) {
  std::vector<std::string> out;
  for (const auto& s : specs) {
    if (all_digits(s)) {
      const int idx = numparse::to_int(s, 0) - 1;  // 桁あふれで例外を投げない
      if (idx >= 0 && idx < static_cast<int>(headers.size())) out.push_back(headers[idx]);
    } else {
      out.push_back(s);
    }
  }
  return out;
}

extractors::Table build_table(const std::vector<std::vector<std::string>>& rows,
                              const TableSpec& spec) {
  if (spec.header_row > 0) {  // 見出し行を明示指定（1始まりのシート行番号）
    extractors::Table t;
    const std::size_t hi = static_cast<std::size_t>(spec.header_row - 1);
    if (hi < rows.size()) {
      t.headers = rows[hi];
      for (std::size_t i = hi + 1; i < rows.size(); ++i) {
        bool any = false;
        for (const auto& c : rows[i]) if (!c.empty()) { any = true; break; }
        if (any) t.rows.push_back(rows[i]);  // 全セル空の行は落とす（要約件数を正しく）
      }
    }
    return t;
  }
  // 自動: 名前指定の列を wanted に渡して見出し行検出を助ける
  std::vector<std::string> wanted;
  for (const auto& s : spec.name_cols) if (!all_digits(s)) wanted.push_back(s);
  for (const auto& s : spec.company_cols) if (!all_digits(s)) wanted.push_back(s);
  return extractors::rows_to_table(rows, wanted);
}

step5::FileResult ingest_table(const std::string& path, const TableSpec& spec) {
  step5::FileResult fr;
  fr.source = basename_of(path);
  fr.has_table = true;
  const std::string e = ext_of(path);
  std::vector<std::vector<std::string>> rows;
  try {
    if (e == "csv") { fr.kind = "csv-table"; rows = extractors::read_csv_rows(path); }
    else if (e == "xlsx") { fr.kind = "xlsx-table"; rows = ooxml::read_xlsx_sheet_rows(path, spec.sheet); }
    else { fr.kind = "skipped"; return fr; }
  } catch (const std::exception& ex) {
    fr.kind = "skipped";
    fr.error = ex.what();  // text には入れない（出力へ束ねられるため）
    return fr;
  }
  const auto t = build_table(rows, spec);
  fr.headers = t.headers;
  fr.rows = t.rows;
  fr.name_cols = resolve_cols(t.headers, spec.name_cols);
  fr.company_cols = resolve_cols(t.headers, spec.company_cols);
  return fr;
}

std::map<std::string, TableSpec> read_tables_jsonl(const std::string& path) {
  const std::string body = fileio::read_all(path);
  if (body.empty() && !file_exists(path))
    throw std::runtime_error("--tables ファイルが読めません: " + path);
  std::map<std::string, TableSpec> out;
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
      const std::string file = j.value("file", std::string());
      if (file.empty()) continue;
      TableSpec ts;
      ts.sheet = j.value("sheet", std::string());
      ts.header_row = j.value("header_row", 0);
      auto load_cols = [&](const char* key, std::vector<std::string>& dst) {
        if (!j.contains(key)) return;
        for (const auto& v : j[key]) {
          if (v.is_string()) dst.push_back(v.get<std::string>());
          else if (v.is_number_integer()) dst.push_back(std::to_string(v.get<int>()));
        }
      };
      load_cols("name_cols", ts.name_cols);
      load_cols("company_cols", ts.company_cols);
      out[basename_of(file)] = ts;
    } catch (const std::exception& ex) {
      throw std::runtime_error("--tables JSONL " + std::to_string(ln) + " 行目を解釈できません: " + ex.what());
    }
  }
  return out;
}

// 見出し語から列種別を推測（skeleton の初期値・人が最終編集する）
std::string guess_col_kind(const std::string& header) {
  static const std::vector<std::string> name_kw = {
      "氏名", "名前", "担当", "姓名", "顧客名", "会員名", "氏 名", "name", "Name"};
  static const std::vector<std::string> comp_kw = {
      "会社", "取引先", "法人", "企業", "組織", "得意先", "屋号", "company", "Company"};
  for (const auto& k : name_kw) if (header.find(k) != std::string::npos) return "name";
  for (const auto& k : comp_kw) if (header.find(k) != std::string::npos) return "company";
  return "";
}

json skeleton_entry(const std::string& file, const std::string& sheet,
                    const std::vector<std::string>& headers, int header_row_1based) {
  json j;
  j["file"] = file;
  if (!sheet.empty()) j["sheet"] = sheet;
  j["header_row"] = header_row_1based;
  json ncols = json::array(), ccols = json::array();
  for (const auto& h : headers) {
    if (utf8::trim(h).empty()) continue;
    const std::string k = guess_col_kind(h);
    if (k == "name") ncols.push_back(h);
    else if (k == "company") ccols.push_back(h);
  }
  j["name_cols"] = ncols;
  j["company_cols"] = ccols;
  return j;
}

// csv は1件・xlsx はシートごとに雛形を出す（人は不要シート行を削除・列を確認して編集）。
std::vector<json> table_skeletons(const std::string& path) {
  std::vector<json> out;
  const std::string e = ext_of(path);
  const std::string b = basename_of(path);
  try {
    if (e == "csv") {
      const auto rows = extractors::read_csv_rows(path);
      const std::size_t hi = extractors::detect_header_row(rows, {});
      const std::vector<std::string> headers = hi < rows.size() ? rows[hi] : std::vector<std::string>{};
      out.push_back(skeleton_entry(b, "", headers, static_cast<int>(hi) + 1));
    } else if (e == "xlsx") {
      const auto shared = ooxml::read_shared_strings(path);
      for (const auto& ns : ooxml::sheets_in_order(path)) {
        const auto rows = ooxml::sheet_rows(ooxml::zip_read(path, ns.second), shared);
        if (rows.empty()) continue;
        const std::size_t hi = extractors::detect_header_row(rows, {});
        const std::vector<std::string> headers = hi < rows.size() ? rows[hi] : std::vector<std::string>{};
        out.push_back(skeleton_entry(b, ns.first, headers, static_cast<int>(hi) + 1));
      }
    }
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "warning: 表の見出し取得に失敗: %s (%s)\n", b.c_str(), ex.what());
  }
  return out;
}

std::vector<step5::FileResult> ingest_files(const std::vector<std::string>& paths,
                                            const std::map<std::string, TableSpec>& specs) {
  std::vector<step5::FileResult> files;
  for (const auto& p : paths) {
    const std::string b = basename_of(p);
    const std::string e = ext_of(p);
    auto it = specs.find(b);
    step5::FileResult fr;
    if (it != specs.end() && (e == "csv" || e == "xlsx")) fr = ingest_table(p, it->second);
    else fr = ingest(p);
    if (fr.kind == "skipped")
      std::fprintf(stderr, "warning: スキップ: %s%s%s\n", b.c_str(),
                   fr.error.empty() ? "" : " — ", fr.error.c_str());
    files.push_back(std::move(fr));
  }
  return files;
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
  // 検知に掛けるテキスト: 各ファイルの本文 ＋ **ファイル名**。
  // bundle_blocks は【ファイル名】ブロックと複数ファイル時の【source】見出しで
  // ファイル名を出力に載せるので、ここで候補にしないと
  // 「社員名簿_山田太郎_確認用.csv」の氏名がマスクされないまま出ていく。
  std::string names;  // targets が指すので寿命をループの外に置く
  {
    std::set<std::string> seen;
    for (const auto& fr : files)
      if (!fr.source.empty() && seen.insert(fr.source).second) names += fr.source + "\n";
  }
  // 本文はコピーせず参照で回す（PDF 等で数MBになるため）
  std::vector<std::pair<std::string, const std::string*>> targets;  // (表示名, テキスト)
  for (const auto& fr : files)
    if (!fr.text.empty() && fr.kind != "skipped") targets.emplace_back(fr.source, &fr.text);
  if (!names.empty()) targets.emplace_back("（ファイル名）", &names);

  for (const auto& [label, text] : targets) {
    std::vector<step2::Candidate> cands;
    try {
      cands = step2::extract_candidates(*eng.patterns, *eng.sd, *eng.ner, *text);
    } catch (const std::exception& ex) {
      // 1本の失敗で全体を落とさない（本文は出さない＝生テキストを漏らさない）
      std::fprintf(stderr, "warning: 検知に失敗: %s — %s\n", label.c_str(), ex.what());
      continue;
    }
    for (const auto& c : cands) {
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
      // fileio::open_write を通す（ナローパスだと日本語の出力先を開けない）
      file = fileio::open_write(path);
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
  // 表オプション（Phase B）
  std::string tables;          // --tables tables.jsonl（複数ファイル/シートの列指定）
  std::string tables_out;      // --tables-out（detect が csv/xlsx の列雛形を書く先）
  bool table = false;          // --table（単一表の簡易指定・入力の csv/xlsx に適用）
  int header_row = 0;          // --header-row N（1始まり・0=自動）
  std::string sheet;           // --sheet NAME（xlsx）
  std::vector<std::string> name_cols, company_cols;  // --name-cols / --company-cols
};

std::vector<std::string> split_commas(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
    else cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

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
    else if (t == "--tables") a.tables = need(i);
    else if (t == "--tables-out") a.tables_out = need(i);
    else if (t == "--table") a.table = true;
    else if (t == "--sheet") a.sheet = need(i);
    else if (t == "--header-row") a.header_row = std::stoi(need(i));
    else if (t == "--name-cols") a.name_cols = split_commas(need(i));
    else if (t == "--company-cols") a.company_cols = split_commas(need(i));
    else if (!t.empty() && t[0] == '-') die("不明なオプション: " + t);
    else a.files.push_back(t);
  }
  return a;
}

// Args から表指定マップを組む（--tables 優先・--table フラグは csv/xlsx 入力に一律適用）
std::map<std::string, TableSpec> build_table_specs(const Args& a) {
  std::map<std::string, TableSpec> specs;
  if (!a.tables.empty()) specs = read_tables_jsonl(a.tables);
  if (a.table) {
    TableSpec ts;
    ts.sheet = a.sheet;
    ts.header_row = a.header_row;
    ts.name_cols = a.name_cols;
    ts.company_cols = a.company_cols;
    for (const auto& p : a.files) {
      const std::string e = ext_of(p);
      if (e == "csv" || e == "xlsx") {
        const std::string b = basename_of(p);
        if (!specs.count(b)) specs[b] = ts;  // --tables があればそちらを優先
      }
    }
  }
  return specs;
}

// ---------------------------------------------------------------- subcommands
int cmd_detect(const Args& a) {
  if (a.files.empty()) die("detect: 対象ファイルを指定してください");
  // prose（NER候補）と 表 csv/xlsx（列雛形）に分ける
  std::vector<std::string> prose_files, table_files;
  for (const auto& p : a.files) {
    const std::string e = ext_of(p);
    if (e == "csv" || e == "xlsx") table_files.push_back(p);
    else prose_files.push_back(p);
  }

  // prose → 候補 JSONL（-o / stdout）
  if (!prose_files.empty()) {
    Engine eng(resolve_data_root(a.data));
    auto files = ingest_files(prose_files, {});
    const auto cands = detect_candidates(eng, files);
    OutSink sink(a.out);
    write_candidates_jsonl(sink.stream(), cands);
    std::fprintf(stderr, "detect: 候補 %zu 件\n", cands.size());
  }

  // 表 → tables.jsonl 雛形（--tables-out）
  if (!table_files.empty()) {
    if (a.tables_out.empty()) {
      std::fprintf(stderr,
        "warning: csv/xlsx がありますが --tables-out 未指定のため表の列雛形を出力しません\n");
    } else {
      std::ofstream tf = fileio::open_write(a.tables_out);  // 日本語パス対応
      if (!tf) die("--tables-out を開けません: " + a.tables_out);
      std::size_t n = 0;
      for (const auto& p : table_files)
        for (const auto& j : table_skeletons(p)) {
          // 列を推測できないシート（フォーム等）はコメント行にする＝mask では無視される。
          // 同一ファイルの複数シートが basename キーで衝突するのも防ぐ（不要なら残す/消す）。
          const bool empty = j["name_cols"].empty() && j["company_cols"].empty();
          if (empty) tf << "# ";
          tf << j.dump() << "\n";
          ++n;
        }
      std::fprintf(stderr,
        "detect: 表の列雛形 %zu 件 → %s（不要なシート行は削除し、name_cols/company_cols を確認）\n",
        n, a.tables_out.c_str());
    }
  }
  return 0;
}

int cmd_mask(const Args& a) {
  if (a.files.empty()) die("mask: 対象ファイルを指定してください");
  Engine eng(resolve_data_root(a.data));
  auto files = ingest_files(a.files, build_table_specs(a));

  std::vector<tokenizer::ConfirmedTerm> confirmed;
  if (!a.candidates.empty()) {
    confirmed = read_candidates_jsonl(a.candidates);  // 権威ファイル（モデル不要）
  } else {
    bool has_prose = false;  // 純表のみならモデルを積まない
    for (const auto& fr : files)
      if (!fr.has_table && fr.kind != "skipped" && !fr.text.empty()) { has_prose = true; break; }
    if (has_prose)
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
      std::ofstream mf = fileio::open_write(a.mapping);  // 日本語パス対応
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
    "  detect  <files...> [-o cand.jsonl] [--tables-out tables.jsonl] [--data DIR]\n"
    "  mask    <files...> [--candidates cand.jsonl] [--reversible --mapping map.jsonl]\n"
    "                     [-o out.txt] [--data DIR]\n"
    "  restore --mapping map.jsonl [-i in.txt] [-o out.txt]\n"
    "\n"
    "  data tables (csv/xlsx): mask/detect も同じ 1 呼び出しで prose と対応表を共有\n"
    "    --tables tables.jsonl                per-file 列指定 {file,sheet,header_row,name_cols,company_cols}\n"
    "    --table --name-cols A,B --company-cols C [--header-row N] [--sheet NAME]\n"
    "                                         単一表の簡易指定（列は見出し名 or 1始まり番号）\n");
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  // Windows は argv を cp932 で渡す。日本語のシート名・列名（--sheet/--name-cols）が
  // UTF-8 の内部と一致しなくなるので、UTF-16 のコマンドラインから UTF-8 で作り直す。
  int wargc = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  std::vector<std::string> u8;
  std::vector<char*> u8ptr;
  if (wargv) {
    for (int i = 0; i < wargc; ++i) u8.push_back(wide_to_utf8(wargv[i]));
    LocalFree(wargv);
    for (auto& s : u8) u8ptr.push_back(s.data());
    argc = wargc;
    argv = u8ptr.data();
  }
#endif
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
