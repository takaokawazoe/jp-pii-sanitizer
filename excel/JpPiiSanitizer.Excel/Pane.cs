// タスクペイン。GUI（index.html）の画面①〜④をセル向けに組み直したもの。
//
//   読み込んで検出 → 列の扱いを決める（列指定）・自由記述の候補を確認（opt-out）
//   → サニタイズして別ブック（可逆なら対応表も隣に書く）
//
// 画面に出すのはこの主経路だけ。たまにしか使わないもの（手動追記・列の値の個別確認・
// リセット）は「詳細」に畳み、別の作業である「実名に戻す」はリボンから開く別ウィンドウ
// （RestoreDialog）にした。タスクペインは幅が狭いので、縦 1 本に全部並べると読めない。
//
// 原則は GUI と同じ: 既定は全マスク（opt-out）、候補の件数は「マスクされるセル数」で
// プレビュー（＝セル選択）と一致させる、対応表は Excel 側に乗せない（保存先のパスだけ渡す）。
//
// 列指定（ColumnRules.cs）: 表では列＝属性なので、列ごとに 自動検出／人名／組織／住所／
// 電話／メール／識別子／対象外 を選ぶ。人名・組織・住所の列の値は候補一覧にも載り
// （[A列] の印つき。既定では隠す）、値単位でチェックを外せる＝その列の skip になる。
//
// スレッド: COM（SheetIO）はメインスレッドでしか触れない。ヘルパー呼び出し（モデル読込・
// 検知）は数秒〜十数秒かかるのでバックグラウンドで行い、結果は BeginInvoke で戻す。
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;
using ExcelDna.Integration.CustomUI;
using Newtonsoft.Json.Linq;

namespace JpPii.Excel
{
    internal static class Pane
    {
        private static CustomTaskPane _pane;

        public static void Show()
        {
            if (_pane == null)
            {
                _pane = CustomTaskPaneFactory.CreateCustomTaskPane(typeof(PaneControl), "PII サニタイザ");
                _pane.DockPosition = MsoCTPDockPosition.msoCTPDockPositionRight;
                _pane.Width = 420;
                _pane.VisibleStateChange += p => { /* 閉じてもインスタンスは残す（状態保持） */ };
            }
            _pane.Visible = true;
        }

        public static void Close()
        {
            try { _pane?.Delete(); } catch (Exception) { }
            _pane = null;
        }
    }

    internal sealed class CandidateRow
    {
        public string Text;
        public string Type;       // PERSON / ORGANIZATION / LOCATION
        public int Detected;      // 検知回数（参考）
        public int MaskCells;     // マスクされるセル数（表示する数字はこちら）
        public bool Manual;
        public string Column;     // 列指定の値なら列記号（"A"）。NER 候補・手動は null

        public override string ToString()
        {
            var tag = Column != null ? "[" + Column + "列] " : "";
            return tag + Text + "　[" + Kinds.Label(Type) + "]　" + MaskCells + " セル" + (Manual ? "（手動）" : "");
        }
    }

    public sealed class PaneControl : UserControl
    {
        private static HelperClient H => HelperClient.Shared;

        private SheetSnapshot _snap;
        private int _minRow, _maxRow;
        private JArray _lastLoadCells;                                   // load の spans（密度の提案に使う）
        private readonly List<ColumnInfo> _columns = new List<ColumnInfo>();
        private readonly List<CandidateRow> _nerRows = new List<CandidateRow>();     // 検知した候補
        private readonly List<CandidateRow> _manualRows = new List<CandidateRow>();  // 手動追記
        private readonly List<CandidateRow> _rows = new List<CandidateRow>();        // 確定リストの元になる全行
        private readonly List<CandidateRow> _listRows = new List<CandidateRow>();    // 一覧に出している行（_rows の部分集合）
        private readonly HashSet<string> _unchecked = new HashSet<string>();        // チェックを外した語
        private readonly HashSet<string> _seeded = new HashSet<string>();           // 定型値として自動で外した語（1 回だけ）
        private readonly List<CellText> _maskedList = new List<CellText>();         // 直近のプレビューでマスクされるセル
        private Dictionary<string, List<CellText>> _cellsByTerm = new Dictionary<string, List<CellText>>();
        private int _autoCells;          // 番号系（メール・電話・郵便番号）だけが当たるセル数
        private int _maskedCells;        // マスク対象セル総数
        private bool _busy;
        private bool _rebuildingList;    // RebuildList 中の ItemCheck / SelectedIndexChanged は利用者の操作ではない
        private bool _fillingGrid;       // FillGrid 中の CellValueChanged は利用者の操作ではない
        private bool _settingHeader;     // 読み込み時に見出し行を自動で入れている間は ValueChanged を無視
        private string _proposalNote = "";
        private readonly ColumnMemory _memory = new ColumnMemory();

        // ---- controls（主経路） ----
        private readonly Label _status = new Label();
        private readonly Button _btnLoad = new Button();
        private readonly NumericUpDown _numHeader = new NumericUpDown();
        private readonly DataGridView _grid = new DataGridView();
        private readonly Label _lblCandidates = new Label();
        private readonly CheckedListBox _list = new CheckedListBox();
        private readonly CheckBox _chkReversible = new CheckBox();
        private readonly CheckBox _chkEncrypt = new CheckBox();
        private readonly TextBox _txtPass = new TextBox();
        private readonly Button _btnGen = new Button();
        private readonly CheckBox _chkShowPass = new CheckBox();
        private readonly LinkLabel _lnkSelectAll = new LinkLabel();
        private readonly Button _btnSanitize = new Button();
        private readonly Label _result = new Label();
        // ---- controls（詳細。畳んである） ----
        private readonly LinkLabel _lnkAdvanced = new LinkLabel();
        private readonly TableLayoutPanel _advanced = new TableLayoutPanel();
        private readonly TextBox _txtManual = new TextBox();
        private readonly ComboBox _cmbType = new ComboBox();
        private readonly Button _btnAdd = new Button();
        private readonly CheckBox _chkShowColumnValues = new CheckBox();
        private readonly Button _btnReset = new Button();

        private const int ColLetter = 0, ColHeader = 1, ColKind = 2, ColMasked = 3;
        private const string CandidatesHint = "　クリック: セルを選択　もう一度: 外す／戻す";

