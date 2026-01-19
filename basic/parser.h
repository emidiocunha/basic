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
    void exec_BEEP();
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
        // Save progress at a safe resume point (between statements).
        // Skip any whitespace so resuming doesn't start at an ambiguous position.
        size_t p = linePosBase + lex.i;
        while (p < currentLine.size() && std::isspace(static_cast<unsigned char>(currentLine[p]))) {
            ++p;
        }
        env.posInLine = p;
    }
    void markStatementStart() {
        // lexer tracks the start index of the current token; for interrupts we want to
        // resume *at* the statement keyword, not after it.
        env.posInLine = linePosBase + lex.tokenStart;
    }

    // Timer safe-point: fire interval interrupt if needed
    void maybeFireIntervalInterrupt() {
        if (!env.intervalEnabled) return;
        if (!env.intervalArmed) return;
        if (env.inIntervalISR) return;
        if (env.intervalSeconds <= 0.0) return;
        if (env.intervalGosubLine <= 0) return;

        auto now = std::chrono::steady_clock::now();
        if (now < env.nextIntervalFire) return;

        // Schedule next fire before jumping.
        env.nextIntervalFire = now + std::chrono::milliseconds(
            static_cast<int>(env.intervalSeconds * 1000.0)
        );

        // Fire ONLY between lines: resume at the start of the NEXT line after RETURN.
        auto retIt = env.pc;
        if (retIt != env.program.end()) ++retIt;

        env.gosubStack.push_back({retIt, 0, true, env.dataPtr});
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
    if (env.screen.putChar) env.screen.putChar(c);
    else std::cout << c;

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

// --- RUN immediate command support ---
// Ensure ON INTERVAL mechanism is reset before each RUN.
inline void basic_reset_run_event_control(Env& env) {
    // RUN should start from a clean event/control state.
    // Reset ON INTERVAL settings so a previous run can't fire during a new run.
    env.intervalEnabled = false;
    env.intervalArmed = false;
    env.inIntervalISR = false;
    env.intervalSeconds = 0.0;
    env.intervalGosubLine = 0;
    env.nextIntervalFire = std::chrono::steady_clock::now();

    // Also clear control stacks for a fresh run.
    env.forStack.clear();
    env.gosubStack.clear();
}


