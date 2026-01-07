//
//  parser.h
//  basic
//
//  Created by Emídio Cunha on 28/12/2025.
//
#pragma once

#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <map>
#include <unordered_map>
#include <vector>
#include <variant>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <functional>
#include <limits>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include "token.h"
#include "env.h"
#include "editor.h"
#include "string.h"
#include "lexer.h"

using std::string;
using std::vector;

struct Parser {
    Lexer lex;
    Token tok;

    Env& env;
    std::string currentLine; // full current line text (without line number)
    size_t linePosBase = 0;  // used to compute posInLine

    explicit Parser(std::string src, Env& e) : lex(std::move(src)), env(e) {
        tok = lex.next();
    }

    void consume(TokenKind k, const char* what) {
        if (tok.kind != k) throw ParseError(std::string("Expected ") + what);
        tok = lex.next();
    }

    bool accept(TokenKind k) {
        if (tok.kind == k) { tok = lex.next(); return true; }
        return false;
    }

    // Expression parsing (Pratt)
    int precedence(TokenKind k) {
        switch (k) {
            case TokenKind::KW_OR: return 1;
            case TokenKind::KW_AND: return 2;
            case TokenKind::Equal:
            case TokenKind::NotEqual:
            case TokenKind::Less:
            case TokenKind::LessEqual:
            case TokenKind::Greater:
            case TokenKind::GreaterEqual: return 3;
            case TokenKind::Plus:
            case TokenKind::Minus: return 4;
            case TokenKind::Star:
            case TokenKind::Slash:
            case TokenKind::Backslash:
            case TokenKind::KW_MOD: return 5;
            case TokenKind::Caret: return 6; // right-associative
            default: return 0;
        }
    }

    static std::string upperName(const std::string& s) {
        std::string u; u.reserve(s.size());
        for (char c : s) u.push_back(std::toupper(static_cast<unsigned char>(c)));
        return u;
    }

    static bool isFunction(const std::string& upper) {
        static const std::unordered_map<std::string, bool> fn = {
            {"SIN",true},{"COS",true},{"TAN",true},{"ATN",true},{"LOG",true},{"EXP",true},{"SQR",true},{"ABS",true},{"INT",true},{"SGN",true},
            {"RND",true},{"TIME",true},{"VAL",true},{"STR$",true},{"LEN",true},{"LEFT$",true},{"RIGHT$",true},{"MID$",true},{"CHR$",true},{"ASC",true},{"TAB",true}
        };
        return fn.find(upper) != fn.end();
    }

    std::vector<Value> parseArgList() {
        std::vector<Value> args;
        consume(TokenKind::LParen, "'('");
        if (tok.kind != TokenKind::RParen) {
            while (true) {
                args.push_back(parseExpression());
                if (accept(TokenKind::Comma)) continue;
                break;
            }
        }
        consume(TokenKind::RParen, "')'");
        return args;
    }