        /// <summary>ペインの文字サイズ（pt）。既定の 9pt は狭いペインでは読みにくい。</summary>
        internal const float FontPt = 10.5f;

        public PaneControl()
        {
            Font = new Font(SystemFonts.MessageBoxFont.FontFamily, FontPt);
            AutoScroll = true;
            BuildLayout();
            WireEvents();
            SetStatus("「読み込んで検出」でアクティブシート（範囲を選択していればその範囲）を読みます。");
        }

        // ------------------------------------------------------------ layout
        private void BuildLayout()
        {
            var table = new TableLayoutPanel
            {
                Dock = DockStyle.Top,
                AutoSize = true,
                AutoSizeMode = AutoSizeMode.GrowAndShrink,
                ColumnCount = 1,
                Padding = new Padding(8),
            };
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));

            // 高さはフォントの行高から決める（DPI と文字サイズに追従。固定ピクセルだと欠ける）
            var lh = Font.Height;
            void Add(Control c, int height = 0) => AddRow(table, c, height);
            Label Section(string text) => new Label { Text = text, AutoSize = false, Height = lh + 2, AutoEllipsis = true,
                Font = new Font(Font, FontStyle.Bold), Margin = new Padding(0, 10, 0, 0) };
            var tips = new ToolTip();

            _status.AutoSize = false;
            _status.BorderStyle = BorderStyle.FixedSingle;
            _status.Padding = new Padding(4);
            Add(_status, lh * 4 + 12);   // 保存先のパスを 2 行出せる高さ

            _btnLoad.Text = "読み込んで検出";
            Add(_btnLoad, lh + 14);

