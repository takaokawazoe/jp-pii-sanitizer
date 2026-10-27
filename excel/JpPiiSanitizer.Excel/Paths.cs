// ヘルパー exe と models ディレクトリの場所。
//
// 配布時のレイアウトは 2 通りある。どちらも XLL の隣に jp-pii-helper.exe がある:
//   - インストール後（%LOCALAPPDATA%\jp-pii-sanitizer\）: models\ も XLL の隣
//   - 配布 zip の中（jp-pii-sanitizer\excel\）: models\ は 1 つ上（GUI/CLI と共有）
// 開発中は XLL の隣に jp-pii-sanitizer.config.json を置いて上書きする:
//   {"helper": "C:\\tools\\jp-pii-sanitizer\\cpp\\build\\jp-pii-helper.exe",
//    "data":   "C:\\tools\\jp-pii-sanitizer\\cpp\\models"}
// 環境変数にしなかったのは、Excel のプロセスに届けるにはログオンし直しが要るため。
using System;
using System.IO;
using ExcelDna.Integration;
using Newtonsoft.Json.Linq;

namespace JpPii.Excel
{
    internal static class Paths
    {
        public const string ConfigFileName = "jp-pii-sanitizer.config.json";

        public static string XllDir => Path.GetDirectoryName(ExcelDnaUtil.XllPath) ?? ".";

        private static JObject _config;

        private static JObject Config
        {
            get
            {
                if (_config != null) return _config;
                var path = Path.Combine(XllDir, ConfigFileName);
                try
                {
                    _config = File.Exists(path) ? JObject.Parse(File.ReadAllText(path)) : new JObject();
                }
                catch (Exception)
                {
                    _config = new JObject();  // 壊れた設定は無視して既定の配置で動く
                }
                return _config;
            }
        }

        public static string HelperExe
        {
            get
            {
                var v = (string)Config["helper"];
                return string.IsNullOrWhiteSpace(v) ? Path.Combine(XllDir, "jp-pii-helper.exe") : v;
            }
        }

        /// <summary>models ディレクトリ。見つからなければ null（ヘルパー自身の解決に任せる）。</summary>
        public static string DataDir
        {
            get
            {
                var v = (string)Config["data"];
                if (!string.IsNullOrWhiteSpace(v)) return v;
                // 隣 → ヘルパーの隣 → 1 つ上（配布 zip の excel\ から GUI と共有する models\）
                var candidates = new[] { XllDir, Path.GetDirectoryName(HelperExe) ?? XllDir, Path.GetDirectoryName(XllDir) };
                foreach (var dir in candidates)
                {
                    if (string.IsNullOrEmpty(dir)) continue;
                    var models = Path.Combine(dir, "models");
                    if (File.Exists(Path.Combine(models, "patterns.json"))) return models;
                }
                return null;
            }
        }
    }
}