    Value callFunction(const std::string& upper, std::vector<Value> args) {
        auto argN = [&](size_t i)->double {
            if (i >= args.size()) return 0.0;
            return args[i].asNumber();
        };
        auto argS = [&](size_t i)->std::string {
            if (i >= args.size()) return std::string("");
            return args[i].asString();
        };

        if (upper == "SIN") return Value(std::sin(argN(0)));
        if (upper == "COS") return Value(std::cos(argN(0)));
        if (upper == "TAN") return Value(std::tan(argN(0)));
        if (upper == "ATN") return Value(std::atan(argN(0)));
        if (upper == "LOG") return Value(std::log(argN(0)));
        if (upper == "EXP") return Value(std::exp(argN(0)));
        if (upper == "SQR") return Value(std::sqrt(argN(0)));
        if (upper == "ABS") return Value(std::fabs(argN(0)));
        if (upper == "INT") return Value(std::floor(argN(0)));
        if (upper == "SGN") {
            double x = argN(0);
            int16_t r = (x > 0) ? static_cast<int16_t>(1) : ((x < 0) ? static_cast<int16_t>(-1) : static_cast<int16_t>(0));
            return Value(r);
        }
        if (upper == "RND") {
            // GW-BASIC-ish behavior:
            //   RND()      -> next random number
            //   RND(x>0)   -> next random number (does NOT reseed)
            //   RND(0)     -> repeat last random number (or generate if none)
            //   RND(x<0)   -> reseed using abs(x) and return next random number

            double x = args.empty() ? 1.0 : argN(0);

            if (x == 0.0) {
                if (!env.hasLastRnd) {
                    double r = static_cast<double>(std::rand()) / (static_cast<double>(RAND_MAX) + 1.0);
                    env.lastRnd = r;
                    env.hasLastRnd = true;
                }
                return Value(env.lastRnd);
            }

            if (x < 0.0) {
                long long sx = static_cast<long long>(x);
                unsigned seed = static_cast<unsigned>(std::llabs(sx));
                std::srand(seed);
                env.hasLastRnd = false;
            }

            // Generate next value
            double r = static_cast<double>(std::rand()) / (static_cast<double>(RAND_MAX) + 1.0);
            env.lastRnd = r;
            env.hasLastRnd = true;
            return Value(r);
        }

        if (upper == "TIME") {
            // TIME(): return current time in seconds since midnight (local time).
            // This is a numeric analogue to TIME$ in classic BASIC dialects.
            std::time_t t = std::time(nullptr);
            std::tm lt{};
#if defined(_WIN32)
            localtime_s(&lt, &t);
#else
            std::tm* p = std::localtime(&t);
            if (p) lt = *p;
#endif
            double secs = static_cast<double>(lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);
            return Value(secs);
        }
        if (upper == "VAL") return Value(Value(argS(0)).asNumber());
        if (upper == "STR$") return Value(Value(argN(0)).asString());
        if (upper == "LEN") return Value(static_cast<double>(argS(0).size()));
        if (upper == "LEFT$") {
            auto s = argS(0);
            int n = static_cast<int>(argN(1));
            if (n < 0) n = 0;
            if (static_cast<size_t>(n) > s.size()) n = static_cast<int>(s.size());
            return Value(s.substr(0, static_cast<size_t>(n)));
        }
        if (upper == "RIGHT$") {
            auto s = argS(0);
            int n = static_cast<int>(argN(1));
            if (n < 0) n = 0;
            if (static_cast<size_t>(n) > s.size()) n = static_cast<int>(s.size());
            return Value(s.substr(s.size() - static_cast<size_t>(n)));
        }
        if (upper == "MID$") {
            auto s = argS(0);
            int start = static_cast<int>(argN(1));
            int len = (args.size() >= 3) ? static_cast<int>(argN(2)) : static_cast<int>(s.size());
            if (start < 1) start = 1; // BASIC is 1-based
            size_t idx = static_cast<size_t>(start - 1);
            if (idx >= s.size()) return Value(std::string(""));
            if (len < 0) len = 0;
            size_t n = std::min(static_cast<size_t>(len), s.size() - idx);
            return Value(s.substr(idx, n));
        }
        if (upper == "CHR$") return Value(std::string(1, static_cast<char>(static_cast<int>(argN(0)) & 0xFF)));
        if (upper == "ASC") {
            auto s = argS(0);
            if (s.empty()) return Value(0.0);
            return Value(static_cast<double>(static_cast<unsigned char>(s[0])));
        }

        // TAB(n): move cursor to 1-based column n; return "" so PRINT doesn't output 0.
        if (upper == "TAB") {
            int col = static_cast<int>(argN(0));
            // side-effect print spaces (handled by helper below)
            // return empty string
            // NOTE: this is intentionally a side-effecting function for PRINT usage.
            // (See helper basic_print_tab_to_column1)
            // We call helper in-place:
            extern void __basic_tab_side_effect(Env&, int); // forward shim (defined below)
            __basic_tab_side_effect(env, col);
            return Value(std::string(""));
        }

        throw RuntimeError("Unknown function");
    }

    Value applyOp(const Value& a, TokenKind op, const Value& b) {
        auto cmp = [&](double lhs, double rhs)->Value {
            switch (op) {
                case TokenKind::Equal: return Value::fromBool(lhs == rhs);
                case TokenKind::NotEqual: return Value::fromBool(lhs != rhs);
                case TokenKind::Less: return Value::fromBool(lhs < rhs);
                case TokenKind::LessEqual: return Value::fromBool(lhs <= rhs);
                case TokenKind::Greater: return Value::fromBool(lhs > rhs);
                case TokenKind::GreaterEqual: return Value::fromBool(lhs >= rhs);
                default: return Value(0.0);
            }
        };

        if (op == TokenKind::Plus) {
            if (a.isString() || b.isString()) return Value(a.asString() + b.asString());
            if (a.isInt() && b.isInt()) {
                int32_t r = static_cast<int32_t>(a.asInt()) + static_cast<int32_t>(b.asInt());
                return Value(Value::toInt16Checked(static_cast<double>(r)));
            }
            return Value(a.asNumber() + b.asNumber());
        }
        if (op == TokenKind::Minus) {
            if (a.isInt() && b.isInt()) {
                int32_t r = static_cast<int32_t>(a.asInt()) - static_cast<int32_t>(b.asInt());
                return Value(Value::toInt16Checked(static_cast<double>(r)));
            }
            return Value(a.asNumber() - b.asNumber());
        }
        if (op == TokenKind::Star) {
            if (a.isInt() && b.isInt()) {
                int32_t r = static_cast<int32_t>(a.asInt()) * static_cast<int32_t>(b.asInt());
                return Value(Value::toInt16Checked(static_cast<double>(r)));
            }
            return Value(a.asNumber() * b.asNumber());
        }
        if (op == TokenKind::Slash) {
            return Value(a.asNumber() / b.asNumber());
        }
        if (op == TokenKind::Backslash) {
            double denomN = b.asNumber();
            if (denomN == 0.0) throw RuntimeError("Division by zero");

            // GW-BASIC integer division: truncate toward zero, result is integer.
            if (a.isInt() && b.isInt()) {
                int16_t av = a.asInt();
                int16_t bv = b.asInt();
                // Special overflow case: -32768 \ -1
                if (av == static_cast<int16_t>(-32768) && bv == static_cast<int16_t>(-1)) {
                    throw RuntimeError("Overflow");
                }
                int16_t q = static_cast<int16_t>(av / bv);
                return Value(q);
            }

            double q = a.asNumber() / denomN;
            double tq = std::trunc(q);
            return Value(Value::toInt16Checked(tq));
        }
        if (op == TokenKind::Caret) return Value(std::pow(a.asNumber(), b.asNumber()));
        if (op == TokenKind::KW_MOD) {
            double denomN = b.asNumber();
            if (denomN == 0.0) throw RuntimeError("Division by zero");

            if (a.isInt() && b.isInt()) {
                int16_t av = a.asInt();
                int16_t bv = b.asInt();
                int16_t r = static_cast<int16_t>(av % bv);
                return Value(r);
            }

            double x = a.asNumber();
            double y = denomN;
            return Value(std::fmod(x, y));
        }
        if (op == TokenKind::KW_AND) return Value::fromBool((a.asNumber() != 0.0) && (b.asNumber() != 0.0));
        if (op == TokenKind::KW_OR)  return Value::fromBool((a.asNumber() != 0.0) || (b.asNumber() != 0.0));

        if (op == TokenKind::Equal || op == TokenKind::NotEqual || op == TokenKind::Less || op == TokenKind::LessEqual || op == TokenKind::Greater || op == TokenKind::GreaterEqual) {
            if (a.isString() && b.isString()) {
                const auto& sa = a.asString();
                const auto& sb = b.asString();
                int rel = (sa < sb) ? -1 : ((sa > sb) ? 1 : 0);
                switch (op) {
                    case TokenKind::Equal: return Value::fromBool(rel == 0);
                    case TokenKind::NotEqual: return Value::fromBool(rel != 0);
                    case TokenKind::Less: return Value::fromBool(rel < 0);
                    case TokenKind::LessEqual: return Value::fromBool(rel <= 0);
                    case TokenKind::Greater: return Value::fromBool(rel > 0);
                    case TokenKind::GreaterEqual: return Value::fromBool(rel >= 0);
                    default: break;
                }
            }
            return cmp(a.asNumber(), b.asNumber());
        }

        throw ParseError("Unknown operator");
    }