            // 「列の扱い」の見出し行と、見出し行の指定を同じ行に
            var hdr = new TableLayoutPanel { ColumnCount = 3, AutoSize = true, Margin = new Padding(0, 10, 0, 0) };
            hdr.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            hdr.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            hdr.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 56));
            var secColumns = Section("列の扱い（行をクリックで列を選択）");
            secColumns.Margin = new Padding(0);
            secColumns.Anchor = AnchorStyles.Left | AnchorStyles.Right;
            var lblHeader = new Label { Text = "見出し行", AutoSize = true, TextAlign = ContentAlignment.MiddleLeft, Anchor = AnchorStyles.Right, Margin = new Padding(0, 1, 4, 0) };
            _numHeader.Minimum = 0;
            _numHeader.Maximum = 1048576;
            _numHeader.Value = 1;
            _numHeader.Anchor = AnchorStyles.Left | AnchorStyles.Right;
            _numHeader.Margin = new Padding(0);
            tips.SetToolTip(_numHeader, "見出しの行番号。0 = 見出しなし。\n列の扱いが掛かるのは見出し行より下。見出し行とその上（タイトル行）は通常の検出だけ。");
            tips.SetToolTip(lblHeader, "見出しの行番号。0 = 見出しなし。");
            hdr.Controls.Add(secColumns, 0, 0);
            hdr.Controls.Add(lblHeader, 1, 0);
            hdr.Controls.Add(_numHeader, 2, 0);
            Add(hdr);

            _grid.AllowUserToAddRows = false;
            _grid.AllowUserToDeleteRows = false;
            _grid.AllowUserToResizeRows = false;
            _grid.RowHeadersVisible = false;
            _grid.MultiSelect = false;
            _grid.SelectionMode = DataGridViewSelectionMode.CellSelect;
            _grid.AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.None;
            // 見出し行の高さはフォントに合わせて自動（固定だと大きい文字・高 DPI で下が欠ける）
            _grid.ColumnHeadersHeightSizeMode = DataGridViewColumnHeadersHeightSizeMode.AutoSize;
            _grid.ColumnHeadersDefaultCellStyle.Padding = new Padding(2, 3, 2, 3);
            _grid.RowTemplate.Height = lh + 10;
            _grid.ScrollBars = ScrollBars.Vertical;
            _grid.BackgroundColor = SystemColors.Window;
            _grid.EditMode = DataGridViewEditMode.EditOnEnter;
            int W(string sample, int extra) => TextRenderer.MeasureText(sample, Font).Width + extra;
            _grid.Columns.Add(new DataGridViewTextBoxColumn { HeaderText = "列", Width = W("AAA", 10), ReadOnly = true });
            _grid.Columns.Add(new DataGridViewTextBoxColumn { HeaderText = "見出し", ReadOnly = true,
                AutoSizeMode = DataGridViewAutoSizeColumnMode.Fill });
            var kind = new DataGridViewComboBoxColumn { HeaderText = "扱い", Width = W("自動検出", 34), FlatStyle = FlatStyle.Flat,
                DisplayStyle = DataGridViewComboBoxDisplayStyle.DropDownButton };
            foreach (var k in Kinds.Order) kind.Items.Add(Kinds.Label(k));
            _grid.Columns.Add(kind);
            _grid.Columns.Add(new DataGridViewTextBoxColumn { HeaderText = "マスク", Width = W("マスク", 14), ReadOnly = true });
            Add(_grid, (lh + 16) + 6 * (lh + 10));   // 見出し＋ 6 行ぶん

            _lblCandidates.Text = "自由記述の候補";
            _lblCandidates.AutoSize = false;
            _lblCandidates.Height = lh + 2;
            _lblCandidates.AutoEllipsis = true;
            _lblCandidates.Font = new Font(Font, FontStyle.Bold);
            _lblCandidates.Margin = new Padding(0, 10, 0, 0);
            Add(_lblCandidates);
            // CheckOnClick は切る: 1 回目のクリックで選択（＝シートで該当セルを選択して文脈を見る）、
            // 同じ行をもう 1 回クリックでチェックを外す／戻す。見てから決める順になる。
            _list.CheckOnClick = false;
            _list.IntegralHeight = false;
            Add(_list, 5 * (lh + 4) + 6);   // 5 行ぶん

            // 可逆・パスフレーズ保護は 1 行に。パスフレーズの入力欄は保護を選んだときだけ出す
            var opts = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, Margin = new Padding(0, 10, 0, 0) };
            opts.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            opts.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            _chkReversible.Text = "可逆（対応表を残す）";
            _chkReversible.Checked = true;
            _chkReversible.AutoSize = true;
            _chkReversible.Margin = new Padding(0, 0, 12, 0);
            tips.SetToolTip(_chkReversible, "対応表を別ブックの隣の「対応表\\」フォルダに書き、あとで「実名に戻す」で元に戻せるようにします。");
            _chkEncrypt.Text = "パスフレーズで保護";
            _chkEncrypt.AutoSize = true;
            _chkEncrypt.Margin = new Padding(0);
            tips.SetToolTip(_chkEncrypt, "対応表を暗号化します。パスフレーズがないと実名に戻せません。");
            opts.Controls.Add(_chkReversible, 0, 0);
            opts.Controls.Add(_chkEncrypt, 1, 0);
            Add(opts);

            var pass = new TableLayoutPanel { ColumnCount = 3, AutoSize = true, Margin = new Padding(0), Visible = false };
            pass.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            pass.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 56));
            pass.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 56));
            _txtPass.UseSystemPasswordChar = true;
            _txtPass.Anchor = AnchorStyles.Left | AnchorStyles.Right;
            _btnGen.Text = "生成";
            _btnGen.Anchor = AnchorStyles.Left | AnchorStyles.Right;
            _chkShowPass.Text = "表示";
            pass.Controls.Add(_txtPass, 0, 0);
            pass.Controls.Add(_btnGen, 1, 0);
            pass.Controls.Add(_chkShowPass, 2, 0);
            Add(pass);
            _chkEncrypt.CheckedChanged += (s, e) => pass.Visible = _chkEncrypt.Checked;
            _chkReversible.CheckedChanged += (s, e) =>
            {
                _chkEncrypt.Enabled = _chkReversible.Checked;
                pass.Visible = _chkReversible.Checked && _chkEncrypt.Checked;
            };

            // プレビュー: 変わるセルを全部シートで選択する。文言は ApplySpans が更新する
            _lnkSelectAll.AutoSize = false;
            _lnkSelectAll.Height = lh + 6;
            _lnkSelectAll.TextAlign = ContentAlignment.MiddleLeft;
            _lnkSelectAll.Text = "";
            Add(_lnkSelectAll);

            _btnSanitize.Text = "サニタイズして別ブックに保存…";
            Add(_btnSanitize, lh + 14);

            _result.AutoSize = false;
            Add(_result, lh * 2 + 8);

            // ---- 詳細（畳む） ----
            _lnkAdvanced.AutoSize = true;
            _lnkAdvanced.Margin = new Padding(0, 8, 0, 0);
            _lnkAdvanced.LinkBehavior = LinkBehavior.HoverUnderline;
            Add(_lnkAdvanced);

            _advanced.ColumnCount = 1;
            _advanced.AutoSize = true;
            _advanced.AutoSizeMode = AutoSizeMode.GrowAndShrink;
            _advanced.Margin = new Padding(0);
            _advanced.Visible = false;
            _advanced.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));

            var lblManual = new Label { Text = "検出されなかった語を候補に加える:", AutoSize = true, Margin = new Padding(0, 6, 0, 0) };
            AddRow(_advanced, lblManual);
            var manual = new TableLayoutPanel { ColumnCount = 3, AutoSize = true, Margin = new Padding(0) };
            manual.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            manual.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 70));
            manual.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 56));
            _txtManual.Anchor = AnchorStyles.Left | AnchorStyles.Right;
            _cmbType.DropDownStyle = ComboBoxStyle.DropDownList;
            _cmbType.Items.AddRange(new object[] { "人名", "組織", "住所" });
            _cmbType.SelectedIndex = 0;
            _cmbType.Anchor = AnchorStyles.Left | AnchorStyles.Right;
            _btnAdd.Text = "追加";
            _btnAdd.Anchor = AnchorStyles.Left | AnchorStyles.Right;
            manual.Controls.Add(_txtManual, 0, 0);
            manual.Controls.Add(_cmbType, 1, 0);
            manual.Controls.Add(_btnAdd, 2, 0);
            AddRow(_advanced, manual);

            _chkShowColumnValues.Text = "列指定の列の値も候補一覧に出す（値ごとに外す）";
            _chkShowColumnValues.AutoSize = true;
            _chkShowColumnValues.Margin = new Padding(0, 6, 0, 0);
            AddRow(_advanced, _chkShowColumnValues);

            _btnReset.Text = "リセット（読み込みをやり直す）";
            _btnReset.Margin = new Padding(0, 10, 0, 3);
            AddRow(_advanced, _btnReset, lh + 10);
            Add(_advanced);
            SetAdvanced(false);

            Controls.Add(table);
        }

        private static void AddRow(TableLayoutPanel table, Control c, int height = 0)
        {
            c.Anchor = AnchorStyles.Left | AnchorStyles.Right;
            if (c.Margin == Padding.Empty || c.Margin == new Padding(3)) c.Margin = new Padding(0, 3, 0, 3);
            if (height > 0) c.Height = height;
            table.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            table.Controls.Add(c, 0, table.RowCount++);
        }

        private void SetAdvanced(bool open)
        {
            _advanced.Visible = open;
            _lnkAdvanced.Text = open ? "▾ 詳細を閉じる" : "▸ 詳細（候補の手動追加・列の値の個別確認・リセット）";
        }

        private void WireEvents()
        {
            _btnLoad.Click += (s, e) => Guard(LoadAndDetect);
            _numHeader.ValueChanged += (s, e) => { if (_snap != null && !_busy && !_settingHeader) Guard(ReproposeColumns); };
            _chkShowColumnValues.CheckedChanged += (s, e) => { if (!_busy) RebuildList(); };
            _lnkSelectAll.LinkClicked += (s, e) => Guard(SelectAllMasked);
            _lnkAdvanced.LinkClicked += (s, e) => SetAdvanced(!_advanced.Visible);

            // ドロップダウンの変更を即座に確定させる（既定ではセルを離れるまで CellValueChanged が出ない）
            _grid.CurrentCellDirtyStateChanged += (s, e) =>
            {
                if (_grid.IsCurrentCellDirty) _grid.CommitEdit(DataGridViewDataErrorContexts.Commit);
            };
            _grid.CellValueChanged += (s, e) =>
            {
                if (_fillingGrid || _busy || e.RowIndex < 0 || e.ColumnIndex != ColKind) return;
                Guard(() => OnKindChanged(e.RowIndex));
            };
            _grid.CellClick += (s, e) =>
            {
                if (_busy || e.RowIndex < 0 || e.ColumnIndex == ColKind) return;
                Guard(() => SelectColumn(e.RowIndex));
            };
            _grid.DataError += (s, e) => { e.ThrowException = false; };

            // 候補をクリック → その語が伏せられるセルをシートで選択（GUI の「クリックでプレビューへ飛ぶ」）
            _list.SelectedIndexChanged += (s, e) => { if (!_rebuildingList && !_busy) Guard(JumpToSelected); };
            // ItemCheck は Items.Add(item, checked) や SetItemChecked でも発火する。
            // 一覧の再構築で出るものを拾うと、プレビュー更新 → 再構築 → ItemCheck … と無限に回る。
            _list.ItemCheck += (s, e) =>
            {
                if (_rebuildingList || _busy) return;
                if (e.Index >= 0 && e.Index < _listRows.Count)
                {
                    if (e.NewValue == CheckState.Unchecked) _unchecked.Add(_listRows[e.Index].Text);
                    else _unchecked.Remove(_listRows[e.Index].Text);
                }
                BeginInvoke(new Action(() => Guard(() => RefreshPreview())));
            };
            _btnAdd.Click += (s, e) => Guard(AddManual);
            _txtManual.KeyDown += (s, e) => { if (e.KeyCode == Keys.Enter) { e.SuppressKeyPress = true; Guard(AddManual); } };
            _btnGen.Click += (s, e) => { _txtPass.Text = GeneratePassphrase(); _chkShowPass.Checked = true; };
            _chkShowPass.CheckedChanged += (s, e) => _txtPass.UseSystemPasswordChar = !_chkShowPass.Checked;
            _btnSanitize.Click += (s, e) => Guard(SanitizeToNewWorkbook);
            _btnReset.Click += (s, e) => Guard(ResetAll);
        }

        // ------------------------------------------------------------ plumbing
        private void Guard(Action a)
        {
            if (_busy) return;
            try { a(); }
            catch (Exception e) { ShowError(e); }
        }

        private void ShowError(Exception e)
        {
            SetBusy(false);
            SetStatus("エラー: " + e.Message, error: true);
            MessageBox.Show(this, e.Message, "PII サニタイザ", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }

        private void SetStatus(string text, bool error = false)
        {
            _status.Text = text;
            _status.ForeColor = error ? Color.Firebrick : SystemColors.ControlText;
        }

        private void SetBusy(bool busy, string text = null)
        {
            _busy = busy;
            // _grid は無効化しない（編集中に無効化すると表示が崩れる）。ハンドラ側で _busy を見る。
            foreach (var b in new Control[] { _btnLoad, _btnAdd, _lnkSelectAll, _btnSanitize, _btnReset, _list, _numHeader, _chkShowColumnValues })
                b.Enabled = !busy;
            if (busy) { UseWaitCursor = true; SetStatus(text ?? "処理中…"); }
            else UseWaitCursor = false;
        }

        /// <summary>work をバックグラウンドで実行し、done をメインスレッドで呼ぶ。例外はダイアログ。</summary>
        private void RunAsync<T>(string busyText, Func<T> work, Action<T> done)
        {
            SetBusy(true, busyText);
            Task.Run(() =>
            {
                try { return Tuple.Create(true, work(), (Exception)null); }
                catch (Exception e) { return Tuple.Create(false, default(T), e); }
            }).ContinueWith(t =>
            {
                if (IsDisposed) return;
                BeginInvoke(new Action(() =>
                {
                    SetBusy(false);
                    if (!t.Result.Item1) ShowError(t.Result.Item3);
                    else
                    {
                        try { done(t.Result.Item2); }
                        catch (Exception e) { ShowError(e); }
                    }
                }));
            });
        }

        private static void RequireOk(JObject resp, string what)
        {
            if (resp.Value<bool?>("ok") == true) return;
            throw new HelperException(what + ": " + (resp.Value<string>("error") ?? "不明なエラー"));
        }

        /// <summary>見出し行（シートの絶対行番号）。0 または範囲より上なら「見出しなし」。</summary>
        private int HeaderRow
        {
            get
            {
                var v = (int)_numHeader.Value;
                return v >= _minRow && v <= _maxRow ? v : 0;
            }
        }

        /// <summary>列指定が掛かる最初の行。見出し行とその上は通常セル（自動検出のみ）。</summary>
        private int DataRow1 => HeaderRow > 0 ? HeaderRow + 1 : _minRow;

        /// <summary>辞書（確定リスト）: 一覧でチェックの付いている語。</summary>
        private JArray ConfirmedTerms()
        {
            var arr = new JArray();
            foreach (var r in _rows)
                if (!_unchecked.Contains(r.Text))
                    arr.Add(new JObject { ["text"] = r.Text, ["type"] = r.Type });
            return arr;
        }

        /// <summary>
        /// 列指定: 自動検出以外の列。skip には、どの種別でも「なし」「N/A」のような定型値を載せ、
        /// 人名・組織・住所ではさらにチェックを外した値を載せる（メール列の「なし」をメールとして
        /// 伏せない。記号だけの「-」はヘルパーが自分で外す）。
        /// </summary>
        private JArray ColumnsJson()
        {
            var arr = new JArray();
            foreach (var col in _columns)
            {
                if (col.Kind == Kinds.Auto) continue;
                var o = new JObject { ["c"] = col.Col, ["r1"] = DataRow1, ["r2"] = _maxRow, ["kind"] = col.Kind };
                if (Kinds.IsWholeCellKind(col.Kind))
                {
                    var skip = new JArray();
                    foreach (var v in col.DistinctValues())
                        if (SkipValue(col, v)) skip.Add(v);
                    if (skip.Count > 0) o["skip"] = skip;
                }
                arr.Add(o);
            }
            return arr.Count > 0 ? arr : null;
        }

        /// <summary>
        /// その列で伏せない値か。人名・組織・住所はチェック状態がすべて（定型値は読み込み時に外して
        /// あり、利用者が戻せる）。それ以外の種別は一覧に出ないので定型値を機械的に外す。
        /// 記号だけの値（"-"）はヘルパー側でも外れる——ここでも同じ判定をして検証を合わせる。
        /// </summary>
        private bool SkipValue(ColumnInfo col, string v) =>
            !v.Any(char.IsLetterOrDigit) ||
            (Kinds.IsDictionaryKind(col.Kind) ? _unchecked.Contains(v) : ColumnRules.PlaceholderValues.Contains(v));

        // ------------------------------------------------------------ 読み込み・検知
        private void LoadAndDetect()
        {
            var snap = SheetIO.ReadTarget();   // COM はここ（メインスレッド）で済ませる
            if (snap.Cells.Count == 0)
            {
                SetStatus(snap.Description + " に値のあるセルがありません。");
                return;
            }
            var cells = new JArray();
            foreach (var c in snap.Cells)
                cells.Add(new JObject { ["r"] = c.Row, ["c"] = c.Col, ["text"] = c.Text });

            RunAsync(H.Ready ? "検知中…（" + snap.Cells.Count + " セル）" : "モデルを読み込んでいます（初回は十数秒かかります）…",
                () =>
                {
                    if (!H.Ready) RequireOk(H.Init(), "初期化に失敗しました");
                    var r = H.Load(cells);
                    RequireOk(r, "検知に失敗しました");
                    return r;
                },
                resp =>
                {
                    _snap = snap;
                    _nerRows.Clear();
                    foreach (var c in (JArray)resp["candidates"])
                        _nerRows.Add(new CandidateRow
                        {
                            Text = (string)c["text"], Type = (string)c["entity_type"], Detected = (int)c["count"],
                        });
                    _lastLoadCells = (JArray)resp["cells"];
                    _unchecked.Clear();   // 新しい表では opt-out を引き継がない（別の表の判断を持ち込まない）
                    _seeded.Clear();
                    _minRow = snap.Cells.Min(c => c.Row);
                    _maxRow = snap.Cells.Max(c => c.Row);
                    // 見出し行の初期値: テーブルなら Excel の答え、そうでなければ推定。利用者が上書きできる。
                    _settingHeader = true;
                    try { _numHeader.Value = ColumnRules.GuessHeaderRow(snap.Cells, _minRow, snap.TableHeaderRow); }
                    finally { _settingHeader = false; }
                    BuildColumns();
                    ProposeKinds();
                    ComposeRows();
                    FillGrid();
                    _result.Text = "";

                    var warnings = (JArray)resp["warnings"];
                    var w = warnings != null && warnings.Count > 0 ? "\n警告: " + string.Join(" / ", warnings.Select(x => (string)x)) : "";
                    Func<int, int, string> status = (nRows, nMasked) =>
                        snap.Description + "　" + snap.Cells.Count + " セル読込 → 候補 " + nRows + " 件、マスク対象 " + nMasked +
                        " セル\n列の扱い: " + _proposalNote + w;
                    if (_columns.Any(c => c.Kind != Kinds.Auto))
                    {
                        // 列指定の提案を反映したプレビューを取り直す（軽い。NER は回らない）
                        RefreshPreview(status, w.Length > 0);
                    }
                    else
                    {
                        ApplySpans(_lastLoadCells);
                        RebuildList();
                        SetStatus(status(_rows.Count, _maskedCells), error: w.Length > 0);
                    }
                });
        }

        /// <summary>読み込んだセルから列の一覧を組む（見出し・データセル）。</summary>
        private void BuildColumns()
        {
            _columns.Clear();
            if (_snap == null || _snap.Cells.Count == 0) return;
            var header = HeaderRow;
            var byCol = new SortedDictionary<int, ColumnInfo>();
            foreach (var c in _snap.Cells)
            {
                ColumnInfo col;
                if (!byCol.TryGetValue(c.Col, out col))
                    byCol[c.Col] = col = new ColumnInfo { Col = c.Col, Letter = ColumnRules.ColumnLetter(c.Col) };
                if (header > 0 && c.Row == header) col.Header = c.Text.Trim();
                else if (header == 0 || c.Row > header) col.DataCells.Add(c);
                // 見出し行より上（タイトル行など）は列指定の外。通常セルとして自動検出だけ掛かる
            }
            _columns.AddRange(byCol.Values);
        }

        /// <summary>種別の提案: 前回の設定（見出し一致） → 見出し語 → 検出密度 → 自動検出。</summary>
        private void ProposeKinds()
        {
            var categories = new Dictionary<long, HashSet<string>>();
            if (_lastLoadCells != null)
                foreach (var c in _lastLoadCells)
                {
                    var set = new HashSet<string>();
                    foreach (var sp in (JArray)c["spans"]) set.Add((string)sp[2]);
                    categories[SheetIO.Key((int)c["r"], (int)c["c"])] = set;
                }

            var headers = _columns.Select(c => c.Header).ToList();
            var exact = false;
            var remembered = HeaderRow > 0 ? _memory.Lookup(headers, out exact) : new string[_columns.Count];

            int fromMemory = 0, fromHeader = 0, fromDensity = 0;
            for (var i = 0; i < _columns.Count; i++)
            {
                var col = _columns[i];
                string why;
                if (!string.IsNullOrEmpty(remembered[i]))
                {
                    col.Kind = remembered[i];
                    col.Reason = exact ? "前回の設定（同じ見出しの表）" : "前回の設定（見出し名）";
                    fromMemory++;
                    continue;
                }
                var k = ColumnRules.SuggestFromHeader(col.Header);
                if (k != null) { col.Kind = k; col.Reason = "見出し「" + col.Header + "」"; fromHeader++; continue; }
                k = ColumnRules.SuggestFromDensity(col, categories, out why);
                if (k != null) { col.Kind = k; col.Reason = why; fromDensity++; continue; }
                col.Kind = Kinds.Auto;
                col.Reason = "";
            }
            var parts = new List<string>();
            if (fromMemory > 0) parts.Add(exact ? "前回の設定を適用（同じ見出しの表）" : "前回の設定を " + fromMemory + " 列に適用");
            if (fromHeader > 0) parts.Add("見出しから " + fromHeader + " 列を提案");
            if (fromDensity > 0) parts.Add("検出結果から " + fromDensity + " 列を提案");
            _proposalNote = parts.Count > 0 ? string.Join("、", parts) + "。表で確認してください" : "提案なし（すべて自動検出）";
        }

        private void ReproposeColumns()
        {
            BuildColumns();
            ProposeKinds();
            ComposeRows();
            FillGrid();
            RefreshPreview();
        }

        /// <summary>
        /// 候補一覧の行を組み直す: 人名・組織・住所の列の値（[A列]）→ 検知した候補 → 手動追記。
        /// 列の値と同じ語の候補は列の行に吸収する（同じ語が 2 行あると件数もチェックも割れる）。
        /// </summary>
        private void ComposeRows()
        {
            _rows.Clear();
            var covered = new HashSet<string>();
            foreach (var col in _columns)
            {
                if (!Kinds.IsDictionaryKind(col.Kind)) continue;
                foreach (var v in col.DistinctValues())
                {
                    if (!covered.Add(v)) continue;
                    // 「-」「なし」のような定型値は最初から外す（1 回だけ。利用者が戻したら尊重する）
                    if (ColumnRules.PlaceholderValues.Contains(v) && _seeded.Add(v)) _unchecked.Add(v);
                    var ner = _nerRows.FirstOrDefault(r => r.Text == v);
                    _rows.Add(new CandidateRow { Text = v, Type = col.Kind, Column = col.Letter, Detected = ner?.Detected ?? 0 });
                }
            }
            foreach (var r in _nerRows)
                if (!covered.Contains(r.Text)) _rows.Add(r);
            foreach (var m in _manualRows)
                if (!covered.Contains(m.Text) && !_rows.Any(r => r.Text == m.Text && r.Type == m.Type)) _rows.Add(m);
        }

        private void FillGrid()
        {
            _fillingGrid = true;
            try
            {
                _grid.Rows.Clear();
                foreach (var col in _columns)
                {
                    var i = _grid.Rows.Add(col.Letter, col.Header.Length > 0 ? col.Header : "（見出しなし）", Kinds.Label(col.Kind), "");
                    var row = _grid.Rows[i];
                    row.Tag = col;
                    row.Cells[ColKind].ToolTipText = col.Reason;
                    row.Cells[ColHeader].ToolTipText = col.Reason;
                    if (col.Header.Length == 0) row.Cells[ColHeader].Style.ForeColor = SystemColors.GrayText;
                }
                UpdateGridCounts();
            }
            finally { _fillingGrid = false; }
        }

        private void UpdateGridCounts()
        {
            foreach (DataGridViewRow row in _grid.Rows)
            {
                var col = row.Tag as ColumnInfo;
                if (col == null) continue;
                row.Cells[ColMasked].Value = col.Kind == Kinds.None ? "—" : col.MaskedCells + "/" + col.DataCells.Count;
                var partial = Kinds.IsWholeCellKind(col.Kind) && col.MaskedCells < col.DataCells.Count;
                row.Cells[ColMasked].Style.ForeColor = partial ? Color.Firebrick : SystemColors.ControlText;
                row.Cells[ColMasked].ToolTipText = partial ? "定型値（- や なし）とチェックを外した値のセルは伏せません" : "";
            }
        }

        private void OnKindChanged(int rowIndex)
        {
            var col = _grid.Rows[rowIndex].Tag as ColumnInfo;
            if (col == null) return;
            var k = Kinds.FromLabel(Convert.ToString(_grid.Rows[rowIndex].Cells[ColKind].Value));
            if (k == col.Kind) return;
            col.Kind = k;
            col.Reason = "手動";
            _grid.Rows[rowIndex].Cells[ColKind].ToolTipText = col.Reason;
            ComposeRows();
            RefreshPreview();
        }

        private void SelectColumn(int rowIndex)
        {
            var col = _grid.Rows[rowIndex].Tag as ColumnInfo;
            if (col == null || _snap == null || col.DataCells.Count == 0) return;
            SheetIO.SelectRange(_snap, DataRow1, col.Col, _maxRow, col.Col);
        }

        /// <summary>load/preview の cells → 候補ごとのセル一覧と件数。番号系（term 空）は自動扱い。</summary>
        private void ApplySpans(JArray cells)
        {
            _cellsByTerm = new Dictionary<string, List<CellText>>();
            _maskedList.Clear();
            _autoCells = 0;
            _maskedCells = 0;
            foreach (var col in _columns) col.MaskedCells = 0;
            if (cells != null && _snap != null)
            {
                var colByIndex = _columns.ToDictionary(c => c.Col);
                foreach (var c in cells)
                {
                    var ct = new CellText { Row = (int)c["r"], Col = (int)c["c"] };
                    var spans = (JArray)c["spans"];
                    if (spans == null || spans.Count == 0) continue;
                    _maskedCells++;
                    _maskedList.Add(ct);
                    ColumnInfo col;
                    if (colByIndex.TryGetValue(ct.Col, out col) && ct.Row >= DataRow1) col.MaskedCells++;
                    var onlyAuto = true;
                    var seen = new HashSet<string>();
                    foreach (var sp in spans)
                    {
                        var term = (string)sp[3] ?? "";
                        if (term.Length == 0) continue;
                        onlyAuto = false;
                        if (!seen.Add(term)) continue;  // 同じセルに同じ語が 2 回出ても 1 セル
                        List<CellText> list;
                        if (!_cellsByTerm.TryGetValue(term, out list)) _cellsByTerm[term] = list = new List<CellText>();
                        list.Add(ct);
                    }
                    if (onlyAuto) _autoCells++;
                }
            }
            foreach (var r in _rows)
            {
                List<CellText> list;
                r.MaskCells = _cellsByTerm.TryGetValue(r.Text, out list) ? list.Count : 0;
            }
            UpdateGridCounts();
            _lnkSelectAll.Text = _snap == null ? "" : _maskedCells == 0
                ? "マスク対象のセルはありません"
                : "マスク対象 " + _maskedCells + " セルをシートで選択して確認する";
        }

        private void RebuildList()
        {
            _rebuildingList = true;
            try
            {
                var sel = _list.SelectedIndex;
                _list.BeginUpdate();
                _list.Items.Clear();
                // 列指定の列の値は既定で隠す。列ごと伏せると決めた以上、1 件ずつ見る必要はない。
                // 値ごとに外したいときだけ「詳細」のチェックで出す（定型値はチェックが外れた状態で見える）。
                _listRows.Clear();
                foreach (var r in _rows)
                    if (r.Column == null || _chkShowColumnValues.Checked) _listRows.Add(r);
                foreach (var r in _listRows) _list.Items.Add(r, !_unchecked.Contains(r.Text));
                _list.EndUpdate();
                if (sel >= 0 && sel < _list.Items.Count) _list.SelectedIndex = sel;
                var hidden = _rows.Count - _listRows.Count;
                _chkShowColumnValues.Text = "列指定の列の値も候補一覧に出す（値ごとに外す）" + (hidden > 0 && !_chkShowColumnValues.Checked ? "　" + hidden + " 件" : "");
                _lblCandidates.Text = "自由記述の候補 (" + _listRows.Count + ")" +
                    (hidden > 0 && !_chkShowColumnValues.Checked ? "　列指定の値 " + hidden + " 件は非表示" : "") +
                    (_listRows.Count > 0 ? CandidatesHint : "");
            }
            finally
            {
                _rebuildingList = false;
            }
        }

        // ------------------------------------------------------------ 列の扱い・opt-out・手動追記・プレビュー
        /// <summary>
        /// プレビュー更新。statusText を渡すと (候補数, マスク対象セル数) から文言を作って出す。
        /// </summary>
        private void RefreshPreview(Func<int, int, string> statusText = null, bool statusError = false)
        {
            if (_snap == null) return;
            var confirmed = ConfirmedTerms();
            var columns = ColumnsJson();
            RunAsync("プレビュー更新中…",
                () => { var r = H.Preview(confirmed, columns); RequireOk(r, "プレビューに失敗しました"); return r; },
                resp =>
                {
                    ApplySpans((JArray)resp["cells"]);
                    RebuildList();
                    if (statusText != null)
                        SetStatus(statusText(_rows.Count, _maskedCells), statusError);
                    else
                        SetStatus(_snap.Description + "　マスク対象 " + _maskedCells + " セル（うち番号系のみ " + _autoCells + " セル）");
                });
        }

        private void AddManual()
        {
            if (_snap == null) throw new InvalidOperationException("先に「読み込んで検出」を実行してください。");
            var text = _txtManual.Text.Trim();
            if (text.Length == 0) return;
            var type = _cmbType.SelectedIndex == 1 ? Kinds.Org : _cmbType.SelectedIndex == 2 ? Kinds.Loc : Kinds.Person;
            _txtManual.Clear();
            if (_manualRows.Any(r => r.Text == text && r.Type == type)) return;
            _manualRows.Add(new CandidateRow { Text = text, Type = type, Manual = true });
            _unchecked.Remove(text);
            ComposeRows();
            RebuildList();
            RefreshPreview();
        }

        /// <summary>一覧で選んだ候補が伏せられるセルをシートで選択する（語ごとのプレビュー）。</summary>
        private void JumpToSelected()
        {
            if (_snap == null) return;
            var i = _list.SelectedIndex;
            if (i < 0 || i >= _listRows.Count) return;
            List<CellText> cells;
            if (!_cellsByTerm.TryGetValue(_listRows[i].Text, out cells) || cells.Count == 0)
            {
                SetStatus("「" + _listRows[i].Text + "」が伏せられるセルはありません（チェックが外れているか、他の候補に含まれています）。");
                return;
            }
            var n = SheetIO.SelectCells(_snap, cells);
            SetStatus("「" + _listRows[i].Text + "」: " + cells.Count + " セル" + (n < cells.Count ? "（先頭 " + n + " セルを選択）" : "を選択しました") +
                      "。残すならもう一度クリックしてチェックを外します。");
        }

        /// <summary>列指定・候補・番号系を合わせた最終結果のプレビュー: 変わるセルを全部選択する。</summary>
        private void SelectAllMasked()
        {
            if (_snap == null) return;
            if (_maskedList.Count == 0) { SetStatus("マスクされるセルはありません。"); return; }
            var n = SheetIO.SelectCells(_snap, _maskedList, maxCells: 2000);
            SetStatus("マスク対象 " + _maskedList.Count + " セル" + (n < _maskedList.Count ? "（先頭 " + n + " セルを選択）" : "を選択しました") +
                      "。選択されたセルがサニタイズで置き換わります。");
        }

        // ------------------------------------------------------------ サニタイズ
        /// <summary>
        /// 1 手順: ブックの保存先を聞く → サニタイズ → （可逆なら）対応表を書く → 別ブックを作って保存。
        ///
        /// 対応表は「保存し忘れると戻せない」唯一の成果物なので、ブックの保存先から自動で派生させて
        /// 必ず一緒に書く。置き場所は同じフォルダの下の `対応表\`——サニタイズ済みブックは AI に添付する
        /// ファイル、対応表は絶対に添付してはいけないファイルなので、xlsx を選ぶ一覧に並ばないようにする。
        /// 順序は 対応表 → ブック。対応表が書けなければブックも作らない（fail closed）。
        /// </summary>
        private void SanitizeToNewWorkbook()
        {
            if (_snap == null) throw new InvalidOperationException("先に「読み込んで検出」を実行してください。");
            var reversible = _chkReversible.Checked;
            var encrypt = reversible && _chkEncrypt.Checked;
            var pass = encrypt ? _txtPass.Text : "";
            if (encrypt && pass.Length == 0)
                throw new InvalidOperationException("空のパスフレーズでは保護できません。パスフレーズを入力するか、保護を外してください。");

            string bookPath;
            using (var dlg = new SaveFileDialog
            {
                Title = "サニタイズ済みブックの保存先",
                FileName = (_snap.WorkbookBaseName.Length > 0 ? _snap.WorkbookBaseName : "sanitized") + "_sanitized.xlsx",
                InitialDirectory = _snap.WorkbookDir.Length > 0 && System.IO.Directory.Exists(_snap.WorkbookDir)
                    ? _snap.WorkbookDir : Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
                Filter = "Excel ブック (*.xlsx)|*.xlsx",
                DefaultExt = "xlsx",
                AddExtension = true,
                OverwritePrompt = true,
            })
            {
                if (dlg.ShowDialog(this) != DialogResult.OK) return;   // キャンセル: 何も作らない
                bookPath = dlg.FileName;
            }
            var mappingDir = System.IO.Path.Combine(System.IO.Path.GetDirectoryName(bookPath) ?? ".", "対応表");
            var mappingPath = reversible
                ? System.IO.Path.Combine(mappingDir, System.IO.Path.GetFileNameWithoutExtension(bookPath) + ".jsonl")
                : null;

            var confirmed = ConfirmedTerms();
            var columns = ColumnsJson();
            var snap = _snap;
            RunAsync("サニタイズ中…",
                () =>
                {
                    var r = H.Sanitize(confirmed, reversible, columns);
                    RequireOk(r, "サニタイズに失敗しました");
                    if (reversible && (int)r["mapping_count"] > 0)
                    {
                        System.IO.Directory.CreateDirectory(mappingDir);
                        RequireOk(H.SaveMapping(mappingPath, pass), "対応表を保存できませんでした（ブックは作成していません）");
                    }
                    return r;
                },
                resp =>
                {
                    _txtPass.Clear();
                    var repl = new Dictionary<long, string>();
                    foreach (var c in (JArray)resp["cells"])
                        repl[SheetIO.Key((int)c["r"], (int)c["c"])] = (string)c["text"];
                    var leaks = ((JArray)resp["leaks"])?.Select(x => (string)x).ToList() ?? new List<string>();
                    var excluded = ((JArray)resp["excludedLeaks"])?.Select(x => (string)x).ToList() ?? new List<string>();
                    var mappingCount = (int)resp["mapping_count"];

                    // 列指定の検証: 伏せると決めた列のセルが全部置き換わっているか。ヘルパーの
                    // 構造上は外れないが、ここで止めれば「列まるごと」の約束を別ブックに書く前に守れる。
                    var missing = new List<string>();
                    foreach (var col in _columns)
                    {
                        if (!Kinds.IsWholeCellKind(col.Kind)) continue;
                        foreach (var cell in col.DataCells)
                        {
                            var v = cell.Text.Trim();
                            if (v.Length == 0 || SkipValue(col, v)) continue;
                            if (!repl.ContainsKey(SheetIO.Key(cell.Row, cell.Col)))
                                missing.Add(col.Letter + cell.Row);
                        }
                    }
                    var mappingWritten = reversible && mappingCount > 0;
                    if (missing.Count > 0)
                    {
                        if (mappingWritten) DeleteQuietly(mappingPath);
                        throw new InvalidOperationException("列指定したセルの一部が置き換わっていません。ブックも対応表も作成しませんでした: " +
                                                            string.Join(", ", missing.Take(10)) + (missing.Count > 10 ? " …" : ""));
                    }

                    try
                    {
                        SheetIO.WriteOutputWorkbook(snap, repl, bookPath);   // メインスレッド（COM）
                    }
                    catch (Exception)
                    {
                        if (mappingWritten) DeleteQuietly(mappingPath);   // ブックが無いのに対応表だけ残さない
                        throw;
                    }

                    // 見出し → 種別 を覚える（セルの値は書かない）
                    if (HeaderRow > 0)
                        _memory.Remember(_columns.Select(c => c.Header).ToList(), _columns.Select(c => c.Kind).ToList());

                    _result.Text = "保存: " + System.IO.Path.GetFileName(bookPath) + "　" + repl.Count + " セルをマスク" +
                                   (reversible ? "、対応表 " + mappingCount + " 件" : "（不可逆・対応表なし）") +
                                   (leaks.Count == 0 ? "　安全ゲート: 通過" : "　⚠ 安全ゲート: " + leaks.Count + " 件が残存") +
                                   (excluded.Count > 0 ? "　対象外の列に " + excluded.Count + " 件の値が残っています" : "");
                    _result.ForeColor = leaks.Count == 0 ? (excluded.Count == 0 ? Color.DarkGreen : Color.DarkGoldenrod) : Color.Firebrick;

                    var where = "サニタイズ済みブック: " + bookPath +
                                (mappingWritten
                                    ? "\n対応表: " + mappingPath + (pass.Length > 0 ? "（パスフレーズ保護）" : "（実名を含みます。AI には渡さないでください）")
                                    : reversible ? "\n対応表: なし（マスクした値がありませんでした）" : "\n不可逆モードのため対応表はありません。");
                    if (leaks.Count > 0)
                        MessageBox.Show(this,
                            "サニタイズ後の出力に、マスクしたはずの値が残っています。送信前に必ず確認してください。\n\n" +
                            string.Join("\n", leaks.Take(10)) + (leaks.Count > 10 ? "\n…" : ""),
                            "安全ゲート", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    if (excluded.Count > 0)
                        SetStatus(where + "\n「対象外」にした列に、他の列で伏せた値が残っています（意図したものか確認してください）: " +
                                  string.Join(", ", excluded.Take(5)) + (excluded.Count > 5 ? " …" : ""), error: true);
                    else
                        SetStatus(where, error: leaks.Count > 0);
                });
        }

        private static void DeleteQuietly(string path)
        {
            try { if (!string.IsNullOrEmpty(path) && System.IO.File.Exists(path)) System.IO.File.Delete(path); }
            catch (Exception) { }
        }

        // ------------------------------------------------------------ リセット
        private void ResetAll()
        {
            if (H.Connected)
            {
                try { H.Reset(); } catch (Exception) { }
            }
            _snap = null;
            _lastLoadCells = null;
            _columns.Clear();
            _nerRows.Clear();
            _manualRows.Clear();
            _rows.Clear();
            _listRows.Clear();
            _unchecked.Clear();
            _seeded.Clear();
            _maskedList.Clear();
            _cellsByTerm.Clear();
            _fillingGrid = true;
            try { _grid.Rows.Clear(); } finally { _fillingGrid = false; }
            _rebuildingList = true;
            try { _list.Items.Clear(); } finally { _rebuildingList = false; }
            _lblCandidates.Text = "自由記述の候補";
            _lnkSelectAll.Text = "";
            _result.Text = "";
            _txtPass.Clear();
            SetStatus("リセットしました。");
        }

        // ------------------------------------------------------------ passphrase
        /// <summary>
        /// GUI と同じ仕様（docs/mapping-encryption.md）: 紛らわしい文字（I/l/1, O/0）を除いた
        /// 31 文字から 20 文字、5 文字 × 4 グループ。約 99 bit。乱数は CSPRNG。
        /// </summary>
        private static string GeneratePassphrase()
        {
            const string alphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
            var sb = new StringBuilder();
            using (var rng = RandomNumberGenerator.Create())
            {
                var one = new byte[1];
                var limit = 256 - 256 % alphabet.Length;  // 偏りを避ける（棄却サンプリング）
                for (var g = 0; g < 4; g++)
                {
                    if (g > 0) sb.Append('-');
                    for (var k = 0; k < 5; k++)
                    {
                        do rng.GetBytes(one); while (one[0] >= limit);
                        sb.Append(alphabet[one[0] % alphabet.Length]);
                    }
                }
            }
            return sb.ToString();
        }
    }
}
