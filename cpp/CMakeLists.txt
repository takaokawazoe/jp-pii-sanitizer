cmake_minimum_required(VERSION 3.20)
project(jp_pii_sanitizer LANGUAGES C CXX)  # C は miniz(ZIP) のため

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ソースは UTF-8（日本語コメント・文字列を含む）。MSVC は既定でシステムコードページ
# （このマシンは cp932）としてソースを読むため、/utf-8 を渡さないと日本語が壊れて
# 構文エラーになる（gcc/clang は既定 UTF-8 なので Linux では不要だった）。
# /utf-8 は実行文字セットも UTF-8 にするので、Python(UTF-8) とのバイト一致にも一致する。
if(MSVC)
  add_compile_options(/utf-8)
endif()

# CMake 4.x は cmake_minimum_required < 3.5 を拒否する。third_party（pcre2 等）に
# 古い最小要求が混じるので、サブディレクトリ取り込み前に互換ポリシーを緩める。
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# ---- プラットフォーム差の一元化（Windows/MSVC 対応）----
# Linux は .so を直リンク＋rpath。Windows は import lib(.lib) をリンクし DLL を exe の隣へ置く
# （rpath 相当が無いため）。Rust の staticlib は Linux で lib*.a・Windows で *.lib。
# システムライブラリも pthread/dl/m（Linux）と Win32 群（Windows）で異なる。
if(WIN32)
  set(ORT_LIB    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/lib/onnxruntime.lib)
  set(SYS_LIBS   ws2_32 bcrypt advapi32 userenv ntdll)  # Rust std(windows-msvc) が要求する native libs
else()
  set(ORT_LIB    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/lib/libonnxruntime.so)
  set(SYS_LIBS   pthread dl m)
endif()

# Rust cdylib/staticlib の成果物パスをプラットフォーム別に組む。
macro(rust_static_lib OUTVAR DIR NAME)
  if(WIN32)
    set(${OUTVAR} ${DIR}/target/release/${NAME}.lib)
  else()
    set(${OUTVAR} ${DIR}/target/release/lib${NAME}.a)
  endif()
endmacro()

# Windows: リンクした DLL を実行ファイルの隣にコピー（rpath が無いため）。Linux では no-op。
function(copy_runtime_dll TGT)
  if(WIN32)
    foreach(dll ${ARGN})
      add_custom_command(TARGET ${TGT} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dll} $<TARGET_FILE_DIR:${TGT}>
        VERBATIM)
    endforeach()
  endif()
endfunction()

set(ORT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime)
set(ORT_DLLS ${ORT_DIR}/lib/onnxruntime.dll ${ORT_DIR}/lib/onnxruntime_providers_shared.dll)
if(NOT EXISTS ${ORT_DIR}/include/onnxruntime_cxx_api.h)
  message(FATAL_ERROR "ONNX Runtime が見つかりません: ${ORT_DIR}\n"
                      "cpp/README.md の手順でプリビルドを配置してください。")
endif()

