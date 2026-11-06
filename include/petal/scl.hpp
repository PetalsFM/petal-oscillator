//          .--.
//     .--.(    ).--.
//    (    )'--'(    )
//     '--'.--.'--'
//        (    )
//         '--'
//
// petal/scl.hpp - read a Scala .scl scale into a petal::Tuning.
//
// Outside the oscillator on purpose: the core takes plain ratios, and this is
// one way to get them. No locale is consulted, so "701.955" reads the same in
// every country. Allocation-free; parse on a non-audio thread anyway.
//
// Copyright (c) 2026 Petals. MIT License, see LICENSE.
#pragma once

#include "core.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace petal {

namespace scl_detail {

inline bool isSpace(char c) { return c == ' ' || c == '\t'; }
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Unsigned integer at s; returns chars read (0 = none).
inline int readUInt(const char* s, const char* e, long long& v) {
    int n = 0; v = 0;
    while (s + n < e && isDigit(s[n]) && n < 18) { v = v * 10 + (s[n] - '0'); ++n; }
    return n;
}

// A decimal with optional sign, fraction and exponent; returns chars read.
inline int readDecimal(const char* s, const char* e, double& out) {
    const char* p = s;
    bool neg = false;
    if (p < e && (*p == '+' || *p == '-')) { neg = (*p == '-'); ++p; }
    double v = 0.0; int digits = 0;
    while (p < e && isDigit(*p)) { v = v * 10.0 + (*p - '0'); ++p; ++digits; }
    if (p < e && *p == '.') {
        ++p;
        double scale = 0.1;
        while (p < e && isDigit(*p)) { v += (*p - '0') * scale; scale *= 0.1; ++p; ++digits; }
    }
    if (digits == 0) return 0;
    if (p < e && (*p == 'e' || *p == 'E')) {
        const char* q = p + 1;
        bool eneg = false;
        if (q < e && (*q == '+' || *q == '-')) { eneg = (*q == '-'); ++q; }
        long long ex; int n = readUInt(q, e, ex);
        if (n > 0) { v *= std::pow(10.0, eneg ? -double(ex) : double(ex)); p = q + n; }
    }
    out = neg ? -v : v;
    return static_cast<int>(p - s);
}

}  // namespace scl_detail

// Parse .scl text. Lines starting with '!' are comments; the first other line
// is the description, which may be empty, the next the note count, then one
// pitch per line: cents if it has a '.', else a ratio "p/q" or a whole number.
// The last pitch is the period. Returns false, and leaves `out` as it was, for
// a file that does not parse, more than kMaxDegrees notes, zero notes, or a
// period not above 1/1.
inline bool parseScl(const char* buf, std::size_t len, Tuning& out) {
    using namespace scl_detail;
    if (!buf || len == 0) return false;
    // A UTF-8 byte order mark, which some editors write, is skipped.
    if (len >= 3 && static_cast<unsigned char>(buf[0]) == 0xEF && static_cast<unsigned char>(buf[1]) == 0xBB
                 && static_cast<unsigned char>(buf[2]) == 0xBF) {
        buf += 3; len -= 3;
    }
    const char* p = buf;
    const char* end = buf + len;
    // The file's own line ending, read off its first line break: LF, CR LF,
    // CR CR LF (what converting a CR LF file a second time leaves), LF CR, or
    // CR alone. Each line then ends at exactly one of it, so an empty line is
    // still a line and an empty description is never skipped. A break that is
    // not the file's own ending is read as one LF, CR or CR LF.
    const char* eol = p;
    while (eol < end && *eol != '\n' && *eol != '\r') ++eol;
    std::size_t eolLen = 0;
    if (eol < end && *eol == '\n') {
        eolLen = (eol + 1 < end && eol[1] == '\r') ? 2 : 1;
    } else if (eol < end) {
        const char* q = eol;
        while (q < end && *q == '\r' && q - eol < 2) ++q;
        eolLen = (q < end && *q == '\n') ? static_cast<std::size_t>(q - eol) + 1 : 1;
    }
    int stage = 0, declared = -1, got = 0;
    double pitches[kMaxDegrees];
    while (p < end) {
        const char* le = p;
        while (le < end && *le != '\n' && *le != '\r') ++le;
        if (*p != '!') {
            const char* ls = p;
            while (ls < le && isSpace(*ls)) ++ls;
            const char* te = le;
            while (te > ls && isSpace(te[-1])) --te;
            if (stage == 0) {
                stage = 1;   // description, may be empty
            } else if (ls < te) {
                if (stage == 1) {
                    long long cnt; int n = readUInt(ls, te, cnt);
                    if (n == 0 || cnt > 1024) return false;
                    declared = static_cast<int>(cnt);
                    stage = 2;
                } else if (got < declared) {
                    const char* tok = ls;
                    const char* tokEnd = ls;
                    while (tokEnd < te && !isSpace(*tokEnd)) ++tokEnd;
                    bool hasDot = false, hasSlash = false;
                    for (const char* c = tok; c < tokEnd; ++c) { hasDot |= (*c == '.'); hasSlash |= (*c == '/'); }
                    double ratio = 0.0;
                    if (hasDot) {
                        double cents;
                        if (readDecimal(tok, tokEnd, cents) == 0) return false;
                        ratio = std::pow(2.0, cents / 1200.0);
                    } else {
                        long long num, den = 1;
                        int n = readUInt(tok, tokEnd, num);
                        if (n == 0 || num <= 0) return false;
                        // "3 / 2" is a ratio typed with spaces, not 3 and a
                        // comment: refuse it rather than load 3/1.
                        const char* after = tokEnd;
                        while (after < te && isSpace(*after)) ++after;
                        if (!hasSlash && after < te && *after == '/') return false;
                        if (hasSlash) {
                            const char* d = tok + n;
                            if (d >= tokEnd || *d != '/') return false;
                            int m = readUInt(d + 1, tokEnd, den);
                            if (m == 0 || den <= 0) return false;
                        }
                        ratio = static_cast<double>(num) / static_cast<double>(den);
                    }
                    if (!(ratio > 0.0) || !std::isfinite(ratio)) return false;
                    if (got >= kMaxDegrees) return false;
                    pitches[got++] = ratio;
                }
            }
        }
        p = le;
        if (eolLen > 0 && static_cast<std::size_t>(end - p) >= eolLen && std::memcmp(p, eol, eolLen) == 0) {
            p += eolLen;
        } else {
            if (p < end && *p == '\r') ++p;
            if (p < end && *p == '\n') ++p;
        }
    }
    if (declared < 1 || declared > kMaxDegrees || got < declared) return false;
    Tuning t;
    if (!t.setFromIntervals(pitches, declared)) return false;
    out = t;
    return true;
}

// The largest .scl this reads. A 64-note scale with comments is under 4 KB;
// the buffer lives on the stack, so it is kept small enough for a thread
// with a modest one.
constexpr std::size_t kMaxSclBytes = 16384;

// Read a .scl file from disk (not for the audio thread). A file larger than
// kMaxSclBytes is refused rather than read in part.
inline bool loadScl(const char* path, Tuning& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char buf[kMaxSclBytes];
    const std::size_t n = std::fread(buf, 1, sizeof buf, f);
    const bool whole = std::feof(f) != 0;
    std::fclose(f);
    return whole && parseScl(buf, n, out);
}

}  // namespace petal
