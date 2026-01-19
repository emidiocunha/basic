//
//  parser.cpp
//  basic
//
//  Created by Emídio Cunha on 16/01/2026.
//

#include "parser.h"
#include "interpreter.h"

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

        if (!prompt.empty()) basic_print_string(env, prompt);
        else basic_print_string(env, "? ");

        std::string line;
        if (!Interpreter::basic_getline_with_sdl_pump(line)) {
            throw RuntimeError("Input aborted");
        }
        line = trim(line);
        // INPUT is line-oriented; once user submits, BASIC typically continues on the next line.
        // Keep output consistent for SDL by ending the prompt line.
        basic_print_char(env, '\n');

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
        env.gosubStack.push_back({env.pc, env.posInLine, false, 0});
    }

    jumpToLine(target);
}

void Parser::exec_RETURN() {
    if (env.gosubStack.empty()) throw RuntimeError("RETURN without GOSUB");

    Env::GosubFrame fr = env.gosubStack.back();
    env.gosubStack.pop_back();

    env.pc = fr.it;
    env.posInLine = fr.pos;

    // Clear ISR flag only when returning from the ON INTERVAL handler frame.
    if (fr.isInterval) {
        // Restore DATA/READ position to what it was when the interval interrupted.
        env.dataPtr = fr.savedDataPtr;
        env.inIntervalISR = false;
    }

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
        case TokenKind::KW_BEEP:
            tok = lex.next();
            exec_BEEP();
            return;
        case TokenKind::KW_INTERVAL:
            tok = lex.next();
            exec_INTERVAL_CTRL();
            return;
        case TokenKind::KW_CLS:
            tok = lex.next();
            if (env.screen.cls) env.screen.cls();
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
        execOneStatement();
        if (tok.kind == TokenKind::Colon) {
            tok = lex.next();
            continue;
        }
        break;
    }
    // Interval safe-point between lines
    maybeFireIntervalInterrupt();
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

    if (cursor == 0) {
        if (env.screen.showCursor) env.screen.showCursor(false);
    } else if (cursor == 1) {
        if (env.screen.showCursor) env.screen.showCursor(true);
    }

    if (env.screen.locate) env.screen.locate(row, col);
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

    // Apply via screen driver (SDL) if present.
    if (env.screen.color) {
        // Clamp to GW-BASIC range 0..15 when provided.
        int cFg = fg;
        int cBg = bg;
        if (cFg >= 0) {
            if (cFg < 0) cFg = 0;
            if (cFg > 15) cFg = 15;
        }
        if (cBg >= 0) {
            if (cBg < 0) cBg = 0;
            if (cBg > 15) cBg = 15;
        }
        env.screen.color(cFg, cBg);
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

void Parser::exec_BEEP() {
    // BEEP
    // Emit a simple bell. On ANSI terminals this is '\a'.
    // Accept and ignore optional parameters if present.
    if (tok.kind != TokenKind::End && tok.kind != TokenKind::Colon) {
        (void)parseExpression();
        if (accept(TokenKind::Comma)) {
            (void)parseExpression();
        }
    }
    if (env.screen.beep) env.screen.beep();
    else std::cout << '\a' << std::flush;
}


