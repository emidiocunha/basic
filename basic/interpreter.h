//
//  interpreter.h
//  basic
//
//  Created by Emídio Cunha on 28/12/2025.
//

#pragma once

#include "token.h"
#include "lexer.h"
#include <atomic>
#include <csignal>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <deque>

static std::string normalize_keywords_upper_preserve(const std::string& line) {
    Lexer lx(line);
    std::string out;
    size_t last = 0;

    while (true) {
        Token t = lx.next();
        if (t.kind == TokenKind::End) {
            // append any remaining trailing whitespace
            if (last < line.size()) out += line.substr(last);
            break;
        }

        // copy any whitespace/characters between previous token end and this token start
        if (last < lx.tokenStart) out += line.substr(last, lx.tokenStart - last);

        // token raw text as in the original source
        std::string raw = line.substr(lx.tokenStart, lx.tokenEnd - lx.tokenStart);

        if (t.kind == TokenKind::KW_REM) {
            // Uppercase REM itself, then preserve the remainder verbatim
            out += "REM";
            if (lx.tokenEnd < line.size()) out += line.substr(lx.tokenEnd);
            break;
        }

        if (is_basic_keyword(t.kind)) {
            // Uppercase only the keyword text (raw contains just the identifier/keyword)
            out += upper_ascii(raw);
        } else {
            // Preserve everything else exactly (identifiers, numbers, operators, strings)
            out += raw;
        }

        last = lx.tokenEnd;
    }

    return out;
}

struct ScopedRawInput {
    termios old{};
    bool active = false;
    ScopedRawInput() {
        if (!isatty(STDIN_FILENO)) return;
        termios t{};
        if (tcgetattr(STDIN_FILENO, &old) != 0) return;
        t = old;
        // Character-at-a-time input, but keep ISIG so Ctrl+C still generates SIGINT.
        t.c_lflag &= static_cast<unsigned>(~(ECHO | ICANON));
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0) {
            active = true;
        }
    }
    ~ScopedRawInput() {
        if (active) {
            tcsetattr(STDIN_FILENO, TCSANOW, &old);
        }
    }
};


// -------------------- Ctrl+C (SIGINT) handling --------------------
static std::atomic<bool> g_sigint_requested{false};
static std::atomic<bool> g_sigwinch_requested{false};

static void basic_sigint_handler(int) {
    g_sigint_requested.store(true, std::memory_order_relaxed);
}

static void basic_sigwinch_handler(int) {
    g_sigwinch_requested.store(true, std::memory_order_relaxed);
}

static inline void install_basic_sigint_handler_once() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    std::signal(SIGINT, basic_sigint_handler);
    std::signal(SIGWINCH, basic_sigwinch_handler);
}

static inline void basic_update_terminal_size(int &cols, int &rows) {
    if (!isatty(STDOUT_FILENO)) return;
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0) cols = static_cast<int>(ws.ws_col);
        if (ws.ws_row > 0) rows = static_cast<int>(ws.ws_row);
    }
}

struct Interpreter {
    int termCols = 80;
    int termRows = 24;
    Interpreter() {
        install_basic_sigint_handler_once();
        basic_update_terminal_size(termCols, termRows);
    }

    void resetAfterProgramEdit() {
        // Any edit to the program invalidates execution state and CONT.
        env.running = false;
        env.stopped = false;
        env.contAvailable = false;
        env.posInLine = 0;
        env.pc = env.program.end();
        // Program text changed: DATA cache is now stale.
        env.dataCacheBuilt = false;
        env.dataCache.clear();
        env.dataPtr = 0;
    }

    void storeProgramLine(int ln, const std::string& restRaw) {
        if (ln <= 0) return;
        std::string rest = trim(restRaw);
        if (rest.empty()) {
            auto it = env.program.find(ln);
            if (it != env.program.end()) env.program.erase(it);
        } else {
            env.program[ln] = normalize_keywords_upper_preserve(rest);
        }
        resetAfterProgramEdit();
    }
    
    void cmd_SAVE(const std::string& filename) {
        std::ofstream out(filename);
        if (!out) {
            std::cout << "Cannot open file for writing: " << filename << "\n";
            return;
        }
        for (const auto& [ln, text] : env.program) {
            out << ln << " " << text << "\n";
        }
        try {
            std::filesystem::path p = std::filesystem::absolute(filename);
            std::cout << "Saved to: " << p.string() << "\n";
        } catch (...) {
            std::cout << "Saved to: " << filename << "\n";
        }
    }

