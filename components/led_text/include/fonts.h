#ifndef FONTS_H
#define FONTS_H

// First byte in font array that's actual character data (zero-indexed)
#define FONT_DATA_OFFSET 3

// ASCII code for first character supported by font (32 corresponds to a space)
// #define FONT_START_CHAR 32

extern const unsigned char fonts_4x6[];

#endif
