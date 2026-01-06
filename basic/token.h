//
//  token.h
//  basic
//
//  Created by Emídio Cunha on 28/12/2025.
//

#pragma once

enum class TokenKind {
    End,
    Number,
    String,
    Identifier,
    // operators
    Plus, Minus, Star, Slash, Backslash, Caret,
    LParen, RParen, Comma, Semicolon, Colon,
    Equal, Less, Greater,
    NotEqual, LessEqual, GreaterEqual,
    // keywords
    KW_PRINT, KW_LET, KW_INPUT, KW_IF, KW_THEN, KW_GOTO, KW_GOSUB, KW_RETURN,
    KW_FOR, KW_TO, KW_STEP, KW_NEXT, KW_END, KW_STOP,
    KW_REM, KW_DIM,
    KW_AND, KW_OR, KW_NOT,
    KW_MOD,
    KW_CLS,
    KW_LOCATE,
    KW_COLOR,
    KW_RANDOMIZE,
    KW_INTERVAL,
    KW_ON,
    KW_OFF,
    KW_DEFINT,
    KW_KEY,
    KW_TIME,
    KW_READ,
    KW_DATA,
    KW_RESTORE,
    // commands (immediate)
    KW_RUN, KW_LIST, KW_NEW, KW_CLEAR, KW_DELETE, KW_CONT, KW_SAVE, KW_LOAD
};

struct Token {
    TokenKind kind;
    std::string text;
    double number = 0.0;
};

static inline bool is_basic_keyword(TokenKind k) {
    switch (k) {
        case TokenKind::KW_PRINT: case TokenKind::KW_LET: case TokenKind::KW_INPUT:
        case TokenKind::KW_IF: case TokenKind::KW_THEN: case TokenKind::KW_GOTO:
        case TokenKind::KW_GOSUB: case TokenKind::KW_RETURN:
        case TokenKind::KW_FOR: case TokenKind::KW_TO: case TokenKind::KW_STEP:
        case TokenKind::KW_NEXT: case TokenKind::KW_END: case TokenKind::KW_STOP:
        case TokenKind::KW_REM: case TokenKind::KW_DIM:
        case TokenKind::KW_AND: case TokenKind::KW_OR: case TokenKind::KW_NOT:
        case TokenKind::KW_MOD: case TokenKind::KW_CLS: case TokenKind::KW_LOCATE:
        case TokenKind::KW_COLOR:
        case TokenKind::KW_INTERVAL:
        case TokenKind::KW_ON:
        case TokenKind::KW_OFF:
        case TokenKind::KW_KEY:
        case TokenKind::KW_RANDOMIZE:
        case TokenKind::KW_DEFINT:
        case TokenKind::KW_TIME:
        case TokenKind::KW_RUN: case TokenKind::KW_LIST: case TokenKind::KW_NEW:
        case TokenKind::KW_CLEAR: case TokenKind::KW_DELETE: case TokenKind::KW_CONT:
        case TokenKind::KW_SAVE: case TokenKind::KW_LOAD:
            return true;
        default:
            return false;
    }
}
