//! HuggingFace `tokenizers` に C ABI を生やす薄いシム。
//!
//! Python 版は transformers の fast tokenizer（＝この Rust クレート）を使うので、同じ核に
//! 繋げば input_ids が一致する（Phase 0 で 39/39 実測済み）。
//! さらに **offsets（offset_mapping）** を返す。HF の aggregation_strategy="simple" は
//! トークンの文字オフセットからエンティティのスパンを組むため、これが無いと hf_spans を作れない。
//!
//! 設計は sudachi-shim と同じ: 不透明ハンドル + 結果構造体 + 明示解放。

use std::ffi::{c_char, c_int, CStr};
use std::ptr;

use tokenizers::Tokenizer;

pub struct TokHandle {
    tk: Tokenizer,
}

/// エンコード結果。ids と offsets は同じ長さ。
#[repr(C)]
pub struct TokEncoding {
    pub ids: *mut u32,
    /// 各トークンの開始オフセット（**文字＝コードポイント単位**。Python の
    /// offset_mapping と同じ意味。実測で確認すること）
    pub begins: *mut usize,
    pub ends: *mut usize,
    /// 特殊トークン（<s>/</s>）なら 1。HF の集約は special_tokens_mask で読み飛ばす。
    pub special: *mut u8,
    pub len: usize,
}

/// tokenizer.json（HF形式）から構築する。失敗時は null。
///
/// # Safety
/// `path` は有効な NUL 終端 UTF-8。
#[no_mangle]
pub unsafe extern "C" fn tok_open(path: *const c_char) -> *mut TokHandle {
    if path.is_null() {
        return ptr::null_mut();
    }
    let Ok(p) = CStr::from_ptr(path).to_str() else {
        return ptr::null_mut();
    };
    match Tokenizer::from_file(p) {
        Ok(tk) => Box::into_raw(Box::new(TokHandle { tk })),
        Err(_) => ptr::null_mut(),
    }
}

/// テキストをエンコードする。`add_special_tokens` は必ず 1 を渡すこと
/// （xlm-roberta は <s>/</s> を前提に学習されており、0 だと推論が変わる）。
///
/// # Safety
/// `handle` は tok_open が返したもの、`text` は NUL 終端 UTF-8。
#[no_mangle]
pub unsafe extern "C" fn tok_encode(
    handle: *mut TokHandle,
    text: *const c_char,
    add_special_tokens: c_int,
    out: *mut TokEncoding,
) -> c_int {
    if handle.is_null() || text.is_null() || out.is_null() {
        return -1;
    }
    let Ok(s) = CStr::from_ptr(text).to_str() else {
        return -1;
    };
    let enc = match (*handle).tk.encode(s, add_special_tokens != 0) {
        Ok(e) => e,
        Err(_) => return -1,
    };

    let mut ids: Vec<u32> = enc.get_ids().to_vec();

    // **Rust の get_offsets() はバイト単位、Python の offset_mapping は文字単位**
    // （transformers が変換している）。実測: 3バイトの漢字で C++=(0,3) / Python=(0,1)。
    // そのまま返すと日本語で必ず壊れるので、ここでコードポイント単位に直す。
    // sudachi.rs の begin()/begin_c() と同じ構図。
    let mut b2c = vec![0usize; s.len() + 1];
    let mut ci = 0usize;
    for (bi, ch) in s.char_indices() {
        for k in 0..ch.len_utf8() {
            b2c[bi + k] = ci;
        }
        ci += 1;
    }
    b2c[s.len()] = ci;

    let mut begins: Vec<usize> = Vec::with_capacity(ids.len());
    let mut ends: Vec<usize> = Vec::with_capacity(ids.len());
    for (b, e) in enc.get_offsets() {
        begins.push(b2c[(*b).min(s.len())]);
        ends.push(b2c[(*e).min(s.len())]);
    }
    let mut special: Vec<u8> = enc.get_special_tokens_mask().iter().map(|x| *x as u8).collect();

    let len = ids.len();
    ids.shrink_to_fit();
    begins.shrink_to_fit();
    ends.shrink_to_fit();
    special.shrink_to_fit();
    (*out).ids = ids.as_mut_ptr();
    (*out).begins = begins.as_mut_ptr();
    (*out).ends = ends.as_mut_ptr();
    (*out).special = special.as_mut_ptr();
    (*out).len = len;
    std::mem::forget(ids);
    std::mem::forget(begins);
    std::mem::forget(ends);
    std::mem::forget(special);
    0
}

/// tok_encode が確保した結果を解放する。
///
/// # Safety
/// `enc` は tok_encode が書き込んだ結果で、二重解放しないこと。
#[no_mangle]
pub unsafe extern "C" fn tok_free_encoding(enc: *mut TokEncoding) {
    if enc.is_null() || (*enc).len == 0 {
        return;
    }
    let n = (*enc).len;
    drop(Vec::from_raw_parts((*enc).ids, n, n));
    drop(Vec::from_raw_parts((*enc).begins, n, n));
    drop(Vec::from_raw_parts((*enc).ends, n, n));
    drop(Vec::from_raw_parts((*enc).special, n, n));
    (*enc).len = 0;
}

/// # Safety
/// `handle` は tok_open が返したもので、二重解放しないこと。
#[no_mangle]
pub unsafe extern "C" fn tok_close(handle: *mut TokHandle) {
    if !handle.is_null() {
        drop(Box::from_raw(handle));
    }
}
