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
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cmath>
#include <vector>
#include <cstring>
#include <algorithm>

#include "SDL.h"
#include "SDL_ttf.h"

// In-place SDL editor (renders into the existing REPL window/renderer).
void run_editor_inplace(Env& env,
                        SDL_Window* win,
                        SDL_Renderer* ren,
                        TTF_Font* font,
                        int cols,
                        int rows,
                        int cellW,
                        int cellH,
                        int insetX,
                        int insetY);

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
    bool debugStepping = false;

    template <typename T>
    static auto basic_dump_vars(T& e, int) -> decltype(e.dumpVars(std::cout), void()) {
        e.dumpVars(std::cout);
    }
    template <typename T>
    static auto basic_dump_vars(T& e, long) -> decltype(e.dump(std::cout), void()) {
        e.dump(std::cout);
    }
    static void basic_dump_vars(...) {
        std::cout << "(No variable dump available: implement Env::dumpVars(std::ostream&) or Env::dump(std::ostream&))\n";
    }

    bool debug_wait_key(ScopedRawInput& raw) {
        std::cout << "[DEBUG] SPACE=next, ESC=stop" << std::flush;
        if (!raw.active) {
            std::cout << " (press Enter for next, 'q' + Enter to stop)" << std::flush;
            std::string s;
            if (!std::getline(std::cin, s)) return false;
            if (!s.empty() && (s[0] == 'q' || s[0] == 'Q')) return false;
            return true;
        }
        while (true) {
            int ch = std::cin.get();
            if (ch == EOF) return false;
            if (ch == 27) { // ESC
                return false;
            }
            if (ch == ' ') {
                return true;
            }
            // ignore everything else
        }
    }

    void runDebugFromStart(ScopedRawInput& raw) {
        debugStepping = true;
        runFromStart();
        debugStepping = false;
        // After a debug run, consume any pending SIGINT so the next prompt is clean.
        g_sigint_requested.store(false, std::memory_order_relaxed);
        (void)raw;
    }
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

    void startRun() {
        g_sigint_requested.store(false, std::memory_order_relaxed);
        env.clearVars();
        env.running = true;
        env.stopped = false;
        env.pc = env.program.begin();
        env.posInLine = 0;
        env.dataCacheBuilt = false;   // or env.rebuildDataCache(env.program);
        env.restoreData(0, env.program);
        basic_reset_run_event_control(env);
    }

    void runFromStart() {
        startRun();
        execute();
    }

    void startCont() {
        if (!env.contAvailable) {
            std::cout << "Cannot CONTINUE\n";
            return;
        }
        g_sigint_requested.store(false, std::memory_order_relaxed);
        env.running = true;
        env.stopped = false;
    }

    void cont() {
        startCont();
        if (!env.running) return;
        execute();
    }

    void execute() {
        while (env.running && !env.stopped) {
            // SDL frontend: RUN blocks the main SDL loop, so we must pump events here to avoid beachball.
            {
                bool quitRequested = false;
                sdl_pump_events_during_run(quitRequested);
                if (quitRequested) {
                    env.running = false;
                    env.stopped = false;
                    env.contAvailable = false;
                    return;
                }
            }
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

            // DEBUG single-step: show current line + variables, then wait for SPACE/ESC.
            if (debugStepping) {
                int ln = env.pc->first;
                const std::string& full = env.pc->second;

                std::cout << "\n[DEBUG] Line " << ln << ": " << full << "\n";
                if (env.posInLine > 0 && env.posInLine < static_cast<int>(full.size())) {
                    std::cout << "[DEBUG] At: " << full.substr(static_cast<size_t>(env.posInLine)) << "\n";
                }
                std::cout << "[DEBUG] Variables:\n";
                basic_dump_vars(env, 0);

                ScopedRawInput raw; // ensure raw mode while stepping (if possible)
                if (!debug_wait_key(raw)) {
                    std::cout << "\n[DEBUG] Stopped\n";
                    env.running = false;
                    env.stopped = false;
                    env.contAvailable = false;
                    return;
                }
                std::cout << "\n";
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
                // Tiny yield during long runs so macOS stays responsive.
                if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) != 0) {
                    SDL_Delay(0);
                }
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

    struct SDLTerminalBuffer {
        struct Cell {
            char ch = ' ';
            uint8_t fg = 7; // default light gray
            uint8_t bg = 0; // black
        };

        int cols = 80;
        int rows = 25;
        int curRow = 0; // 0-based
        int curCol = 0; // 0-based
        bool cursorVisible = true;

        uint8_t curFg = 7;
        uint8_t curBg = 0;

        std::vector<Cell> grid;

        SDLTerminalBuffer() {
            grid.assign((size_t)(cols * rows), Cell{});
        }

        void clear() {
            std::fill(grid.begin(), grid.end(), Cell{});
            curRow = 0;
            curCol = 0;
        }

        void setColor(int fg, int bg) {
            if (fg >= 0) {
                fg = std::clamp(fg, 0, 15);
                curFg = (uint8_t)fg;
            }
            if (bg >= 0) {
                bg = std::clamp(bg, 0, 15);
                curBg = (uint8_t)bg;
            }
        }

        void showCursor(bool show) { cursorVisible = show; }

        void locate1(int row1, int col1) {
            int r = row1 - 1;
            int c = col1 - 1;
            r = std::clamp(r, 0, rows - 1);
            c = std::clamp(c, 0, cols - 1);
            curRow = r;
            curCol = c;
        }

        void scrollUp() {
            if (rows <= 1) return;
            std::memmove(&grid[0], &grid[(size_t)cols], (size_t)(cols * (rows - 1)) * sizeof(Cell));
            for (int c = 0; c < cols; ++c) {
                grid[(size_t)((rows - 1) * cols + c)] = Cell{};
            }
            if (curRow > 0) curRow--;
        }

        void newline() {
            curCol = 0;
            curRow++;
            if (curRow >= rows) {
                scrollUp();
                curRow = rows - 1;
            }
        }

        void putChar(char c) {
            if (c == '\r') {
                curCol = 0;
                return;
            }
            if (c == '\n') {
                newline();
                return;
            }
            if (c == '\t') {
                int next = ((curCol / 8) + 1) * 8;
                while (curCol < next) putChar(' ');
                return;
            }
            if ((unsigned char)c < 32) return; // ignore other control chars

            Cell &cell = grid[(size_t)(curRow * cols + curCol)];
            cell.ch = c;
            cell.fg = curFg;
            cell.bg = curBg;

            curCol++;
            if (curCol >= cols) newline();
        }

        void write(const std::string& s) {
            for (char c : s) putChar(c);
        }

        void pushLine(const std::string& s) {
            // Write text and ensure it ends with a newline.
            write(s);
            if (s.empty() || s.back() != '\n') putChar('\n');
        }
    };

    struct SDLTerminalStreamBuf : public std::streambuf {
        SDLTerminalBuffer* buf = nullptr;
        std::string pending;

        explicit SDLTerminalStreamBuf(SDLTerminalBuffer* b) : buf(b) {}

        int overflow(int ch) override {
            if (ch == EOF) return EOF;
            char c = (char)ch;
            if (c == '\n') {
                if (!pending.empty()) {
                    buf->write(pending);
                    pending.clear();
                }
                buf->putChar('\n');
            } else {
                pending.push_back(c);
                // Flush if buffer grows too much
                if (pending.size() > 4096) {
                    buf->write(pending);
                    pending.clear();
                }
            }
            return ch;
        }

        int sync() override {
            if (!pending.empty()) {
                buf->write(pending);
                pending.clear();
            }
            return 0;
        }
    };

    static inline bool sdl_try_open_font(TTF_Font*& font, int ptSize) {
        // macOS system monospace (usually exists)
        const char* candidates[] = {
            "/Users/emidio/Library/Fonts/MSX-Screen0.ttf",
            "/System/Library/Fonts/Menlo.ttc",
            "/System/Library/Fonts/Supplemental/Menlo.ttc",
            "/Library/Fonts/Menlo.ttc",
            "/System/Library/Fonts/Supplemental/Courier New.ttf",
            "/Library/Fonts/Courier New.ttf",
            nullptr
        };
        for (int i = 0; candidates[i]; ++i) {
            font = TTF_OpenFont(candidates[i], ptSize);
            if (font) return true;
        }
        return false;
    }

    static inline SDL_Texture* sdl_make_text_texture(SDL_Renderer* r, TTF_Font* font, const std::string& text, SDL_Color color, int& outW, int& outH) {
        outW = outH = 0;
        if (!font) return nullptr;
        // SDL_ttf expects UTF-8
        SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text.c_str(), color);
        if (!surf) return nullptr;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
        outW = surf->w;
        outH = surf->h;
        SDL_FreeSurface(surf);
        return tex;
    }
    
    static inline void sdl_pump_events_during_run(bool& requestQuit) {
        requestQuit = false;
        if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0) return;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                requestQuit = true;
                break;
            }
            // ignore everything else while BASIC is running
        }
    }

    void repl_sdl2_ttf() {
        install_basic_sigint_handler_once();

        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            std::cout << "SDL_Init failed: " << SDL_GetError() << "\n";
            std::cout << "Falling back to console REPL.\n";
            repl();
            return;
        }
        if (TTF_Init() != 0) {
            std::cout << "TTF_Init failed: " << TTF_GetError() << "\n";
            std::cout << "Falling back to console REPL.\n";
            SDL_Quit();
            repl();
            return;
        }

        SDL_Window* win = SDL_CreateWindow(
            "GW-BASIC",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            900, 540,
            SDL_WINDOW_ALLOW_HIGHDPI
        );
        if (!win) {
            std::cout << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
            std::cout << "Falling back to console REPL.\n";
            TTF_Quit();
            SDL_Quit();
            repl();
            return;
        }

        SDL_Renderer* renderer = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) {
            std::cout << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
            std::cout << "Falling back to console REPL.\n";
            SDL_DestroyWindow(win);
            TTF_Quit();
            SDL_Quit();
            repl();
            return;
        }

        // Font
        TTF_Font* font = nullptr;
        int ptSize = 24;
        if (!sdl_try_open_font(font, ptSize)) {
            std::cout << "Could not open a monospace font (Menlo). TTF error: " << TTF_GetError() << "\n";
            std::cout << "Falling back to console REPL.\n";
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(win);
            TTF_Quit();
            SDL_Quit();
            repl();
            return;
        }

        // Determine character cell size using a representative glyph
        int charW = 0, charH = 0;
        {
            int w = 0, h = 0;
            SDL_Color fg{255, 255, 255, 255};
            SDL_Texture* t = sdl_make_text_texture(renderer, font, "M", fg, w, h);
            if (t) SDL_DestroyTexture(t);

            // For monospace fonts, using the glyph advance yields a stable cell width.
            int minx = 0, maxx = 0, miny = 0, maxy = 0, advance = 0;
            if (TTF_GlyphMetrics(font, 'M', &minx, &maxx, &miny, &maxy, &advance) == 0 && advance > 0) {
                charW = advance;
            } else {
                charW = (w > 0 ? w : 10);
            }

            charH = TTF_FontLineSkip(font);
            if (charH <= 0) charH = (h > 0 ? h : 18);
        }

        // Fixed console geometry: 80x25 characters
        termCols = 80;
        termRows = 25;

        if (charW > 0 && charH > 0) {
            // Desired drawable (pixel) size based on font cell metrics.
            const int desiredPxW = termCols * charW;
            const int desiredPxH = termRows * charH;

            // On HiDPI displays, window size (points) != drawable size (pixels).
            int winW = 0, winH = 0;
            int outW = 0, outH = 0;
            SDL_GetWindowSize(win, &winW, &winH);
            SDL_GetRendererOutputSize(renderer, &outW, &outH);

            float scaleX = (winW > 0) ? ((float)outW / (float)winW) : 1.0f;
            float scaleY = (winH > 0) ? ((float)outH / (float)winH) : 1.0f;
            if (scaleX <= 0.0f) scaleX = 1.0f;
            if (scaleY <= 0.0f) scaleY = 1.0f;

            int desiredWinW = (int)lroundf((float)desiredPxW / scaleX);
            int desiredWinH = (int)lroundf((float)desiredPxH / scaleY);

            // Add extra margin for rounded window corners: +32pt overall (16pt inset each side).
            SDL_SetWindowSize(win, desiredWinW + 32, desiredWinH + 32);

            // Re-center after resizing for nicer UX.
            SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }

        // Padding for rounded corners: 16pt inset from top/left (convert to pixels for HiDPI).
        int winW_pts = 0, winH_pts = 0;
        int outW_px = 0, outH_px = 0;
        SDL_GetWindowSize(win, &winW_pts, &winH_pts);
        SDL_GetRendererOutputSize(renderer, &outW_px, &outH_px);

        float padScaleX = (winW_pts > 0) ? ((float)outW_px / (float)winW_pts) : 1.0f;
        float padScaleY = (winH_pts > 0) ? ((float)outH_px / (float)winH_pts) : 1.0f;
        if (padScaleX <= 0.0f) padScaleX = 1.0f;
        if (padScaleY <= 0.0f) padScaleY = 1.0f;

        const int insetX = (int)lroundf(16.0f * padScaleX);
        const int insetY = (int)lroundf(16.0f * padScaleY);

        SDLTerminalBuffer term;
        term.pushLine("GW-BASIC-like interpreter. Use RUN, LIST, EDIT, NEW, CLEAR, CONT, DELETE n, SAVE \"file\", LOAD \"file\", and QUIT.");

        // Wire BASIC screen driver to the SDL terminal buffer (real 80x25 grid with cursor + colors).
        env.screen.putChar = [&](char c) { term.putChar(c); };
        env.screen.cls = [&]() { term.clear(); };
        env.screen.locate = [&](int row, int col) { term.locate1(row, col); };
        env.screen.showCursor = [&](bool show) { term.showCursor(show); };
        env.screen.color = [&](int fg, int bg) {
            // In GW-BASIC, COLOR can omit either parameter (e.g. COLOR 2 or COLOR ,4).
            // The parser uses -1 to mean "unchanged"; preserve current colors here.
            int useFg = (fg < 0) ? term.curFg : fg;
            int useBg = (bg < 0) ? term.curBg : bg;
            term.setColor(useFg, useBg);
        };
        env.screen.beep = [&]() { /* no audio yet */ };

        // Redirect std::cout to the terminal buffer while this UI runs
        SDLTerminalStreamBuf sb(&term);
        std::streambuf* oldCout = std::cout.rdbuf(&sb);

        // REPL input state (typed directly into the grid, scrolling like a real console)
        std::string line;
        std::deque<std::string> history;
        std::string historyDraft;
        int historyIndex = -1;
        bool historyNav = false;

        int inputAnchorRow = term.curRow;
        int inputAnchorCol = term.curCol;

        auto putAt0 = [&](int r, int c, char ch) {
            if (r < 0 || c < 0 || r >= term.rows || c >= term.cols) return;
            auto &cell = term.grid[(size_t)(r * term.cols + c)];
            cell.ch = ch;
            cell.fg = term.curFg;
            cell.bg = term.curBg;
        };

        auto beginPrompt = [&]() {
            term.setColor(15, 0);
            term.write("OK> ");
            inputAnchorRow = term.curRow;
            inputAnchorCol = term.curCol;
            line.clear();
            historyNav = false;
            historyIndex = -1;
        };

        auto moveCursorToInputEnd = [&]() {
            int pos = inputAnchorCol + (int)line.size();
            int r = inputAnchorRow + (pos / term.cols);
            int c = (pos % term.cols);
            if (r >= term.rows) r = term.rows - 1;
            term.curRow = r;
            term.curCol = c;
        };

        auto eraseCurrentInput = [&]() {
            int pos0 = inputAnchorCol;
            int len = (int)line.size();
            for (int i = 0; i < len; ++i) {
                int pos = pos0 + i;
                int r = inputAnchorRow + (pos / term.cols);
                int c = (pos % term.cols);
                if (r >= term.rows) break;
                putAt0(r, c, ' ');
            }
        };

        auto redrawInput = [&](const std::string& newLine) {
            eraseCurrentInput();
            line = newLine;
            int pos0 = inputAnchorCol;
            for (size_t i = 0; i < line.size(); ++i) {
                int pos = pos0 + (int)i;
                int r = inputAnchorRow + (pos / term.cols);
                int c = (pos % term.cols);
                if (r >= term.rows) break;
                putAt0(r, c, line[i]);
            }
            moveCursorToInputEnd();
        };

        bool running = true;

        bool programRunning = false;

        auto finishProgramRun = [&]() {
            programRunning = false;
            // After a run finishes, show a fresh prompt on the next line.
            term.putChar('\n');
            beginPrompt();
        };

        auto executeStep = [&]() {
            if (!env.running || env.stopped) {
                finishProgramRun();
                return;
            }

            // Pump SDL events while running (avoid beachball). Only react to quit.
            {
                bool quitRequested = false;
                sdl_pump_events_during_run(quitRequested);
                if (quitRequested) {
                    env.running = false;
                    env.stopped = false;
                    env.contAvailable = false;
                    finishProgramRun();
                    running = false;
                    return;
                }
            }

            // Ctrl+C breaks execution and returns to the REPL.
            if (g_sigint_requested.exchange(false, std::memory_order_relaxed)) {
                std::cout << "\nBreak\n";
                env.running = false;
                env.stopped = false;
                env.contAvailable = true;
                finishProgramRun();
                return;
            }
            if (g_sigwinch_requested.exchange(false, std::memory_order_relaxed)) {
                basic_update_terminal_size(termCols, termRows);
            }

            if (env.pc == env.program.end()) {
                env.running = false;
                env.contAvailable = false;
                finishProgramRun();
                return;
            }

            int currentLineNumber = env.pc->first;
            std::string lineText = env.pc->second;

            std::string toParse;
            if (env.posInLine > 0 && env.posInLine < (int)lineText.size()) {
                toParse = lineText.substr((size_t)env.posInLine);
            } else {
                env.posInLine = 0;
                toParse = lineText;
            }

            Parser p(toParse, env);
            p.currentLine = lineText;
            p.linePosBase = env.posInLine;

            try {
                p.parseAndExecLine();
                env.pc++;
                env.posInLine = 0;

                // Tiny yield during long runs so the UI stays snappy.
                SDL_Delay(0);
            } catch (const RuntimeError& e) {
                if (std::string(e.what()) == "__JUMP__") {
                    return; // jump already updated env.pc/env.posInLine
                }
                std::cout << "Runtime error in " << currentLineNumber << ": " << e.what() << "\n";
                env.running = false;
                env.contAvailable = true;
                finishProgramRun();
                return;
            } catch (const ParseError& e) {
                std::cout << "Syntax error in " << currentLineNumber << ": " << e.what() << "\n";
                env.running = false;
                env.contAvailable = true;
                finishProgramRun();
                return;
            }
        };

        auto commitLine = [&](const std::string& raw) {
            std::string t = trim(raw);

            moveCursorToInputEnd();
            term.putChar('\n');

            if (t.empty()) { beginPrompt(); return; }

            if (history.empty() || history.back() != t) {
                history.push_back(t);
                if (history.size() > 64) history.pop_front();
            }

            // Program line
            if (std::isdigit((unsigned char)t[0])) {
                std::istringstream iss(t);
                int ln = 0;
                iss >> ln;
                if (ln <= 0) {
                    std::cout << "Bad line number\n";
                    beginPrompt();
                    return;
                }
                std::string rest;
                std::getline(iss, rest);
                rest = trim(rest);
                storeProgramLine(ln, rest);
                beginPrompt();
                return;
            }

            std::string upper;
            upper.reserve(t.size());
            for (char c : t) upper.push_back((char)std::toupper((unsigned char)c));

            if (upper == "RUN") {
                startRun();
                programRunning = true;
                return;
            }

            if (upper == "DEBUG") {
                std::cout << "DEBUG mode is not yet interactive in SDL frontend.\n";
                beginPrompt();
                return;
            }

            if (istartswith(upper, "LIST")) {
                cmd_LIST(trim(t.substr(4)));
                beginPrompt();
                return;
            }

            if (upper == "NEW") { cmd_NEW(); beginPrompt(); return; }
            if (upper == "CLEAR") { cmd_CLEAR(); beginPrompt(); return; }
            if (upper == "CONT") {
                startCont();
                if (env.running) programRunning = true;
                else beginPrompt();
                return;
            }

            if (upper == "QUIT" || upper == "EXIT") {
                std::cout << "Bye\n";
                running = false;
                return;
            }

            if (istartswith(upper, "SAVE")) {
                std::string rest = trim(t.substr(4));
                if (rest.empty() || rest[0] != '"') {
                    std::cout << "SAVE requires a filename in quotes\n";
                    beginPrompt();
                    return;
                }
                size_t endq = rest.find('"', 1);
                if (endq == std::string::npos) {
                    std::cout << "SAVE requires a filename in quotes\n";
                    beginPrompt();
                    return;
                }
                cmd_SAVE(rest.substr(1, endq - 1));
                beginPrompt();
                return;
            }

            if (istartswith(upper, "LOAD")) {
                std::string rest = trim(t.substr(4));
                if (rest.empty() || rest[0] != '"') {
                    std::cout << "LOAD requires a filename in quotes\n";
                    beginPrompt();
                    return;
                }
                size_t endq = rest.find('"', 1);
                if (endq == std::string::npos) {
                    std::cout << "LOAD requires a filename in quotes\n";
                    beginPrompt();
                    return;
                }

                std::string fn = rest.substr(1, endq - 1);

                bool runAfterLoad = false;
                std::string tail = trim(rest.substr(endq + 1));
                if (!tail.empty()) {
                    if (tail[0] != ',') {
                        std::cout << "LOAD: unexpected text after filename\n";
                        beginPrompt();
                        return;
                    }
                    tail = trim(tail.substr(1));
                    std::string tailUpper;
                    tailUpper.reserve(tail.size());
                    for (char c : tail) tailUpper.push_back((char)std::toupper((unsigned char)c));
                    if (tailUpper == "R") runAfterLoad = true;
                    else {
                        std::cout << "LOAD: unknown option '" << tail << "'\n";
                        beginPrompt();
                        return;
                    }
                }

                cmd_LOAD(fn);
                if (runAfterLoad) runFromStart();
                beginPrompt();
                return;
            }

            if (istartswith(upper, "DELETE")) {
                std::istringstream iss(t.substr(6));
                int ln = 0;
                iss >> ln;
                if (ln > 0) cmd_DELETE(ln);
                else std::cout << "DELETE requires line number\n";
                beginPrompt();
                return;
            }

            if (upper == "EDIT") {
                // Run the editor *inside the same SDL window/renderer* as the REPL.
                // Temporarily restore std::cout so any editor diagnostics go to the console.
                std::cout.rdbuf(oldCout);
                run_editor_inplace(env, win, renderer, font,
                                   termCols, termRows,
                                   charW, charH,
                                   insetX, insetY);
                std::cout.rdbuf(&sb);

                // The editor may stop SDL text input; ensure REPL can type again.
                SDL_StartTextInput();

                // Drop any stale input events (e.g., the ESC used to exit the editor).
                SDL_FlushEvent(SDL_TEXTINPUT);

                resetAfterProgramEdit();
                beginPrompt();
                return;
            }

            executeImmediate(t);
            beginPrompt();
        };

        SDL_StartTextInput();
        beginPrompt();

        while (running) {
            if (g_sigint_requested.exchange(false, std::memory_order_relaxed)) {
                term.pushLine("Break");
                beginPrompt();
            }

            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) {
                    running = false;
                    break;
                }
                if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    // Fixed 80x25 console; ignore resizes.
                    continue;
                }

                if (e.type == SDL_TEXTINPUT) {
                    if (programRunning) {
                        continue;
                    }
                    if (historyNav) { historyNav = false; historyIndex = -1; }
                    const char* t = e.text.text;
                    if (t) {
                        for (const char* p = t; *p; ++p) {
                            line.push_back(*p);
                            term.putChar(*p);
                        }
                    }
                    continue;
                }

                if (e.type == SDL_KEYDOWN) {
                    SDL_Keycode sym = e.key.keysym.sym;
                    SDL_Keymod mod = (SDL_Keymod)e.key.keysym.mod;

                    if (programRunning) {
                        // Allow ESC to break program execution.
                        if (sym == SDLK_ESCAPE) {
                            g_sigint_requested.store(true, std::memory_order_relaxed);
                        }
                        continue;
                    }

                    // Quit
                    if (sym == SDLK_ESCAPE) {
                        running = false;
                        break;
                    }

                    // RUN
                    if (sym == SDLK_F5) {
                        redrawInput("RUN");
                        commitLine("RUN");
                        continue;
                    }

                    // Clear screen-ish (Ctrl+L)
                    if ((mod & KMOD_CTRL) && (sym == SDLK_l)) {
                        term.clear();
                        term.pushLine("(cleared)");
                        beginPrompt();
                        continue;
                    }

                    // Backspace
                    if (sym == SDLK_BACKSPACE) {
                        if (historyNav) { historyNav = false; historyIndex = -1; }
                        if (!line.empty()) {
                            // Important: redrawInput() erases based on the *current* displayed length.
                            // So compute the new string first, then redraw (so erase uses the old length).
                            std::string newLine = line;
                            newLine.pop_back();
                            redrawInput(newLine);
                        }
                        continue;
                    }

                    // History UP/DOWN
                    if (sym == SDLK_UP) {
                        if (!history.empty()) {
                            if (!historyNav) {
                                historyNav = true;
                                historyDraft = line;
                                historyIndex = (int)history.size() - 1;
                            } else if (historyIndex > 0) {
                                historyIndex--;
                            }
                            redrawInput(history[(size_t)historyIndex]);
                        }
                        continue;
                    }
                    if (sym == SDLK_DOWN) {
                        if (historyNav) {
                            if (historyIndex < (int)history.size() - 1) {
                                historyIndex++;
                                redrawInput(history[(size_t)historyIndex]);
                            } else {
                                historyNav = false;
                                historyIndex = -1;
                                redrawInput(historyDraft);
                            }
                        }
                        continue;
                    }

                    // Enter
                    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                        commitLine(line);
                        continue;
                    }
                }
            }

            if (programRunning) {
                // Run a few steps per frame to keep UI responsive while executing.
                for (int i = 0; i < 200 && programRunning; ++i) {
                    executeStep();
                }
            }

            // Render
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);

            auto basicPalette = [&](uint8_t idx) -> SDL_Color {
                static const SDL_Color pal[16] = {
                    {0,0,0,255},{0,0,170,255},{0,170,0,255},{0,170,170,255},
                    {170,0,0,255},{170,0,170,255},{170,85,0,255},{170,170,170,255},
                    {85,85,85,255},{85,85,255,255},{85,255,85,255},{85,255,255,255},
                    {255,85,85,255},{255,85,255,255},{255,255,85,255},{255,255,255,255}
                };
                return pal[idx & 15];
            };

            const int cellW = charW;
            const int cellH = charH;

            // Draw background per run and render foreground text per run for each row.
            for (int r = 0; r < term.rows; ++r) {
                int c = 0;
                while (c < term.cols) {
                    const auto &cell0 = term.grid[(size_t)(r * term.cols + c)];
                    uint8_t fg = cell0.fg;
                    uint8_t bg = cell0.bg;

                    int cStart = c;
                    std::string run;
                    run.reserve((size_t)term.cols);
                    while (c < term.cols) {
                        const auto &cell = term.grid[(size_t)(r * term.cols + c)];
                        if (cell.fg != fg || cell.bg != bg) break;
                        run.push_back(cell.ch ? cell.ch : ' ');
                        ++c;
                    }

                    // Background rect for run
                    SDL_Color bgc = basicPalette(bg);
                    SDL_SetRenderDrawColor(renderer, bgc.r, bgc.g, bgc.b, bgc.a);
                    SDL_Rect bgRect{ insetX + cStart * cellW, insetY + r * cellH, (c - cStart) * cellW, cellH };
                    SDL_RenderFillRect(renderer, &bgRect);

                    // Foreground text for run (skip if all spaces)
                    bool allSpace = true;
                    for (char ch : run) { if (ch != ' ') { allSpace = false; break; } }
                    if (!allSpace) {
                        SDL_Color fgc = basicPalette(fg);
                        int tw = 0, th = 0;
                        SDL_Texture* tex = sdl_make_text_texture(renderer, font, run, fgc, tw, th);
                        if (tex) {
                            SDL_Rect dst{ insetX + cStart * cellW, insetY + r * cellH, tw, th };
                            SDL_RenderCopy(renderer, tex, nullptr, &dst);
                            SDL_DestroyTexture(tex);
                        }
                    }
                }
            }

            // Draw BASIC cursor (outline) at term cursor position if visible.
            if (term.cursorVisible) {
                SDL_Color cc = basicPalette(term.curFg);
                SDL_SetRenderDrawColor(renderer, cc.r, cc.g, cc.b, 255);
                SDL_Rect curRect{ insetX + term.curCol * cellW, insetY + term.curRow * cellH, cellW, cellH };
                SDL_RenderDrawRect(renderer, &curRect);
            }

            SDL_RenderPresent(renderer);
        }

        SDL_StopTextInput();

        // Detach SDL screen driver hooks
        env.screen = {};

        // Restore cout
        std::cout.rdbuf(oldCout);

        TTF_CloseFont(font);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(win);
        TTF_Quit();
        SDL_Quit();
    }

    void repl() {
        // Tip: define BASIC_USE_SDL and call repl_sdl2_ttf() instead of repl() to run the graphics frontend.
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
            if (upper == "DEBUG") {
                // Step through the program one statement at a time.
                // SPACE advances; ESC stops.
                runDebugFromStart(raw);
                continue;
            }
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