    Value parsePrimary() {
        if (tok.kind == TokenKind::Number) {
            double v = tok.number; tok = lex.next(); return Value(v);
        }
        if (tok.kind == TokenKind::String) {
            std::string s = tok.text; tok = lex.next(); return Value(s);
        }
        if (tok.kind == TokenKind::Identifier) {
            std::string name = tok.text;
            std::string upper = upperName(name);
            tok = lex.next();

            // Function calls: NAME(args)
            if (tok.kind == TokenKind::LParen && isFunction(upper)) {
                auto args = parseArgList();
                return callFunction(upper, std::move(args));
            }

            // Allow TIME without parentheses (TIME == TIME())
            if (upper == "TIME") {
                return callFunction(upper, {});
            }

            if (tok.kind == TokenKind::LParen) {
                auto args = parseArgList();
                if (args.size() != 1) throw RuntimeError("Bad subscript");
                int idx = static_cast<int>(args[0].asNumber());
                return env.getArrayElem(name, idx);
            }

            return env.getVar(name);
        }
        if (tok.kind == TokenKind::LParen) {
            tok = lex.next();
            Value v = parseExpression();
            consume(TokenKind::RParen, "')'");
            return v;
        }
        if (tok.kind == TokenKind::Minus) {
            tok = lex.next();
            Value v = parsePrimary();
            if (v.isInt()) {
                int16_t iv = v.asInt();
                if (iv == static_cast<int16_t>(-32768)) throw RuntimeError("Overflow");
                return Value(static_cast<int16_t>(-iv));
            }
            return Value(-v.asNumber());
        }
        if (tok.kind == TokenKind::KW_NOT) {
            tok = lex.next();
            Value v = parsePrimary();
            return Value::fromBool(!(v.asNumber() != 0.0));
        }
        throw ParseError("Expected expression");
    }

    Value parseBinOpRHS(int exprPrec, Value lhs) {
        while (true) {
            int tokPrec = precedence(tok.kind);
            bool rightAssoc = (tok.kind == TokenKind::Caret);
            if (tokPrec < exprPrec) return lhs;

            TokenKind op = tok.kind;
            tok = lex.next();

            Value rhs = parsePrimary();

            int nextPrec = precedence(tok.kind);
            if (tokPrec < nextPrec || (tokPrec == nextPrec && rightAssoc)) {
                rhs = parseBinOpRHS(tokPrec + (rightAssoc ? 0 : 1), rhs);
            }

            lhs = applyOp(lhs, op, rhs);
        }
    }

    Value parseExpression() {
        Value lhs = parsePrimary();
        return parseBinOpRHS(1, lhs);
    }

    // Statement parsing/execution
    void parseAndExecLine();

    // --- statements ---
    void execOneStatement();
    void exec_PRINT();
    void exec_LET_or_ASSIGN();
    void exec_INPUT();
    void exec_IF();
    void exec_GOTO(bool isGosub);
    void exec_RETURN();
    void exec_FOR();
    void exec_NEXT();
    void exec_DIM();
    void exec_COLOR();
    void exec_LOCATE();
    void exec_RANDOMIZE();
    void exec_ON();
    void exec_INTERVAL_CTRL();
    void exec_DEFINT();
    void exec_CLEAR();
    void exec_KEY_CTRL();
    void exec_DATA();
    void exec_READ();
    void exec_RESTORE();
    void jumpToLine(int target);

    void markLineProgress() {
        env.posInLine = linePosBase + lex.i;
    }
    void markStatementStart() {
        // lexer tracks the start index of the current token; for interrupts we want to
        // resume *at* the statement keyword, not after it.
        env.posInLine = linePosBase + lex.tokenStart;
    }

