//! sudachi.rs に C ABI を生やす薄いシム。
//!
//! なぜ必要か: Python 版の `furigana.py` は SudachiPy の `part_of_speech()` / `reading_form()` /
//! `surface()` を使って氏名・社名の芯を切り出している。SudachiPy 0.6.11 は sudachi.rs（Rust）の
//! バインディングにすぎないので、C++ から同じ核に繋げば **品詞体系も辞書も同一** になり、
//! 挙動が完全一致する。MeCab に乗り換えると品詞体系が変わり、eval を測り直す羽目になる。
//!
//! 設計は Phase 0 の tokenizers-cpp と同じ流儀:
//!   不透明ハンドル + 結果構造体 + 明示解放。文字列は UTF-8 のまま渡す。

use std::ffi::{c_char, c_int, CStr, CString};
use std::path::PathBuf;
use std::ptr;

use sudachi::analysis::stateless_tokenizer::StatelessTokenizer;
use sudachi::analysis::{Mode, Tokenize};
use sudachi::config::Config;
use sudachi::dic::dictionary::JapaneseDictionary;

/// 辞書＋トークナイザを保持する不透明ハンドル。
pub struct SudachiHandle {
    dict: JapaneseDictionary,
}

/// 形態素1つ分。すべて所有権つきの C 文字列で返し、free で明示解放する。
#[repr(C)]
pub struct SudachiMorpheme {
    /// 表層形
    pub surface: *mut c_char,
    /// 品詞。SudachiPy の part_of_speech() と同じ6要素をタブ区切りで連結したもの。
    /// （C 側で配列を組むより、区切り文字1つの方が FFI が単純で壊れにくい）
    pub pos: *mut c_char,
    /// 読み（カタカナ）
    pub reading: *mut c_char,
    /// 原文中の開始位置（**文字＝コードポイント単位**）
    pub begin: usize,
    /// 原文中の終了位置（**文字＝コードポイント単位**）
    pub end: usize,
}

#[repr(C)]
pub struct SudachiResult {
    pub morphemes: *mut SudachiMorpheme,
    pub len: usize,
}

fn to_c(s: &str) -> *mut c_char {
    CString::new(s).unwrap_or_default().into_raw()
}

/// 辞書を開く。
///
/// - `config_path`: SudachiPy 同梱の `sudachipy/resources/sudachi.json`
/// - `resource_dir`: `char.def` / `unk.def` / `rewrite.def` のあるディレクトリ
/// - `system_dic`: `sudachidict_core/resources/system.dic`
///
/// **この3つは必ず SudachiPy が使っているものと同じ実体を渡すこと。** 設定に
/// ProlongedSoundMark / IgnoreYomigana / OOV 等のプラグインが並んでおり、
/// これが違うと分割結果が変わって Python 版と一致しなくなる。
///
/// 失敗時は null を返す。
///
/// # Safety
/// 3つのポインタは有効な NUL 終端 UTF-8 文字列を指していること。
#[no_mangle]
pub unsafe extern "C" fn sudachi_open(
    config_path: *const c_char,
    resource_dir: *const c_char,
    system_dic: *const c_char,
) -> *mut SudachiHandle {
    let cstr = |p: *const c_char| -> Option<PathBuf> {
        if p.is_null() {
            return None;
        }
        CStr::from_ptr(p).to_str().ok().map(PathBuf::from)
    };
    let (Some(cfg), Some(res), Some(dic)) =
        (cstr(config_path), cstr(resource_dir), cstr(system_dic))
    else {
        return ptr::null_mut();
    };

    let config = match Config::new(Some(cfg), Some(res), Some(dic)) {
        Ok(c) => c,
        Err(_) => return ptr::null_mut(),
    };
    match JapaneseDictionary::from_cfg(&config) {
        Ok(dict) => Box::into_raw(Box::new(SudachiHandle { dict })),
        Err(_) => ptr::null_mut(),
    }
}

/// テキストを形態素解析する。`mode`: 0=A, 1=B, 2=C（Python 版は SplitMode.C を使う）。
///
/// # Safety
/// `handle` は sudachi_open が返した有効なポインタ、`text` は NUL 終端 UTF-8。
#[no_mangle]
pub unsafe extern "C" fn sudachi_tokenize(
    handle: *mut SudachiHandle,
    text: *const c_char,
    mode: c_int,
    out: *mut SudachiResult,
) -> c_int {
    if handle.is_null() || text.is_null() || out.is_null() {
        return -1;
    }
    let Ok(input) = CStr::from_ptr(text).to_str() else {
        return -1;
    };
    let split = match mode {
        0 => Mode::A,
        1 => Mode::B,
        _ => Mode::C,
    };

    let tokenizer = StatelessTokenizer::new(&(*handle).dict);
    let morphemes = match tokenizer.tokenize(input, split, false) {
        Ok(m) => m,
        Err(_) => return -1,
    };

    let mut items: Vec<SudachiMorpheme> = Vec::with_capacity(morphemes.len());
    for m in morphemes.iter() {
        // SudachiPy の part_of_speech() は6要素のタプル。同じ並びをタブで連結する。
        let pos = m.part_of_speech().join("\t");
        // surface() は Ref<str>（RefCell 借用）を返すので、CString 化の前に実体化する。
        let surface = m.surface().to_string();
        items.push(SudachiMorpheme {
            surface: to_c(&surface),
            pos: to_c(&pos),
            reading: to_c(m.reading_form()),
            // **必ず begin_c/end_c（コードポイント）を使う。** begin/end はバイト単位で、
            // SudachiPy の begin()/end() は文字単位（実測: 「株式会社あおぞら物産」で 0..10）。
            // ここを取り違えると furigana の name[begin:end] が日本語で必ず壊れる。
            begin: m.begin_c(),
            end: m.end_c(),
        });
    }
    items.shrink_to_fit();
    let len = items.len();
    let ptr_ = items.as_mut_ptr();
    std::mem::forget(items);
    (*out).morphemes = ptr_;
    (*out).len = len;
    0
}

/// sudachi_tokenize が確保した結果を解放する。
///
/// # Safety
/// `res` は sudachi_tokenize が書き込んだ結果で、二重解放しないこと。
#[no_mangle]
pub unsafe extern "C" fn sudachi_free_result(res: *mut SudachiResult) {
    if res.is_null() || (*res).morphemes.is_null() {
        return;
    }
    let items = Vec::from_raw_parts((*res).morphemes, (*res).len, (*res).len);
    for m in items {
        drop(CString::from_raw(m.surface));
        drop(CString::from_raw(m.pos));
        drop(CString::from_raw(m.reading));
    }
    (*res).morphemes = ptr::null_mut();
    (*res).len = 0;
}

/// 辞書を閉じる。
///
/// # Safety
/// `handle` は sudachi_open が返したもので、二重解放しないこと。
#[no_mangle]
pub unsafe extern "C" fn sudachi_close(handle: *mut SudachiHandle) {
    if !handle.is_null() {
        drop(Box::from_raw(handle));
    }
}
