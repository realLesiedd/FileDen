cmake_minimum_required(VERSION 3.17)
project(fileden C)
# the payload: web UI + file API on port 9923 (the full screen app is built by app/fself/build.sh)

# miniz (MIT): ZIP reading and writing
add_library(miniz STATIC third_party/miniz/miniz.c)
target_compile_options(miniz PRIVATE -w -O2)

# 7-Zip's C code (LZMA SDK, public domain): 7z reading, LZMA2 writing, SHA-256, CRC32
set(LZ ${CMAKE_SOURCE_DIR}/third_party/lzma)
add_library(lzma STATIC
  ${LZ}/7zArcIn.c ${LZ}/7zBuf.c ${LZ}/7zCrc.c ${LZ}/7zCrcOpt.c ${LZ}/7zDec.c ${LZ}/7zStream.c ${LZ}/Alloc.c
  ${LZ}/Bcj2.c ${LZ}/Bra.c ${LZ}/Bra86.c ${LZ}/BraIA64.c ${LZ}/CpuArch.c ${LZ}/Delta.c ${LZ}/Lzma2Dec.c
  ${LZ}/LzmaDec.c ${LZ}/Lzma2Enc.c ${LZ}/LzmaEnc.c ${LZ}/LzFind.c ${LZ}/LzFindOpt.c ${LZ}/Ppmd7.c ${LZ}/Ppmd7Dec.c
  ${LZ}/Sha256.c ${LZ}/Sha256Opt.c)
target_include_directories(lzma PUBLIC ${LZ})
target_compile_definitions(lzma PUBLIC Z7_ST Z7_PPMD_SUPPORT)
target_compile_options(lzma PRIVATE -w -O2)

# jxrlib (BSD-2-Clause): PS5 HDR screenshots (.jxr) for the image viewer
set(JXR ${CMAKE_SOURCE_DIR}/third_party/jxrlib)
file(GLOB JXR_SRC ${JXR}/image/sys/*.c ${JXR}/image/decode/*.c ${JXR}/image/encode/*.c ${JXR}/jxrgluelib/*.c)
add_library(jxr STATIC ${JXR_SRC})
target_include_directories(jxr PUBLIC ${JXR}/common/include ${JXR}/image/sys ${JXR}/jxrgluelib)
target_compile_definitions(jxr PUBLIC __ANSI__ DISABLE_PERF_MEASUREMENT)
target_compile_options(jxr PRIVATE -w -Os -include ${CMAKE_SOURCE_DIR}/third_party/jxrlib_compat.h)

# zlib, bzip2, xz, zstd, OpenSSL's libcrypto and libwebp: the ps5-payload-dev PacBrew ports
# (tools/fetch-deps.sh puts them in .deps/pacbrew)
set(PACBREW ${CMAKE_SOURCE_DIR}/.deps/pacbrew/opt/ps5-payload-sdk/target/user/homebrew CACHE PATH "PacBrew ports prefix")
if(NOT EXISTS ${PACBREW}/lib/libcrypto.a)
  message(FATAL_ERROR "PacBrew ports not found in ${PACBREW}: run tools/fetch-deps.sh (or set -DPACBREW=...)")
endif()

# libarchive 3.7.4 (BSD-2-Clause), vendored and built here with OpenSSL for ZIP passwords
foreach(o ENABLE_TEST ENABLE_TAR ENABLE_CPIO ENABLE_CAT ENABLE_UNZIP ENABLE_ACL ENABLE_XATTR ENABLE_ICONV ENABLE_LIBXML2
    ENABLE_EXPAT ENABLE_LZ4 ENABLE_LZO ENABLE_LIBB2 ENABLE_PCREPOSIX ENABLE_PCRE2POSIX ENABLE_CNG ENABLE_MBEDTLS
    ENABLE_NETTLE ENABLE_INSTALL ENABLE_WERROR BUILD_SHARED_LIBS)
  set(${o} OFF CACHE BOOL "" FORCE)
endforeach()
set(ENABLE_OPENSSL ON CACHE BOOL "" FORCE)
set(OPENSSL_ROOT_DIR ${PACBREW})
set(OPENSSL_INCLUDE_DIR ${PACBREW}/include)
set(OPENSSL_CRYPTO_LIBRARY ${PACBREW}/lib/libcrypto.a)
set(OPENSSL_SSL_LIBRARY ${PACBREW}/lib/libssl.a)
set(OPENSSL_USE_STATIC_LIBS ON)
set(ZLIB_INCLUDE_DIR ${PACBREW}/include)
set(ZLIB_LIBRARY ${PACBREW}/lib/libz.a)
set(BZIP2_INCLUDE_DIR ${PACBREW}/include)
set(BZIP2_LIBRARIES ${PACBREW}/lib/libbz2.a)
set(LIBLZMA_INCLUDE_DIR ${PACBREW}/include)
set(LIBLZMA_LIBRARY ${PACBREW}/lib/liblzma.a)
set(ZSTD_INCLUDE_DIR ${PACBREW}/include)
set(ZSTD_LIBRARY ${PACBREW}/lib/libzstd.a)
add_subdirectory(third_party/libarchive EXCLUDE_FROM_ALL)
set(PB_LIBS ${PACBREW}/lib/liblzma.a ${PACBREW}/lib/libzstd.a ${PACBREW}/lib/libbz2.a ${PACBREW}/lib/libz.a
  ${PACBREW}/lib/libcrypto.a ${PACBREW}/lib/libwebp.a ${PACBREW}/lib/libsharpyuv.a)

add_executable(fileden main.c fsops.c archive.c rarcrypt.c title.c image.c jxrdec.c)
target_compile_options(fileden PRIVATE -Wall -Werror -O2)
set_source_files_properties(jxrdec.c image.c PROPERTIES COMPILE_OPTIONS "-Wno-error")
target_include_directories(fileden PRIVATE ${CMAKE_SOURCE_DIR}/third_party/libarchive/libarchive ${PACBREW}/include)
target_compile_definitions(fileden PRIVATE LIBARCHIVE_STATIC FD_WEBP FD_JXR)
target_link_libraries(fileden PRIVATE miniz lzma jxr archive_static ${PB_LIBS} kernel_sys pthread)
set_target_properties(fileden PROPERTIES OUTPUT_NAME fileden.elf)
# the libraries' debug info would triple the payload's size
add_custom_command(TARGET fileden POST_BUILD COMMAND ${CMAKE_STRIP} --strip-debug $<TARGET_FILE:fileden>)