    // Timer safe-point: fire interval interrupt if needed
    void maybeFireIntervalInterrupt() {
        if (!env.intervalArmed || !env.intervalEnabled) return;
        if (env.intervalSeconds <= 0.0) return;
        if (env.inIntervalISR) return;

        auto now = std::chrono::steady_clock::now();
        if (now < env.nextIntervalFire) return;

        // Schedule next fire (avoid drift by stepping forward in multiples)
        auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(env.intervalSeconds)
        );
        if (period.count() <= 0) return;

        do {
            env.nextIntervalFire += period;
        } while (env.nextIntervalFire <= now);

        // Perform a GOSUB-like jump to the handler line.
        // Save the *statement start* so we re-execute the interrupted statement on RETURN.
        markStatementStart();
        env.gosubStack.push_back({env.pc, env.posInLine});
        env.inIntervalISR = true;
        jumpToLine(env.intervalGosubLine);
    }
};


// -------------------- ANSI COLOR helpers --------------------

static inline int basic_ansi_fg_code(int c) {
    // Map GW-BASIC COLOR codes 0..15 to ANSI SGR foreground codes.
    // 0-7: normal (30-37), 8-15: bright (90-97)
    static const int map[16] = {
        30, // 0 Black
        34, // 1 Blue
        32, // 2 Green
        36, // 3 Cyan
        31, // 4 Red
        35, // 5 Magenta
        33, // 6 Brown (dark yellow)
        37, // 7 White
        90, // 8 Gray (bright black)
        94, // 9 Light Blue
        92, // 10 Light Green
        96, // 11 Light Cyan
        91, // 12 Light Red
        95, // 13 Light Magenta
        93, // 14 Yellow (bright yellow)
        97  // 15 Bright White
    };
    if (c < 0) c = 0;
    if (c > 15) c = 15;
    return map[c];
}

static inline int basic_ansi_bg_code(int c) {
    // Map GW-BASIC COLOR codes 0..15 to ANSI SGR background codes.
    // 0-7: normal (40-47), 8-15: bright (100-107)
    static const int map[16] = {
        40,  // 0 Black
        44,  // 1 Blue
        42,  // 2 Green
        46,  // 3 Cyan
        41,  // 4 Red
        45,  // 5 Magenta
        43,  // 6 Brown (dark yellow)
        47,  // 7 White
        100, // 8 Gray
        104, // 9 Light Blue
        102, // 10 Light Green
        106, // 11 Light Cyan
        101, // 12 Light Red
        105, // 13 Light Magenta
        103, // 14 Yellow
        107  // 15 Bright White
    };
    if (c < 0) c = 0;
    if (c > 15) c = 15;
    return map[c];
}

// -------------------- TAB/PRINT helpers (column-aware) --------------------

static constexpr int BASIC_TAB_WIDTH = 14;

static inline void basic_print_char(Env& env, char c) {
    std::cout << c;
    if (c == '\n' || c == '\r') env.printCol = 0;
    else env.printCol++;
}

static inline void basic_print_string(Env& env, const std::string& s) {
    for (char c : s) basic_print_char(env, c);
}

static inline void basic_print_tab_to_next_stop(Env& env) {
    int next = ((env.printCol / BASIC_TAB_WIDTH) + 1) * BASIC_TAB_WIDTH;
    while (env.printCol < next) basic_print_char(env, ' ');
}

static inline void basic_print_tab_to_column1(Env& env, int col1based) {
    if (col1based < 1) col1based = 1;
    int target = col1based - 1;
    while (env.printCol < target) basic_print_char(env, ' ');
}

// Small shim used by TAB() to call the helper (keeps Parser::callFunction earlier clean)
void __basic_tab_side_effect(Env& env, int col1based) {
    basic_print_tab_to_column1(env, col1based);
}


// -------------------- Parser statement execution --------------------

void Parser::jumpToLine(int target) {
    auto it = env.program.find(target);
    if (it == env.program.end()) throw RuntimeError("Undefined line number");
    env.pc = it;
    env.posInLine = 0;
    throw RuntimeError("__JUMP__");
}

void Parser::exec_PRINT() {
    // PRINT [expr][;|, expr ...]
    bool newline = true;
    bool first = true;

    while (tok.kind != TokenKind::End && tok.kind != TokenKind::Colon) {
        (void)first;
        first = false;

        // empty item (e.g., PRINT ,)
        if (tok.kind == TokenKind::Comma) {
            basic_print_tab_to_next_stop(env);
            tok = lex.next();
            newline = false;
            continue;
        }
        if (tok.kind == TokenKind::Semicolon) {
            tok = lex.next();
            newline = false;
            continue;
        }

        Value v = parseExpression();
        basic_print_string(env, v.asString());

        if (tok.kind == TokenKind::Comma) {
            basic_print_tab_to_next_stop(env);
            tok = lex.next();
            newline = false;
            continue;
        }
        if (tok.kind == TokenKind::Semicolon) {
            tok = lex.next();
            newline = false;
            continue;
        }

        if (tok.kind != TokenKind::End && tok.kind != TokenKind::Colon) {
            basic_print_char(env, ' ');
            newline = false;
        }
    }

    if (newline) basic_print_char(env, '\n');
}

