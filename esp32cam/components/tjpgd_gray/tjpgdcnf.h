/* TJpgDec configuration for the grayscale path.
 *
 * JD_FORMAT 2 emits the Y component only: no chroma decode, no RGB conversion,
 * which is exactly what the viewer needs. Symbols are renamed so the ESP32 ROM
 * tjpgd is never mixed with this build. */
#pragma once

#define JD_SZBUF           512
#define JD_FORMAT          2   /* 0: RGB888, 1: RGB565, 2: grayscale */
#define JD_USE_SCALE       1
#define JD_TBLCLIP         1
#define JD_FASTDECODE      1   /* 32-bit barrel shifter, ~3.1 kB pool */
#define JD_DEFAULT_HUFFMAN 0

#define jd_prepare esp_tjpgd_gray_prepare
#define jd_decomp  esp_tjpgd_gray_decomp