    void cmd_LOAD(const std::string& filename) {
        std::ifstream in(filename);
        if (!in) {
            std::cout << "Cannot open file for reading: " << filename << "\n";
            return;
        }

        env.program.clear();
        resetAfterProgramEdit();

        std::string line;
        while (std::getline(in, line)) {
            std::string t = trim(line);
            if (t.empty()) continue;

            // Expect: <lineNumber> <rest>
            if (!std::isdigit(static_cast<unsigned char>(t[0]))) continue;

            std::istringstream iss(t);
            int ln = 0;
            iss >> ln;
            if (ln <= 0) continue;

            std::string rest;
            std::getline(iss, rest);
            rest = trim(rest);

            storeProgramLine(ln, rest);
        }
        // After LOAD, show how many lines are in memory.
        std::cout << "Loaded " << env.program.size() << " lines. ";
        std::cout << "OK\n";
    }
    Env env;

    void cmd_LIST(const std::string& args = "") {
        // LIST
        // Supported forms:
        //   LIST          -> all
        //   LIST X        -> only line X
        //   LIST X-       -> from X to end
        //   LIST -Y       -> from start to Y
        //   LIST X-Y      -> from X to Y
        std::string a = trim(args);
        bool hasDash = (a.find('-') != std::string::npos);

        auto parseInt = [](const std::string& s, int& out) -> bool {
            std::string t = trim(s);
            if (t.empty()) return false;
            // allow leading +
            size_t i = 0;
            if (t[0] == '+') i = 1;
            if (i >= t.size()) return false;
            for (size_t j = i; j < t.size(); ++j) {
                if (!std::isdigit(static_cast<unsigned char>(t[j]))) return false;
            }
            try {
                out = std::stoi(t);
            } catch (...) {
                return false;
            }
            return out > 0;
        };

        int start = 0;
        int end = 0;
        bool hasStart = false;
        bool hasEnd = false;

        if (a.empty()) {
            // no range
        } else if (!hasDash) {
            // LIST X
            int x = 0;
            if (!parseInt(a, x)) {
                std::cout << "LIST: bad line number\n";
                return;
            }
            start = x;
            end = x;
            hasStart = true;
            hasEnd = true;
        } else {
            // LIST X-Y / -Y / X-
            size_t dash = a.find('-');
            std::string left = a.substr(0, dash);
            std::string right = a.substr(dash + 1);

            int x = 0;
            int y = 0;
            if (parseInt(left, x)) {
                start = x;
                hasStart = true;
            }
            if (parseInt(right, y)) {
                end = y;
                hasEnd = true;
            }
            if (!hasStart && !hasEnd) {
                std::cout << "LIST: bad range\n";
                return;
            }
        }

        auto it = hasStart ? env.program.lower_bound(start) : env.program.begin();
        for (; it != env.program.end(); ++it) {
            int ln = it->first;
            if (hasEnd && ln > end) break;
            std::cout << ln << " " << it->second << "\n";
        }
    }

    void cmd_NEW() {
        env.clearProgramAndState();
        std::cout << "OK\n";
    }

    void cmd_CLEAR() {
        env.clearVars();
        env.contAvailable = false;
        std::cout << "OK\n";
    }

    void cmd_DELETE(int line) {
        storeProgramLine(line, "");
    }

    void runFromStart() {
        g_sigint_requested.store(false, std::memory_order_relaxed);
        env.clearVars();
        env.running = true;
        env.stopped = false;
        env.pc = env.program.begin();
        env.posInLine = 0;
        env.dataCacheBuilt = false;   // or env.rebuildDataCache(env.program);
        env.restoreData(0, env.program);
        basic_reset_run_event_control(env);
        execute();
    }

    void cont() {
        if (!env.contAvailable) {
            std::cout << "Cannot CONTINUE\n";
            return;
        }
        g_sigint_requested.store(false, std::memory_order_relaxed);
        env.running = true;
        env.stopped = false;
        execute();
    }

    void execute() {
        while (env.running && !env.stopped) {
            // Ctrl+C breaks execution and returns to the REPL.
            if (g_sigint_requested.exchange(false, std::memory_order_relaxed)) {
                std::cout << "\nBreak\n";
                env.running = false;
                env.stopped = false;
                env.contAvailable = true;
                return;
            }
            if (g_sigwinch_requested.exchange(false, std::memory_order_relaxed)) {
                basic_update_terminal_size(termCols, termRows);
            }

            if (env.pc == env.program.end()) {
                env.running = false;
                env.contAvailable = false;
                break;
            }
            int currentLineNumber = env.pc->first;
            std::string lineText = env.pc->second;

            std::string toParse;
            if (env.posInLine > 0 && env.posInLine < lineText.size()) {
                toParse = lineText.substr(env.posInLine);
            } else {
                env.posInLine = 0;
                toParse = lineText;
            }

            Parser p(toParse, env);
            p.currentLine = lineText;
            p.linePosBase = (env.posInLine);

            try {
                p.parseAndExecLine();
                env.pc++;
                env.posInLine = 0;
            } catch (const RuntimeError& e) {
                if (std::string(e.what()) == "__JUMP__") {
                    continue;
                }
                std::cout << "Runtime error in " << currentLineNumber << ": " << e.what() << "\n";
                env.running = false;
                env.contAvailable = true;
                return;
            } catch (const ParseError& e) {
                std::cout << "Syntax error in " << currentLineNumber << ": " << e.what() << "\n";
                env.running = false;
                env.contAvailable = true;
                return;
            }
        }
    }