void Parser::exec_LET_or_ASSIGN() {
    bool hadLet = accept(TokenKind::KW_LET);
    (void)hadLet;

    if (tok.kind != TokenKind::Identifier) throw ParseError("Expected variable name");
    std::string name = tok.text;
    tok = lex.next();

    bool isArray = false;
    int idx = 0;
    if (tok.kind == TokenKind::LParen) {
        auto args = parseArgList();
        if (args.size() != 1) throw RuntimeError("Bad subscript");
        idx = static_cast<int>(args[0].asNumber());
        isArray = true;
    }

    consume(TokenKind::Equal, "'='");
    Value rhs = parseExpression();

    if (isArray) env.setArrayElem(name, idx, rhs);
    else env.setVar(name, rhs);
}

void Parser::exec_INPUT() {
    std::string prompt;
    if (tok.kind == TokenKind::String) {
        prompt = tok.text;
        tok = lex.next();
        if (tok.kind == TokenKind::Semicolon || tok.kind == TokenKind::Comma) tok = lex.next();
    }

    while (true) {
        if (tok.kind != TokenKind::Identifier) throw ParseError("Expected variable name");
        std::string name = tok.text;
        tok = lex.next();

        bool isArray = false;
        int idx = 0;
        if (tok.kind == TokenKind::LParen) {
            auto args = parseArgList();
            if (args.size() != 1) throw RuntimeError("Bad subscript");
            idx = static_cast<int>(args[0].asNumber());
            isArray = true;
        }

        if (!prompt.empty()) std::cout << prompt;
        else std::cout << "? ";

        std::string line;
        if (!std::getline(std::cin, line)) line = "";
        line = trim(line);

        Value v;
        if (!name.empty() && name.back() == '$') {
            v = Value(line);
        } else {
            char* end = nullptr;
            double d = std::strtod(line.c_str(), &end);
            if (end == line.c_str()) d = 0.0;
            v = Value(d);
        }

        if (isArray) env.setArrayElem(name, idx, v);
        else env.setVar(name, v);

        if (tok.kind == TokenKind::Comma) {
            tok = lex.next();
            continue;
        }
        break;
    }
}

void Parser::exec_GOTO(bool isGosub) {
    if (tok.kind != TokenKind::Number) throw ParseError("Expected line number");
    int target = static_cast<int>(tok.number);
    tok = lex.next();

    if (isGosub) {
        markLineProgress();
        env.gosubStack.push_back({env.pc, env.posInLine});
    }

    jumpToLine(target);
}

void Parser::exec_RETURN() {
    if (env.gosubStack.empty()) throw RuntimeError("RETURN without GOSUB");
    auto [it, pos] = env.gosubStack.back();
    env.gosubStack.pop_back();
    env.pc = it;
    env.posInLine = pos;
    // If we are returning from an INTERVAL interrupt handler, clear ISR flag.
    env.inIntervalISR = false;
    throw RuntimeError("__JUMP__");
}

void Parser::exec_IF() {
    Value cond = parseExpression();
    consume(TokenKind::KW_THEN, "THEN");
    // Remember where the first token after THEN begins.
    size_t thenStmtStart = lex.tokenStart;

    bool truthy = (cond.asNumber() != 0.0);
    if (!truthy) {
        // In BASIC, ':' after THEN is still part of the THEN-clause.
        // If the condition is false, skip the entire remainder of the line.
        while (tok.kind != TokenKind::End) tok = lex.next();
        return;
    }

    if (tok.kind == TokenKind::Number) {
        int target = static_cast<int>(tok.number);
        tok = lex.next();
        jumpToLine(target);
    }

    std::string rest = lex.s.substr(thenStmtStart);
    if (!rest.empty()) {
        Parser p2(rest, env);
        p2.currentLine = currentLine;
        p2.linePosBase = linePosBase + thenStmtStart;
        while (p2.tok.kind != TokenKind::End) {
            p2.execOneStatement();
            if (p2.tok.kind == TokenKind::Colon) {
                p2.tok = p2.lex.next();
                continue;
            }
            break;
        }
        tok = Token{TokenKind::End, "", 0.0};
        lex.i = lex.s.size();
    }
}

void Parser::exec_FOR() {
    if (tok.kind != TokenKind::Identifier) throw ParseError("Expected variable name");
    std::string var = tok.text;
    tok = lex.next();
    consume(TokenKind::Equal, "'='");
    double start = parseExpression().asNumber();
    consume(TokenKind::KW_TO, "TO");
    double end = parseExpression().asNumber();
    double step = 1.0;
    if (accept(TokenKind::KW_STEP)) {
        step = parseExpression().asNumber();
        if (step == 0.0) throw RuntimeError("STEP cannot be 0");
    }

    env.setVar(var, Value(start));

    markLineProgress();
    auto resumeIt = env.pc;
    size_t resumePos = env.posInLine;

    if (tok.kind == TokenKind::End) {
        auto it2 = env.pc;
        if (it2 != env.program.end()) ++it2;
        resumeIt = it2;
        resumePos = 0;
    } else if (tok.kind == TokenKind::Colon) {
        // FOR with an inline body on the same line.
        // IMPORTANT: do NOT consume the ':' here. `parseAndExecLine()` uses ':' as the
        // statement separator; if we advance past it, the rest of the line won't run.
        resumeIt = env.pc;
        // Resume after ':' (tokenEnd points to the character just after ':').
        resumePos = linePosBase + lex.tokenEnd;
        // Leave tok as ':' so the outer loop can advance and execute the inline body.
    }

    Env::ForFrame frame;
    frame.var = var;
    frame.endValue = end;
    frame.step = step;
    frame.returnIt = resumeIt;
    frame.posInLine = resumePos;
    
    // GW-BASIC semantics: remove any existing FOR with same control variable (case-insensitive)
    const std::string uvar = upperName(var);
    for (int i = static_cast<int>(env.forStack.size()) - 1; i >= 0; --i) {
        if (upperName(env.forStack[static_cast<size_t>(i)].var) == uvar) {
            env.forStack.erase(env.forStack.begin() + i, env.forStack.end());
            break;
        }
    }
    
    env.forStack.push_back(std::move(frame));
}