add_executable(phase0_parity src/phase0_parity.cpp)
target_include_directories(phase0_parity PRIVATE
  ${ORT_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(phase0_parity PRIVATE ${ORT_LIB})
set_target_properties(phase0_parity PROPERTIES
  BUILD_RPATH ${ORT_DIR}/lib
  INSTALL_RPATH ${ORT_DIR}/lib)
copy_runtime_dll(phase0_parity ${ORT_DLLS})

# --- 形態素解析（sudachi.rs に C ABI を生やした自作シム） ---
# Python 版の SudachiPy 0.6.11 は sudachi.rs のバインディングなので、同じ Rust の核に繋ぐ。
# MeCab に乗り換えると品詞体系が変わり eval を測り直す羽目になる。
set(SHIM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/sudachi-shim)
rust_static_lib(SHIM_LIB ${SHIM_DIR} sudachi_shim)
find_program(CARGO_EXE cargo HINTS $ENV{HOME}/.cargo/bin $ENV{USERPROFILE}/.cargo/bin)
if(CARGO_EXE)
  add_custom_command(
    OUTPUT ${SHIM_LIB}
    COMMAND ${CARGO_EXE} build --release
    WORKING_DIRECTORY ${SHIM_DIR}
    DEPENDS ${SHIM_DIR}/src/lib.rs ${SHIM_DIR}/Cargo.toml
    COMMENT "Building sudachi-shim (Rust)"
    VERBATIM)
  add_custom_target(sudachi_shim_build DEPENDS ${SHIM_LIB})

  add_executable(phase1_sudachi src/phase1_sudachi.cpp)
  add_dependencies(phase1_sudachi sudachi_shim_build)
  target_include_directories(phase1_sudachi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/src)
  # staticlib はシステムライブラリを自前で解決しないのでここで足す
  target_link_libraries(phase1_sudachi PRIVATE ${SHIM_LIB} ${SYS_LIBS})
else()
  message(WARNING "cargo が見つからないため phase1_sudachi をスキップします。"
                  " PATH に ~/.cargo/bin（Windows は %USERPROFILE%\\.cargo\\bin）を通してください。")
endif()

# --- トークナイザ（HF tokenizers に C ABI を生やした自作シム） ---
# tokenizers-cpp（mlc-ai）は使わない: 公開 Encode が add_special_tokens=false 決め打ちで、
# かつ offsets を FFI が公開していない（HF の集約に必須）。sudachi-shim と同じ流儀で自作する。
# ※ このブロックは PCRE2 より前に置くこと。TOKSHIM_LIB を後段の phase2_hf が参照する。
set(TOKSHIM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tokenizer-shim)
rust_static_lib(TOKSHIM_LIB ${TOKSHIM_DIR} tokenizer_shim)
if(CARGO_EXE)
  add_custom_command(
    OUTPUT ${TOKSHIM_LIB}
    COMMAND ${CARGO_EXE} build --release
    WORKING_DIRECTORY ${TOKSHIM_DIR}
    DEPENDS ${TOKSHIM_DIR}/src/lib.rs ${TOKSHIM_DIR}/Cargo.toml
    COMMENT "Building tokenizer-shim (Rust)"
    VERBATIM)
  add_custom_target(tokenizer_shim_build DEPENDS ${TOKSHIM_LIB})

  add_executable(phase0_tokenize src/phase0_tokenize.cpp)
  add_dependencies(phase0_tokenize tokenizer_shim_build)
  target_include_directories(phase0_tokenize PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(phase0_tokenize PRIVATE ${TOKSHIM_LIB} ${SYS_LIBS})
endif()

# --- 正規表現（PCRE2） ---
# RE2 ではない: tokenizer.py の PHONE_RE/POSTAL_RE が後読み (?<![\d\-]) を使っており、
# RE2 は後読み・先読みを実装していない。PCRE2 は Perl 互換で Python re に近い。
# システムに libpcre2-8 はあるが開発ヘッダが無いので、ONNX Runtime 等と同じく
# third_party にソースを置いてビルドする（sudo 不要・Windows でも同じ手が使える）。
set(PCRE2_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pcre2)
if(EXISTS ${PCRE2_DIR}/CMakeLists.txt)
  set(PCRE2_BUILD_PCRE2_8 ON CACHE BOOL "" FORCE)
  set(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "" FORCE)
  set(PCRE2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(PCRE2_SUPPORT_UNICODE ON CACHE BOOL "" FORCE)  # PCRE2_UTF/PCRE2_UCP に必須
  add_subdirectory(${PCRE2_DIR} pcre2 EXCLUDE_FROM_ALL)

  add_executable(phase2_regex src/phase2_regex.cpp)
  target_include_directories(phase2_regex PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/pcre2)
  target_link_libraries(phase2_regex PRIVATE pcre2-8)

  # HF NER（ONNX＋トークナイザ＋simple集約）の等価性
  if(CARGO_EXE)
    add_executable(phase2_hf src/phase2_hf.cpp)
    add_dependencies(phase2_hf tokenizer_shim_build)
    target_include_directories(phase2_hf PRIVATE
      ${ORT_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(phase2_hf PRIVATE
      ${ORT_LIB} ${TOKSHIM_LIB} ${SYS_LIBS})
    set_target_properties(phase2_hf PROPERTIES BUILD_RPATH ${ORT_DIR}/lib)
    copy_runtime_dll(phase2_hf ${ORT_DLLS})

    # Phase 2 完成条件: 検知パイプライン全体（regex＋HF＋sudachi＋トリム＋集約）
    add_executable(phase2_pipeline src/phase2_pipeline.cpp)
    add_dependencies(phase2_pipeline tokenizer_shim_build sudachi_shim_build)
    target_include_directories(phase2_pipeline PRIVATE
      ${ORT_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_BINARY_DIR}/pcre2)
    target_link_libraries(phase2_pipeline PRIVATE
      ${ORT_LIB} ${TOKSHIM_LIB} ${SHIM_LIB} pcre2-8 ${SYS_LIBS})
    set_target_properties(phase2_pipeline PROPERTIES BUILD_RPATH ${ORT_DIR}/lib)
    copy_runtime_dll(phase2_pipeline ${ORT_DLLS})

    # Phase 3: トークン化・逆置換（ONNX は不要。sudachi はフリガナ名寄せで使う）
    add_executable(phase3_tokenize src/phase3_tokenize.cpp)
    add_dependencies(phase3_tokenize sudachi_shim_build)
    target_include_directories(phase3_tokenize PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_BINARY_DIR}/pcre2)
    target_link_libraries(phase3_tokenize PRIVATE ${SHIM_LIB} pcre2-8 ${SYS_LIBS})

    # Phase 5 コア残り: 安全ゲート・ハイライト・束ね（ONNX 不要。sudachi＋CSV 読みに extractors）
    add_executable(phase5_core src/phase5_core.cpp)
    add_dependencies(phase5_core sudachi_shim_build)
    target_include_directories(phase5_core PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_BINARY_DIR}/pcre2)
    target_link_libraries(phase5_core PRIVATE ${SHIM_LIB} pcre2-8 ${SYS_LIBS})

    # Phase 4: 入口（extractors）。OOXML = ZIP(miniz) + XML(pugixml)、PDF = PDFium。
    set(PDFIUM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium)
    if(WIN32)
      set(PDFIUM_LIB ${PDFIUM_DIR}/lib/pdfium.dll.lib)
      set(PDFIUM_DLL ${PDFIUM_DIR}/bin/pdfium.dll)
    else()
      set(PDFIUM_LIB ${PDFIUM_DIR}/lib/libpdfium.so)
    endif()
    add_executable(phase4_extract src/phase4_extract.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz/miniz.c
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugixml/src/pugixml.cpp)
    target_include_directories(phase4_extract PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugixml/src
      ${PDFIUM_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_BINARY_DIR}/pcre2)
    target_link_libraries(phase4_extract PRIVATE pcre2-8 ${PDFIUM_LIB})
    set_target_properties(phase4_extract PROPERTIES BUILD_RPATH ${PDFIUM_DIR}/lib)
    if(WIN32)
      copy_runtime_dll(phase4_extract ${PDFIUM_DLL})
    endif()

    # 対応表のパスフレーズ暗号化（Argon2id + XChaCha20-Poly1305）。設計は
    # docs/mapping-encryption.md。KDF がオフライン総当たりに対する主防御なので、
    # Argon2id を持つ libsodium を使う（PBKDF2 しか無い OS API では代替できない）。
    #
    # Windows: 公式 MSVC プリビルドを third_party に vendoring し**静的リンク**する
    #          （配布 ZIP に DLL を増やさないため）。
    # POSIX  : 配布パッケージ（apt: libsodium-dev / brew: libsodium）。
    #          SODIUM_ROOT を渡せばそこを優先する（自前ビルドを使う開発環境向け）。
    if(WIN32)
      set(SODIUM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libsodium)
      set(SODIUM_INCLUDE ${SODIUM_DIR}/include)
      set(SODIUM_LIB ${SODIUM_DIR}/x64/Release/v143/static/libsodium.lib)
      if(NOT EXISTS ${SODIUM_LIB})
        message(FATAL_ERROR "libsodium が見つかりません: ${SODIUM_LIB}\n"
                            "cpp/README.md の手順で libsodium-*-msvc.zip を配置してください。")
      endif()
      # 静的リンク時は SODIUM_STATIC が必須。未定義だと SODIUM_EXPORT が
      # __declspec(dllimport) に展開され、リンクが未解決シンボルで落ちる。
      set(SODIUM_DEFS SODIUM_STATIC=1)
    else()
      find_path(SODIUM_INCLUDE sodium.h HINTS ${SODIUM_ROOT}/include)
      find_library(SODIUM_LIB NAMES sodium HINTS ${SODIUM_ROOT}/lib)
      if(NOT SODIUM_INCLUDE OR NOT SODIUM_LIB)
        message(FATAL_ERROR "libsodium が見つかりません。"
                            "apt install libsodium-dev / brew install libsodium、"
                            "または -DSODIUM_ROOT=<prefix> を指定してください。")
      endif()
      set(SODIUM_DEFS "")
    endif()

    # 単体試験（オラクル比較の phase* とは別種）。文字コード境界・数値パース・
    # 対応表の読み書き（平文と暗号化）を、モデル辞書なしで検証する。
    add_executable(unit_tests src/unit_tests.cpp)
    target_include_directories(unit_tests PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${SODIUM_INCLUDE})
    target_link_libraries(unit_tests PRIVATE ${SODIUM_LIB})
    if(SODIUM_DEFS)
      target_compile_definitions(unit_tests PRIVATE ${SODIUM_DEFS})
    endif()

    # Phase 4c: .msg（Aspose.Email FOSS vendoring+patch）＋ NER候補一致で判定。
    # Aspose は C++17。gcc13 は -Wchanges-meaning がエラーになるので -fpermissive で回避する。
    # MSVC は該当診断が無い代わり、巨大な生成物・UTF-8 ソース対応に /bigobj /utf-8 が要る。
    set(ASPOSE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/aspose-email)
    if(WIN32)
      set(ASPOSE_LIB ${ASPOSE_DIR}/build/AsposeEmailFoss.lib)
      set(ASPOSE_EXTRA_FLAGS "/bigobj /utf-8 /EHsc")
      set(ASPOSE_MSG_FLAGS /bigobj /utf-8 /EHsc)
    else()
      set(ASPOSE_LIB ${ASPOSE_DIR}/build/libAsposeEmailFoss.a)
      set(ASPOSE_EXTRA_FLAGS "-Wno-changes-meaning -fpermissive")
      set(ASPOSE_MSG_FLAGS -Wno-changes-meaning -fpermissive)
    endif()
    if(EXISTS ${ASPOSE_DIR}/CMakeLists.txt)
      add_custom_command(
        OUTPUT ${ASPOSE_LIB}
        COMMAND ${CMAKE_COMMAND} -S ${ASPOSE_DIR} -B ${ASPOSE_DIR}/build -G Ninja
                -DCMAKE_BUILD_TYPE=Release
                -DCMAKE_POLICY_VERSION_MINIMUM=3.5
                "-DCMAKE_CXX_FLAGS=${ASPOSE_EXTRA_FLAGS}"
        COMMAND ${CMAKE_COMMAND} --build ${ASPOSE_DIR}/build --target AsposeEmailFoss
        COMMENT "Building Aspose.Email FOSS (patched)"
        VERBATIM)
      add_custom_target(aspose_build DEPENDS ${ASPOSE_LIB})

      # 完成条件は「PII取りこぼしゼロ」＝抽出テキストへの表層被覆なので ONNX/sudachi 不要。
      add_executable(phase4_msg src/phase4_msg.cpp)
      add_dependencies(phase4_msg aspose_build)
      target_compile_options(phase4_msg PRIVATE ${ASPOSE_MSG_FLAGS})
      target_include_directories(phase4_msg PRIVATE
        ${ASPOSE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}/pcre2)
      target_link_libraries(phase4_msg PRIVATE ${ASPOSE_LIB} pcre2-8 ${SYS_LIBS})

      # CLI（Phase A: prose）。WebView2 不要＝クロスプラットフォーム。msg.hpp を含むため
      # この TU は Aspose を取り込む（GUI facade core.hpp は Aspose 非依存のまま）。
      #
      # 対応表のパスフレーズ暗号化（docs/mapping-encryption.md）で libsodium を使う。
      # SODIUM_INCLUDE / SODIUM_LIB / SODIUM_DEFS は上流の libsodium ブロックで決まる。
      add_executable(jp_pii_sanitizer_cli src/cli/main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz/miniz.c
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugixml/src/pugixml.cpp)
      set_target_properties(jp_pii_sanitizer_cli PROPERTIES OUTPUT_NAME "jp-pii-sanitizer-cli")
      add_dependencies(jp_pii_sanitizer_cli aspose_build tokenizer_shim_build sudachi_shim_build)
      target_compile_options(jp_pii_sanitizer_cli PRIVATE ${ASPOSE_MSG_FLAGS})
      target_include_directories(jp_pii_sanitizer_cli PRIVATE
        ${ASPOSE_DIR}/include
        ${ORT_DIR}/include
        ${PDFIUM_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugixml/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}/pcre2
        ${SODIUM_INCLUDE})
      target_link_libraries(jp_pii_sanitizer_cli PRIVATE
        ${ASPOSE_LIB} ${ORT_LIB} ${TOKSHIM_LIB} ${SHIM_LIB} pcre2-8 ${PDFIUM_LIB}
        ${SODIUM_LIB} ${SYS_LIBS})
      if(SODIUM_DEFS)
        target_compile_definitions(jp_pii_sanitizer_cli PRIVATE ${SODIUM_DEFS})
      endif()
      if(WIN32)
        target_link_libraries(jp_pii_sanitizer_cli PRIVATE shell32)  # CommandLineToArgvW
      endif()
      set_target_properties(jp_pii_sanitizer_cli PROPERTIES BUILD_RPATH ${PDFIUM_DIR}/lib)
      if(WIN32)
        copy_runtime_dll(jp_pii_sanitizer_cli ${ORT_DLLS} ${PDFIUM_DLL})
      endif()
    endif()

    # Phase 5: WebView2 GUI（Windows 専用）。
    # この段階は最小疎通（ウィンドウ＋JS↔C++ ブリッジ）のみ。コア（extract_candidates 等）の
    # 配線は次段。WebView2 は DLL ローダ（WebView2Loader.dll）を exe の隣に置く定番構成。
    # 実行時は WebView2 Runtime（Evergreen・Win11 標準搭載）が別途必要。
    if(WIN32)
      enable_language(RC)  # アプリアイコン（app.rc）を .res にコンパイルするため
      set(WV2_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/build/native)
      # コア（extractors=miniz+pugixml、pdf=PDFium、検知=ONNX+shims+pcre2）を GUI exe に取り込む。
      # msg(Aspose) と csv 列指定は次段（core.hpp の割り切りを参照）。app.rc はアイコン。
      add_executable(jp_pii_sanitizer WIN32 src/app/winmain.cpp src/app/app.rc
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz/miniz.c
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugixml/src/pugixml.cpp)
      # 配布 exe 名はプロジェクト名（jp-pii-sanitizer.exe）にそろえる。CMake ターゲット識別子は
      # ハイフン不可なので jp_pii_sanitizer、出力ファイル名だけハイフンにする。
      set_target_properties(jp_pii_sanitizer PROPERTIES OUTPUT_NAME "jp-pii-sanitizer")
      add_dependencies(jp_pii_sanitizer tokenizer_shim_build sudachi_shim_build)
      # UNICODE/_UNICODE: Win32 の TCHAR マクロ（IDC_ARROW 等）を W 系にそろえる（Unicode アプリ既定）
      target_compile_definitions(jp_pii_sanitizer PRIVATE UNICODE _UNICODE)
      target_include_directories(jp_pii_sanitizer PRIVATE
        ${WV2_DIR}/include
        ${ORT_DIR}/include
        ${PDFIUM_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugixml/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}/pcre2
        ${SODIUM_INCLUDE})
      target_compile_definitions(jp_pii_sanitizer PRIVATE ${SODIUM_DEFS})
      target_link_libraries(jp_pii_sanitizer PRIVATE
        ${WV2_DIR}/x64/WebView2Loader.dll.lib
        ${ORT_LIB} ${TOKSHIM_LIB} ${SHIM_LIB} pcre2-8 ${PDFIUM_LIB} ${SODIUM_LIB} ${SYS_LIBS}
        version ole32 oleaut32 shlwapi shell32)
      copy_runtime_dll(jp_pii_sanitizer
        ${WV2_DIR}/x64/WebView2Loader.dll ${ORT_DLLS} ${PDFIUM_DLL})
      # UI（index.html）を exe の隣へ配置。実行時に load_ui_html() が読む。
      add_custom_command(TARGET jp_pii_sanitizer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/src/app/index.html $<TARGET_FILE_DIR:jp_pii_sanitizer>
        VERBATIM)
    endif()
  endif()
else()
  message(WARNING "pcre2 が無いため phase2_regex をスキップします（cpp/README.md 参照）")
endif()
