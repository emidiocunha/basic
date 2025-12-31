//
//  lexer.h
//  basic
//
//  Created by Emídio Cunha on 28/12/2025.
//
#pragma once

#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include "env.h"
#include "token.h"

using std::string;
using std::vector;

struct Lexer {
    std::string s;
    size_t i = 0;
    size_t tokenStart = 0;
    size_t tokenEnd = 0;

    explicit Lexer(std::string src) : s(std::move(src)), i(0) {}

    void skipSpace() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    }

    bool match(const std::string& kw) {
        size_t j = i;
        for (char c : kw) {
            if (j >= s.size() || std::tolower(static_cast<unsigned char>(s[j])) != std::tolower(static_cast<unsigned char>(c)))
                return false;
            ++j;
        }
        if (j < s.size() && std::isalnum(static_cast<unsigned char>(s[j]))) return false;
        i = j;
        return true;
    }

    Token next() {
        skipSpace();
        tokenStart = i;
        auto makeTok = [&](TokenKind k, const std::string& txt, double num)->Token {
            tokenEnd = i;
            return Token{k, txt, num};
        };
        if (i >= s.size()) return makeTok(TokenKind::End, "", 0.0);

        char c = s[i];

        // String literal
        if (c == '\"') {
            ++i;
            std::string out;
            while (i < s.size()) {
                char d = s[i++];
                if (d == '\"') {
                    if (i < s.size() && s[i] == '\"') { // doubled quote
                        out.push_back('\"');
                        ++i;
                        continue;
                    }
                    break;
                }
                out.push_back(d);
            }
            return makeTok(TokenKind::String, out, 0.0);
        }

        // Number (integer or float)
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i+1])))) {
            size_t start = i;
            bool seenDot = false;
            if (s[i] == '.') { seenDot = true; ++i; }
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            if (!seenDot && i < s.size() && s[i] == '.') {
                seenDot = true; ++i;
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            }
            // exponent
            if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
                size_t j = i + 1;
                if (j < s.size() && (s[j] == '+' || s[j] == '-')) ++j;
                bool any = false;
                while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) { any = true; ++j; }
                if (any) i = j;
            }
            double val = std::strtod(s.substr(start, i - start).c_str(), nullptr);
            return makeTok(TokenKind::Number, "", val);
        }

        // Identifier or keyword
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t start = i++;
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' || s[i] == '$')) ++i;
            std::string ident = s.substr(start, i - start);
            std::string upper;
            upper.reserve(ident.size());
            for (char ch : ident) upper.push_back(std::toupper(static_cast<unsigned char>(ch)));

            auto kw = [&](const char* k, TokenKind kind)->std::optional<Token> {
                if (upper == k) return Token{kind, ident, 0.0};
                return std::nullopt;
            };

            if (auto t = kw("PRINT", TokenKind::KW_PRINT)) { tokenEnd = i; return *t; }
            if (auto t = kw("LET", TokenKind::KW_LET)) { tokenEnd = i; return *t; }
            if (auto t = kw("INPUT", TokenKind::KW_INPUT)) { tokenEnd = i; return *t; }
            if (auto t = kw("IF", TokenKind::KW_IF)) { tokenEnd = i; return *t; }
            if (auto t = kw("THEN", TokenKind::KW_THEN)) { tokenEnd = i; return *t; }
            if (auto t = kw("GOTO", TokenKind::KW_GOTO)) { tokenEnd = i; return *t; }
            if (auto t = kw("GOSUB", TokenKind::KW_GOSUB)) { tokenEnd = i; return *t; }
            if (auto t = kw("RETURN", TokenKind::KW_RETURN)) { tokenEnd = i; return *t; }
            if (auto t = kw("FOR", TokenKind::KW_FOR)) { tokenEnd = i; return *t; }
            if (auto t = kw("TO", TokenKind::KW_TO)) { tokenEnd = i; return *t; }
            if (auto t = kw("STEP", TokenKind::KW_STEP)) { tokenEnd = i; return *t; }
            if (auto t = kw("NEXT", TokenKind::KW_NEXT)) { tokenEnd = i; return *t; }
            if (auto t = kw("END", TokenKind::KW_END)) { tokenEnd = i; return *t; }
            if (auto t = kw("STOP", TokenKind::KW_STOP)) { tokenEnd = i; return *t; }
            if (auto t = kw("REM", TokenKind::KW_REM)) { tokenEnd = i; return *t; }
            if (auto t = kw("DIM", TokenKind::KW_DIM)) { tokenEnd = i; return *t; }
            if (auto t = kw("AND", TokenKind::KW_AND)) { tokenEnd = i; return *t; }
            if (auto t = kw("OR", TokenKind::KW_OR)) { tokenEnd = i; return *t; }
            if (auto t = kw("NOT", TokenKind::KW_NOT)) { tokenEnd = i; return *t; }
            if (auto t = kw("MOD", TokenKind::KW_MOD)) { tokenEnd = i; return *t; }
            if (auto t = kw("CLS", TokenKind::KW_CLS)) { tokenEnd = i; return *t; }
            if (auto t = kw("LOCATE", TokenKind::KW_LOCATE)) { tokenEnd = i; return *t; }
            if (auto t = kw("RANDOMIZE", TokenKind::KW_RANDOMIZE)) { tokenEnd = i; return *t; }

            if (auto t = kw("RUN", TokenKind::KW_RUN)) { tokenEnd = i; return *t; }
            if (auto t = kw("LIST", TokenKind::KW_LIST)) { tokenEnd = i; return *t; }
            if (auto t = kw("NEW", TokenKind::KW_NEW)) { tokenEnd = i; return *t; }
            if (auto t = kw("CLEAR", TokenKind::KW_CLEAR)) { tokenEnd = i; return *t; }
            if (auto t = kw("DELETE", TokenKind::KW_DELETE)) { tokenEnd = i; return *t; }
            if (auto t = kw("CONT", TokenKind::KW_CONT)) { tokenEnd = i; return *t; }
            if (auto t = kw("SAVE", TokenKind::KW_SAVE)) { tokenEnd = i; return *t; }
            if (auto t = kw("LOAD", TokenKind::KW_LOAD)) { tokenEnd = i; return *t; }

            return makeTok(TokenKind::Identifier, ident, 0.0);
        }

        // Two-char relational operators
        if (c == '<' && i + 1 < s.size() && s[i+1] == '>') { i += 2; return makeTok(TokenKind::NotEqual, "<>", 0.0); }
        if (c == '<' && i + 1 < s.size() && s[i+1] == '=') { i += 2; return makeTok(TokenKind::LessEqual, "<=", 0.0); }
        if (c == '>' && i + 1 < s.size() && s[i+1] == '=') { i += 2; return makeTok(TokenKind::GreaterEqual, ">=", 0.0); }

        // Single char tokens
        ++i;
        switch (c) {
            case '+': return makeTok(TokenKind::Plus, "+", 0.0);
            case '-': return makeTok(TokenKind::Minus, "-", 0.0);
            case '*': return makeTok(TokenKind::Star, "*", 0.0);
            case '/': return makeTok(TokenKind::Slash, "/", 0.0);
            case '\\': return makeTok(TokenKind::Backslash, "\\", 0.0);
            case '^': return makeTok(TokenKind::Caret, "^", 0.0);
            case '(': return makeTok(TokenKind::LParen, "(", 0.0);
            case ')': return makeTok(TokenKind::RParen, ")", 0.0);
            case ',': return makeTok(TokenKind::Comma, ",", 0.0);
            case ';': return makeTok(TokenKind::Semicolon, ";", 0.0);
            case ':': return makeTok(TokenKind::Colon, ":", 0.0);
            case '=': return makeTok(TokenKind::Equal, "=", 0.0);
            case '<': return makeTok(TokenKind::Less, "<", 0.0);
            case '>': return makeTok(TokenKind::Greater, ">", 0.0);
            case '%': return makeTok(TokenKind::KW_MOD, "%", 0.0);
        }

        throw ParseError(std::string("Unexpected character: ") + c);
    }
};