void Parser::exec_NEXT() {
    std::string var;
    if (tok.kind == TokenKind::Identifier) {
        var = tok.text;
        tok = lex.next();
    }
    if (env.forStack.empty()) {
        throw RuntimeError("NEXT without FOR");
    }

    int idxFrame = static_cast<int>(env.forStack.size()) - 1;

    // If NEXT specifies a variable, find the most recent FOR for that variable.
    // BASIC is case-insensitive.
    if (!var.empty()) {
        const std::string uvar = upperName(var);
        bool found = false;
        for (int i = idxFrame; i >= 0; --i) {
            if (upperName(env.forStack[static_cast<size_t>(i)].var) == uvar) {
                idxFrame = i;
                found = true;
                break;
            }
        }
        if (!found) {
            throw RuntimeError("NEXT without FOR");
        }

        // Drop any inner FORs above the matched one (GOTO can jump out of inner loops).
        if (idxFrame + 1 < static_cast<int>(env.forStack.size())) {
            env.forStack.erase(env.forStack.begin() + (idxFrame + 1), env.forStack.end());
        }
    }

    Env::ForFrame &frame = env.forStack.back();
    double cur = env.getVar(frame.var).asNumber();
    cur += frame.step;
    env.setVar(frame.var, Value(cur));

    bool cont = (frame.step >= 0.0) ? (cur <= frame.endValue) : (cur >= frame.endValue);
    if (cont) {
        env.pc = frame.returnIt;
        env.posInLine = frame.posInLine;
        throw RuntimeError("__JUMP__");
    }

    env.forStack.pop_back();
}

void Parser::exec_DIM() {
    while (true) {
        if (tok.kind != TokenKind::Identifier) throw ParseError("Expected array name");
        std::string name = tok.text;
        tok = lex.next();
        consume(TokenKind::LParen, "'('");
        Value v = parseExpression();
        consume(TokenKind::RParen, "')'");
        int ub = static_cast<int>(v.asNumber());
        env.dimArray(name, ub);

        if (tok.kind == TokenKind::Comma) {
            tok = lex.next();
            continue;
        }
        break;
    }
}

void Parser::exec_ON() {
    // Only implementing: ON INTERVAL <ticks> GOSUB <line>
    // (MS/GW-BASIC-like, simplified)

    if (tok.kind != TokenKind::KW_INTERVAL) {
        throw RuntimeError("Unsupported ON event (only ON INTERVAL implemented)");
    }
    tok = lex.next();

    // Accept (value in 1/60th-second ticks):
    //   ON INTERVAL 60 GOSUB 100        ' 1 second
    //   ON INTERVAL(30) GOSUB 100       ' 0.5 seconds
    //   ON INTERVAL = 120 GOSUB 100     ' 2 seconds

    // Optional '=' form
    (void)accept(TokenKind::Equal);

    // Optional parentheses: ON INTERVAL(5)
    // NOTE: interval value is specified in 1/60th-second units (ticks).
    double ticks = 0.0;
    if (accept(TokenKind::LParen)) {
        Value v = parseExpression();
        consume(TokenKind::RParen, "')'");
        ticks = v.asNumber();
    } else {
        ticks = parseExpression().asNumber();
    }

    // Convert ticks (1/60s) -> seconds
    env.intervalSeconds = ticks / 60.0;

    consume(TokenKind::KW_GOSUB, "GOSUB");
    if (tok.kind != TokenKind::Number) throw ParseError("Expected line number");
    env.intervalGosubLine = static_cast<int>(tok.number);
    tok = lex.next();

    // Arm the interval, but keep enabled state as-is; INTERVAL ON/OFF controls it.
    env.intervalArmed = true;

    // Initialize schedule so it can fire starting now + period.
    auto now = std::chrono::steady_clock::now();
    env.nextIntervalFire = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(std::max(0.0, env.intervalSeconds))
    );
}

void Parser::exec_INTERVAL_CTRL() {
    // INTERVAL ON | INTERVAL OFF | INTERVAL STOP
    // - ON: enable timer (scheduled based on current intervalSeconds)
    // - OFF: disable timer (keeps settings)
    // - STOP: disable and disarm

    if (tok.kind == TokenKind::KW_ON) {
        tok = lex.next();
        env.intervalEnabled = true;
        if (env.intervalArmed && env.intervalSeconds > 0.0) {
            auto now = std::chrono::steady_clock::now();
            env.nextIntervalFire = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(env.intervalSeconds)
            );
        }
        return;
    }

    if (tok.kind == TokenKind::KW_OFF) {
        tok = lex.next();
        env.intervalEnabled = false;
        return;
    }

    if (tok.kind == TokenKind::KW_STOP) {
        tok = lex.next();
        env.intervalEnabled = false;
        env.intervalArmed = false;
        return;
    }

    throw RuntimeError("Expected INTERVAL ON/OFF/STOP");
}