    void executeImmediate(const std::string& line) {
        // Clear any pending Ctrl+C before immediate execution
        g_sigint_requested.store(false, std::memory_order_relaxed);
        Parser p(line, env);
        try {
            p.parseAndExecLine();
        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << "\n";
        }
    }

    void repl() {
        std::string line;
        std::deque<std::string> history; // command history (max 64)
        std::string historyDraft;        // what user was typing before history navigation
        int historyIndex = -1;           // index into history while navigating
        bool historyNav = false;         // currently navigating history
        ScopedRawInput raw; // disable terminal echo when possible
        while (true) {
            std::cout << "OK> ";
            line.clear();

            // If we're not attached to a real TTY (e.g., Xcode debug console),
            // fall back to line-based input to avoid broken arrow handling and double-echo.
            if (!raw.active) {
                if (!std::getline(std::cin, line)) break;
                // Ctrl+C may have interrupted getline in some environments
                if (g_sigint_requested.exchange(false, std::memory_order_relaxed)) {
                    std::cout << "\nBreak\n";
                    std::cin.clear();
                    line.clear();
                    continue;
                }
            } else {
                while (true) {
                    int ch = std::cin.get();
                    if (ch == EOF) break;

                    // Window resize
                    if (g_sigwinch_requested.exchange(false, std::memory_order_relaxed)) {
                        basic_update_terminal_size(termCols, termRows);
                        // Repaint the prompt + current input on a fresh line.
                        std::cout << "\nOK> " << line << std::flush;
                        continue;
                    }

                    // Ctrl+C
                    if (g_sigint_requested.exchange(false, std::memory_order_relaxed)) {
                        std::cout << "\nBreak\n";
                        line.clear();
                        break;
                    }

                    // Enter
                    if (ch == '\n' || ch == '\r') {
                        std::cout << '\n';
                        if (historyNav) { historyNav = false; historyIndex = -1; }
                        break;
                    }

                    // Backspace
                    if (ch == 127 || ch == '\b') {
                        if (historyNav) { historyNav = false; historyIndex = -1; }
                        if (!line.empty()) {
                            line.pop_back();
                            std::cout << "\b \b";
                        }
                        continue;
                    }

                    // Escape sequences (arrows / function keys)
                    if (ch == 27) { // ESC
                        int next = std::cin.get();
                        if (next == '[') {
                            int code = std::cin.get();

                            // Arrow keys: ESC [ A/B/C/D
                            if (code == 'A') { // UP arrow
                                if (!history.empty()) {
                                    if (!historyNav) {
                                        historyNav = true;
                                        historyDraft = line; // save what the user was typing
                                        historyIndex = static_cast<int>(history.size()) - 1;
                                    } else if (historyIndex > 0) {
                                        historyIndex--;
                                    }

                                    // clear current input
                                    while (!line.empty()) {
                                        std::cout << "\b \b";
                                        line.pop_back();
                                    }

                                    line = history[static_cast<size_t>(historyIndex)];
                                    std::cout << line;
                                }
                                continue;
                            }

                            if (code == 'B') { // DOWN arrow
                                if (historyNav) {
                                    if (historyIndex < static_cast<int>(history.size()) - 1) {
                                        historyIndex++;

                                        // clear current input
                                        while (!line.empty()) {
                                            std::cout << "\b \b";
                                            line.pop_back();
                                        }

                                        line = history[static_cast<size_t>(historyIndex)];
                                        std::cout << line;
                                    } else {
                                        // Past the newest entry -> restore draft (often empty)
                                        historyNav = false;
                                        historyIndex = -1;

                                        // clear current input
                                        while (!line.empty()) {
                                            std::cout << "\b \b";
                                            line.pop_back();
                                        }

                                        line = historyDraft;
                                        std::cout << line;
                                    }
                                }
                                continue;
                            }

                            // Function keys often arrive as ESC [ <digits> ~ (e.g. F5 = 15~)
                            if (std::isdigit(static_cast<unsigned char>(code))) {
                                std::string digits;
                                digits.push_back(static_cast<char>(code));
                                while (true) {
                                    int c2 = std::cin.get();
                                    if (c2 == EOF) break;
                                    if (c2 == '~') break;
                                    if (std::isdigit(static_cast<unsigned char>(c2))) {
                                        digits.push_back(static_cast<char>(c2));
                                        continue;
                                    }
                                    // Unknown/unsupported sequence; stop.
                                    break;
                                }

                                // F5 is commonly ESC [ 15 ~ in Terminal.app / xterm.
                                if (digits == "15") {
                                    std::cout << "\n";
                                    // Discard any partially typed command
                                    line.clear();
                                    // Execute RUN
                                    runFromStart();
                                }
                                continue;
                            }

                            continue;
                        }

                        if (next == 'O') {
                            // Some terminals use ESC O ... for special keys (keep UP support just in case)
                            int code = std::cin.get();
                            if (code == 'A') { // UP arrow
                                // (legacy support, just clear and recall last history entry)
                                if (!history.empty()) {
                                    // clear current input
                                    while (!line.empty()) {
                                        std::cout << "\b \b";
                                        line.pop_back();
                                    }
                                    line = history.back();
                                    std::cout << line;
                                }
                            }
                            continue;
                        }

                        continue;
                    }

                    // Regular character
                    if (historyNav) { historyNav = false; historyIndex = -1; }
                    line.push_back((char)ch);
                    std::cout << (char)ch;
                }
            }

            std::string t = trim(line);
            if (t.empty()) continue;

            // Record history (max 64). Avoid duplicating consecutive identical commands.
            if (history.empty() || history.back() != t) {
                history.push_back(t);
                if (history.size() > 64) history.pop_front();
            }
            // reset navigation state
            historyNav = false;
            historyIndex = -1;

            if (std::isdigit(static_cast<unsigned char>(t[0]))) {
                std::istringstream iss(t);
                int ln = 0;
                iss >> ln;
                if (ln <= 0) {
                    std::cout << "Bad line number\n";
                    continue;
                }
                std::string rest;
                std::getline(iss, rest);
                rest = trim(rest);
                storeProgramLine(ln, rest);
                continue;
            }

            std::string upper;
            upper.reserve(t.size());
            for (char c : t) upper.push_back(std::toupper(static_cast<unsigned char>(c)));

            if (upper == "RUN") { runFromStart(); continue; }
            if (istartswith(upper, "LIST")) {
                std::string rest = trim(t.substr(4));
                cmd_LIST(rest);
                continue;
            }
            if (upper == "NEW") { cmd_NEW(); continue; }
            if (upper == "CLEAR") { cmd_CLEAR(); continue; }
            if (upper == "CONT") { cont(); continue; }
            if (upper == "QUIT" || upper == "EXIT") {
                std::cout << "Bye\n";
                return; // exit REPL and terminate app
            }
            if (istartswith(upper, "SAVE")) {
                std::string rest = trim(t.substr(4));
                if (rest.empty() || rest[0] != '"') {
                    std::cout << "SAVE requires a filename in quotes\n";
                    continue;
                }
                size_t endq = rest.find('"', 1);
                if (endq == std::string::npos) {
                    std::cout << "SAVE requires a filename in quotes\n";
                    continue;
                }
                std::string fn = rest.substr(1, endq - 1);
                cmd_SAVE(fn);
                continue;
            }

            if (istartswith(upper, "LOAD")) {
                std::string rest = trim(t.substr(4));
                if (rest.empty() || rest[0] != '"') {
                    std::cout << "LOAD requires a filename in quotes\n";
                    continue;
                }
                size_t endq = rest.find('"', 1);
                if (endq == std::string::npos) {
                    std::cout << "LOAD requires a filename in quotes\n";
                    continue;
                }

                std::string fn = rest.substr(1, endq - 1);

                // Optional flags after filename: e.g. LOAD "file.bas",R
                bool runAfterLoad = false;
                std::string tail = trim(rest.substr(endq + 1));
                if (!tail.empty()) {
                    if (tail[0] != ',') {
                        std::cout << "LOAD: unexpected text after filename\n";
                        continue;
                    }
                    tail = trim(tail.substr(1));
                    std::string tailUpper;
                    tailUpper.reserve(tail.size());
                    for (char c : tail) tailUpper.push_back(std::toupper(static_cast<unsigned char>(c)));

                    if (tailUpper == "R") {
                        runAfterLoad = true;
                    } else {
                        std::cout << "LOAD: unknown option '" << tail << "'\n";
                        continue;
                    }
                }

                cmd_LOAD(fn);
                if (runAfterLoad) {
                    runFromStart();
                }
                continue;
            }
            if (istartswith(upper, "DELETE")) {
                std::istringstream iss(t.substr(6));
                int ln = 0;
                iss >> ln;
                if (ln > 0) cmd_DELETE(ln);
                else std::cout << "DELETE requires line number\n";
                continue;
            }
            if (upper == "EDIT") {
                run_editor(env);
                continue;
            }

            executeImmediate(t);
        }
    }
};
