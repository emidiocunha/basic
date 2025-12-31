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
            {"RND",true},{"VAL",true},{"STR$",true},{"LEN",true},{"LEFT$",true},{"RIGHT$",true},{"MID$",true},{"CHR$",true},{"ASC",true},{"TAB",true}
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
            return Value((x > 0) ? 1.0 : ((x < 0) ? -1.0 : 0.0));
        }
        if (upper == "RND") {
            // GW-BASIC-ish behavior:
            //  RND(x<0)  : reseed using x and return a new value
            //  RND(0)    : return the last value (or generate one if none)
            //  RND(x>0)  : return a new value
            double x = args.empty() ? 1.0 : argN(0);

            if (x < 0.0) {
                long long sx = static_cast<long long>(x);
                unsigned seed = static_cast<unsigned>(std::llabs(sx));
                std::srand(seed);
                double r = static_cast<double>(std::rand()) / (static_cast<double>(RAND_MAX) + 1.0);
                env.lastRnd = r;
                env.hasLastRnd = true;
                return Value(r);
            }
            if (x == 0.0) {
                if (!env.hasLastRnd) {
                    double r = static_cast<double>(std::rand()) / (static_cast<double>(RAND_MAX) + 1.0);
                    env.lastRnd = r;
                    env.hasLastRnd = true;
                }
                return Value(env.lastRnd);
            }

            double r = static_cast<double>(std::rand()) / (static_cast<double>(RAND_MAX) + 1.0);
            env.lastRnd = r;
            env.hasLastRnd = true;
            return Value(r);
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
            return Value(a.asNumber() + b.asNumber());
        }
        if (op == TokenKind::Minus) return Value(a.asNumber() - b.asNumber());
        if (op == TokenKind::Star) return Value(a.asNumber() * b.asNumber());
        if (op == TokenKind::Slash) return Value(a.asNumber() / b.asNumber());
        if (op == TokenKind::Backslash) {
            double denom = b.asNumber();
            if (denom == 0.0) throw RuntimeError("Division by zero");
            double q = a.asNumber() / denom;
            // GW-BASIC-style integer division: truncate toward zero.
            double tq = std::trunc(q);
            return Value(tq);
        }
        if (op == TokenKind::Caret) return Value(std::pow(a.asNumber(), b.asNumber()));
        if (op == TokenKind::KW_MOD) {
            double x = a.asNumber();
            double y = b.asNumber();
            if (y == 0.0) throw RuntimeError("Division by zero");
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

            if (tok.kind == TokenKind::LParen && isFunction(upper)) {
                auto args = parseArgList();
                return callFunction(upper, std::move(args));
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
    void exec_LOCATE();
    void exec_RANDOMIZE();
    void jumpToLine(int target);

    void markLineProgress() {
        env.posInLine = linePosBase + lex.i;
    }
};

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
    throw RuntimeError("__JUMP__");
}

void Parser::exec_IF() {
    Value cond = parseExpression();
    auto r = lex.i;
    
    consume(TokenKind::KW_THEN, "THEN");

    bool truthy = (cond.asNumber() != 0.0);
    if (!truthy) {
        while (tok.kind != TokenKind::End && tok.kind != TokenKind::Colon) tok = lex.next();
        return;
    }

    if (tok.kind == TokenKind::Number) {
        int target = static_cast<int>(tok.number);
        tok = lex.next();
        jumpToLine(target);
    }

    std::string rest = lex.s.substr(r);
    if (!rest.empty()) {
        Parser p2(rest, env);
        p2.currentLine = currentLine;
        p2.linePosBase = linePosBase + lex.i;
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
        tok = lex.next();
        markLineProgress();
        resumeIt = env.pc;
        resumePos = env.posInLine;
    }

    Env::ForFrame frame;
    frame.var = var;
    frame.endValue = end;
    frame.step = step;
    frame.returnIt = resumeIt;
    frame.posInLine = resumePos;
    env.forStack.push_back(std::move(frame));
}

void Parser::exec_NEXT() {
    std::string var;
    if (tok.kind == TokenKind::Identifier) {
        var = tok.text;
        tok = lex.next();
    }

    if (env.forStack.empty()) throw RuntimeError("NEXT without FOR");

    int idxFrame = static_cast<int>(env.forStack.size()) - 1;
    if (!var.empty()) {
        bool found = false;
        for (int i = idxFrame; i >= 0; --i) {
            if (env.forStack[static_cast<size_t>(i)].var == var) { idxFrame = i; found = true; break; }
        }
        if (!found) throw RuntimeError("NEXT without matching FOR");
    }

    Env::ForFrame frame = env.forStack[static_cast<size_t>(idxFrame)];
    double cur = env.getVar(frame.var).asNumber();
    cur += frame.step;
    env.setVar(frame.var, Value(cur));

    bool cont = (frame.step >= 0.0) ? (cur <= frame.endValue) : (cur >= frame.endValue);
    if (cont) {
        env.pc = frame.returnIt;
        env.posInLine = frame.posInLine;
        throw RuntimeError("__JUMP__");
    }

    env.forStack.erase(env.forStack.begin() + idxFrame, env.forStack.end());
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

void Parser::execOneStatement() {
    if (tok.kind == TokenKind::End || tok.kind == TokenKind::Colon) return;

    if (tok.kind == TokenKind::KW_REM) {
        tok = Token{TokenKind::End, "", 0.0};
        lex.i = lex.s.size();
        return;
    }

    switch (tok.kind) {
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
        execOneStatement();
        if (tok.kind == TokenKind::Colon) {
            tok = lex.next();
            continue;
        }
        break;
    }
}

void Parser::exec_LOCATE() {
    // LOCATE row[,col]
    int row = 1;
    int col = 1;

    // Allow: LOCATE ,10 (missing row)
    if (tok.kind != TokenKind::Comma && tok.kind != TokenKind::End && tok.kind != TokenKind::Colon) {
        row = static_cast<int>(parseExpression().asNumber());
    }
    if (accept(TokenKind::Comma)) {
        if (tok.kind != TokenKind::End && tok.kind != TokenKind::Colon) {
            col = static_cast<int>(parseExpression().asNumber());
        }
    }

    if (row < 1) row = 1;
    if (col < 1) col = 1;

    // ANSI cursor position is 1-based
    std::cout << "\033[" << row << ";" << col << "H";
    env.printCol = col - 1;
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