void Parser::execOneStatement() {
    if (tok.kind == TokenKind::End || tok.kind == TokenKind::Colon) return;

    if (tok.kind == TokenKind::KW_REM) {
        tok = Token{TokenKind::End, "", 0.0};
        lex.i = lex.s.size();
        return;
    }

    switch (tok.kind) {
        case TokenKind::KW_ON:
            tok = lex.next();
            exec_ON();
            return;
        case TokenKind::KW_PRINT:
            tok = lex.next();
            exec_PRINT();
            return;
        case TokenKind::KW_INPUT:
            tok = lex.next();
            exec_INPUT();
            return;
        case TokenKind::KW_IF:
            tok = lex.next();
            exec_IF();
            return;
        case TokenKind::KW_GOTO:
            tok = lex.next();
            exec_GOTO(false);
            return;
        case TokenKind::KW_GOSUB:
            tok = lex.next();
            exec_GOTO(true);
            return;
        case TokenKind::KW_RETURN:
            tok = lex.next();
            exec_RETURN();
            return;
        case TokenKind::KW_FOR:
            tok = lex.next();
            exec_FOR();
            return;
        case TokenKind::KW_NEXT:
            tok = lex.next();
            exec_NEXT();
            return;
        case TokenKind::KW_DIM:
            tok = lex.next();
            exec_DIM();
            return;
        case TokenKind::KW_COLOR:
            tok = lex.next();
            exec_COLOR();
            return;
        case TokenKind::KW_INTERVAL:
            tok = lex.next();
            exec_INTERVAL_CTRL();
            return;
        case TokenKind::KW_CLS:
            tok = lex.next();
            // ANSI clear screen + cursor home (macOS Terminal/iTerm/VSCode terminal)
            std::cout << "\033[2J\033[H";
            env.printCol = 0;
            return;
        case TokenKind::KW_LOCATE:
            tok = lex.next();
            exec_LOCATE();
            return;
        case TokenKind::KW_RANDOMIZE:
            tok = lex.next();
            exec_RANDOMIZE();
            return;
        case TokenKind::KW_DEFINT:
            tok = lex.next();
            exec_DEFINT();
            return;
        case TokenKind::KW_KEY:
            tok = lex.next();
            exec_KEY_CTRL();
            return;
        case TokenKind::KW_CLEAR:
            tok = lex.next();
            exec_CLEAR();
            return;
        case TokenKind::KW_END:
        case TokenKind::KW_STOP:
            env.running = false;
            env.contAvailable = false;
            tok = Token{TokenKind::End, "", 0.0};
            lex.i = lex.s.size();
            return;
        case TokenKind::KW_LET:
            exec_LET_or_ASSIGN();
            return;
        case TokenKind::KW_DATA:
            tok = lex.next();
            exec_DATA();
            return;
        case TokenKind::KW_READ:
            tok = lex.next();
            exec_READ();
            return;
        case TokenKind::KW_RESTORE:
            tok = lex.next();
            exec_RESTORE();
            return;
        default:
            break;
    }

    if (tok.kind == TokenKind::Identifier) {
        exec_LET_or_ASSIGN();
        return;
    }

    (void)parseExpression();
}

void Parser::parseAndExecLine() {
    while (tok.kind != TokenKind::End) {
        // Timer safe-point before each statement
        maybeFireIntervalInterrupt();

        execOneStatement();

        // Timer safe-point after each statement
        maybeFireIntervalInterrupt();

        if (tok.kind == TokenKind::Colon) {
            tok = lex.next();
            continue;
        }
        break;
    }
}

void Parser::exec_LOCATE() {
    // LOCATE row[,col[,cursor]]
    // cursor: 0 = hide cursor, 1 = show cursor (GW-BASIC/MSX-style)
    int row = 1;
    int col = 1;
    int cursor = -1; // -1 = leave unchanged

    // Allow: LOCATE ,10 (missing row)
    if (tok.kind != TokenKind::Comma && tok.kind != TokenKind::End && tok.kind != TokenKind::Colon) {
        row = static_cast<int>(parseExpression().asNumber());
    }

    if (accept(TokenKind::Comma)) {
        // Optional col (can be missing: LOCATE row,)
        if (tok.kind != TokenKind::Comma && tok.kind != TokenKind::End && tok.kind != TokenKind::Colon) {
            col = static_cast<int>(parseExpression().asNumber());
        }

        // Optional 3rd parameter: cursor visibility
        if (accept(TokenKind::Comma)) {
            if (tok.kind != TokenKind::End && tok.kind != TokenKind::Colon) {
                cursor = static_cast<int>(parseExpression().asNumber());
            }
        }
    }

    if (row < 1) row = 1;
    if (col < 1) col = 1;

    // Cursor visibility (ANSI): show = CSI ? 25 h, hide = CSI ? 25 l
    if (cursor == 0) {
        std::cout << "\033[?25l";
    } else if (cursor == 1) {
        std::cout << "\033[?25h";
    }

    // ANSI cursor position is 1-based
    std::cout << "\033[" << row << ";" << col << "H";
    env.printCol = col - 1;
}

void Parser::exec_COLOR() {
    // COLOR f,b
    // f = foreground (0..15), b = background (0..15)
    // If only one argument is provided, it's the foreground.
    // If a comma is present, background may follow.

    int fg = -1;
    int bg = -1;

    // Allow: COLOR ,b (missing foreground)
    if (tok.kind != TokenKind::Comma && tok.kind != TokenKind::End && tok.kind != TokenKind::Colon) {
        fg = static_cast<int>(parseExpression().asNumber());
    }

    if (accept(TokenKind::Comma)) {
        if (tok.kind != TokenKind::End && tok.kind != TokenKind::Colon) {
            bg = static_cast<int>(parseExpression().asNumber());
        }
    }

    // Apply ANSI SGR sequences
    if (fg >= 0) {
        if (fg < 0) fg = 0;
        if (fg > 15) fg = 15;
        std::cout << "\033[" << basic_ansi_fg_code(fg) << "m";
    }

    if (bg >= 0) {
        if (bg < 0) bg = 0;
        if (bg > 15) bg = 15;
        // Set background for subsequent output only (GW-BASIC-style)
        std::cout << "\033[" << basic_ansi_bg_code(bg) << "m";
    }
}

void Parser::exec_RANDOMIZE() {
    // RANDOMIZE [seed]
    // If no seed, use current time.
    if (tok.kind == TokenKind::End || tok.kind == TokenKind::Colon) {
        unsigned seed = static_cast<unsigned>(std::time(nullptr));
        std::srand(seed);
        env.hasLastRnd = false;
        return;
    }

    Value v = parseExpression();
    unsigned seed = static_cast<unsigned>(static_cast<long long>(v.asNumber()));
    std::srand(seed);
    env.hasLastRnd = false;
}

void Parser::exec_DEFINT() {
    // DEFINT A-Z, DEFINT A-C, X, M-P, etc.
    // Sets default type to INTEGER (16-bit) for variables starting with given letters.

    auto read_letter = [&]() -> char {
        if (tok.kind != TokenKind::Identifier || tok.text.empty())
            throw ParseError("Expected letter in DEFINT");
        char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(tok.text[0])));
        if (ch < 'A' || ch > 'Z')
            throw ParseError("Expected A-Z letter in DEFINT");
        tok = lex.next();
        return ch;
    };

    while (true) {
        bool hadParen = accept(TokenKind::LParen);

        char a = read_letter();
        char b = a;
        if (accept(TokenKind::Minus)) {
            b = read_letter();
        }

        if (hadParen) consume(TokenKind::RParen, "')'");

        env.setDefIntRange(a, b, true);

        if (accept(TokenKind::Comma)) continue;
        break;
    }
}

void Parser::exec_CLEAR() {
    // CLEAR [n]
    // GW-BASIC allows CLEAR to take parameters for memory/string space.
    // This interpreter doesn't use those memory model tunings, but we accept
    // an optional numeric argument and ignore it.

    if (tok.kind != TokenKind::End && tok.kind != TokenKind::Colon) {
        // parse and ignore optional numeric parameter
        (void)parseExpression();
    }

    // CLEAR runtime state (variables/arrays), keep program intact.
    // Many GW-BASIC programs use CLEAR n as a memory-tuning hint; they don't expect it
    // to break active control flow. Preserve FOR/GOSUB stacks.
    auto savedFor = env.forStack;
    auto savedGosub = env.gosubStack;
    bool savedInISR = env.inIntervalISR;

    env.clearVars();

    env.forStack = std::move(savedFor);
    env.gosubStack = std::move(savedGosub);
    env.inIntervalISR = savedInISR;
}

void Parser::exec_KEY_CTRL() {
    // KEY ON / KEY OFF
    // GW-BASIC controls function-key macro display/handling. We accept and ignore.

    if (tok.kind == TokenKind::KW_ON) {
        tok = lex.next();
        return;
    }
    if (tok.kind == TokenKind::KW_OFF) {
        tok = lex.next();
        return;
    }

    throw RuntimeError("Expected KEY ON/OFF");
}

void Parser::exec_DATA() {
    // DATA is non-executable: skip to end of statement (or ':' / end)
    while (tok.kind != TokenKind::End && tok.kind != TokenKind::Colon) {
        tok = lex.next();
    }
}

void Parser::exec_RESTORE() {
    // RESTORE [line]
    int line = 0;
    if (tok.kind == TokenKind::Number) {
        line = (int)tok.number;
        tok = lex.next();
    }
    env.restoreData(line, env.program);
}

void Parser::exec_READ() {
    // READ var[,var...]
    while (true) {
        if (tok.kind != TokenKind::Identifier) throw ParseError("Expected variable name");
        std::string name = tok.text;
        tok = lex.next();

        bool isArray = false;
        int idx = 0;
        if (tok.kind == TokenKind::LParen) {
            auto args = parseArgList();
            if (args.size() != 1) throw RuntimeError("Bad subscript");
            idx = (int)args[0].asNumber();
            isArray = true;
        }

        bool wantString = (!name.empty() && name.back() == '$');
        Value v = env.readNextData(wantString, env.program);

        if (isArray) env.setArrayElem(name, idx, v);
        else env.setVar(name, v);

        if (accept(TokenKind::Comma)) continue;
        break;
    }
}

