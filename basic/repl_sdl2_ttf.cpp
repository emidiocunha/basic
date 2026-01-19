//
//  repl_sdl2_ttf.cpp
//  basic
//
//  Created by Emídio Cunha on 15/01/2026.
//

#include <iostream>
#include <deque>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <thread>
#include <mutex>
#include <atomic>

#if defined(__APPLE__)
#include <pthread.h>
#endif

#include "SDL.h"
#include "SDL_ttf.h"

#include "interpreter.h"

namespace {

// --- Moved helpers from interpreter.h ---

struct SDLTerminalBuffer {
    struct Cell {
        char ch = ' ';
        uint8_t fg = 7;
        uint8_t bg = 0;
    };

    int cols = 80;
    int rows = 25;
    int curRow = 0;
    int curCol = 0;
    bool cursorVisible = true;

    uint8_t curFg = 7;
    uint8_t curBg = 0;

    std::vector<Cell> grid;

    SDLTerminalBuffer() { grid.assign((size_t)(cols * rows), Cell{}); }

    void clear() {
        Cell blank;
        blank.ch = ' ';
        blank.fg = curFg;
        blank.bg = curBg;

        std::fill(grid.begin(), grid.end(), blank);
        curRow = 0;
        curCol = 0;
    }

    void setColor(int fg, int bg) {
        if (fg >= 0) { fg = std::clamp(fg, 0, 15); curFg = (uint8_t)fg; }
        if (bg >= 0) { bg = std::clamp(bg, 0, 15); curBg = (uint8_t)bg; }
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
        std::memmove(&grid[0], &grid[(size_t)cols],
                     (size_t)(cols * (rows - 1)) * sizeof(Cell));
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
        if (c == '\r') { curCol = 0; return; }
        if (c == '\n') { newline(); return; }
        if (c == '\t') {
            int next = ((curCol / 8) + 1) * 8;
            while (curCol < next) putChar(' ');
            return;
        }
        if ((unsigned char)c < 32) return;

        Cell& cell = grid[(size_t)(curRow * cols + curCol)];
        cell.ch = c;
        cell.fg = curFg;
        cell.bg = curBg;

        curCol++;
        if (curCol >= cols) newline();
    }

    void write(const std::string& s) { for (char c : s) putChar(c); }

    void pushLine(const std::string& s) {
        write(s);
        if (s.empty() || s.back() != '\n') putChar('\n');
    }
};

struct SDLTerminalStreamBuf : public std::streambuf {
    SDLTerminalBuffer* buf = nullptr;
    std::mutex* mtx = nullptr;
    std::string pending;

    SDLTerminalStreamBuf(SDLTerminalBuffer* b, std::mutex* m) : buf(b), mtx(m) {}

    int overflow(int ch) override {
        if (ch == EOF) return EOF;
        char c = (char)ch;
        if (c == '\n') {
            std::lock_guard<std::mutex> lock(*mtx);
            if (!pending.empty()) {
                buf->write(pending);
                pending.clear();
            }
            buf->putChar('\n');
        } else {
            pending.push_back(c);
            if (pending.size() > 4096) {
                std::lock_guard<std::mutex> lock(*mtx);
                buf->write(pending);
                pending.clear();
            }
        }
        return ch;
    }

    int sync() override {
        if (!pending.empty()) {
            std::lock_guard<std::mutex> lock(*mtx);
            buf->write(pending);
            pending.clear();
        }
        return 0;
    }
};

static bool sdl_try_open_font(TTF_Font*& font, int ptSize) {
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

static SDL_Texture* sdl_make_text_texture(SDL_Renderer* r,
                                         TTF_Font* font,
                                         const std::string& text,
                                         SDL_Color color,
                                         int& outW,
                                         int& outH) {
    outW = outH = 0;
    if (!font) return nullptr;
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
    outW = surf->w;
    outH = surf->h;
    SDL_FreeSurface(surf);
    return tex;
}

} // namespace

// --- The method moved out of header ---
void Interpreter::repl_sdl2_ttf() {
    install_basic_sigint_handler_once();

#if defined(__APPLE__)
    // SDL's Cocoa backend requires event pumping on the main thread.
    // If this REPL is invoked from a worker thread, Cocoa will raise:
    // "nextEventMatchingMask should only be called from the Main Thread!"
    if (pthread_main_np() == 0) {
        std::cout << "SDL REPL must run on the main thread on macOS. Falling back to console REPL.\n";
        repl();
        return;
    }
#endif

    // Ensure the SDL window is raised/activated on creation (macOS often keeps focus in Terminal).
    SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "1");

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
    sdl_ui_active_flag().store(true, std::memory_order_relaxed);

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

    // Try to bring the new window to the front and give it input focus.
    SDL_ShowWindow(win);
    SDL_RaiseWindow(win);
#if SDL_VERSION_ATLEAST(2,0,5)
    (void)SDL_SetWindowInputFocus(win);
#endif
    SDL_PumpEvents();

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

    // Raising again after renderer creation can help on some macOS setups.
    SDL_RaiseWindow(win);
#if SDL_VERSION_ATLEAST(2,0,5)
    (void)SDL_SetWindowInputFocus(win);
#endif
    SDL_PumpEvents();

    // Fixed console geometry: 80x25 characters
    termCols = 80;
    termRows = 25;

    // Determine renderer pixel size (important on HiDPI) and choose padding.
    int winW_pts = 0, winH_pts = 0, outW_px = 0, outH_px = 0;
    SDL_GetWindowSize(win, &winW_pts, &winH_pts);
    SDL_GetRendererOutputSize(renderer, &outW_px, &outH_px);

    float padScaleX = (winW_pts > 0) ? ((float)outW_px / (float)winW_pts) : 1.0f;
    float padScaleY = (winH_pts > 0) ? ((float)outH_px / (float)winH_pts) : 1.0f;
    if (padScaleX <= 0.0f) padScaleX = 1.0f;
    if (padScaleY <= 0.0f) padScaleY = 1.0f;

    int insetX = (int)lroundf(16.0f * padScaleX);
    int insetY = (int)lroundf(16.0f * padScaleY);

    // Font: pick the largest point size that still fits 80x25 in the available pixel area.
    TTF_Font* font = nullptr;
    int ptSize = 24;
    int charW = 0, charH = 0;

    auto measure_cell = [&](TTF_Font* f, int& outCharW, int& outCharH) {
        outCharW = 0;
        outCharH = 0;
        if (!f) return;

        int minx=0,maxx=0,miny=0,maxy=0,advance=0;
        if (TTF_GlyphMetrics(f, 'M', &minx,&maxx,&miny,&maxy,&advance) == 0 && advance > 0) {
            outCharW = advance;
        } else {
            // Fallback if metrics fail: render a single glyph.
            int w = 0, h = 0;
            SDL_Color fg{255,255,255,255};
            SDL_Texture* t = sdl_make_text_texture(renderer, f, "M", fg, w, h);
            if (t) SDL_DestroyTexture(t);
            outCharW = (w > 0 ? w : 10);
            outCharH = (h > 0 ? h : 18);
        }

        int ls = TTF_FontLineSkip(f);
        outCharH = (ls > 0) ? ls : (outCharH > 0 ? outCharH : 18);
        if (outCharW <= 0) outCharW = 10;
        if (outCharH <= 0) outCharH = 18;
    };

    auto open_best_font = [&]() -> bool {
        const int availW = std::max(0, outW_px - insetX * 2);
        const int availH = std::max(0, outH_px - insetY * 2);

        // Search a reasonable point-size range.
        int bestPt = -1;
        int bestCW = 0;
        int bestCH = 0;
        TTF_Font* bestFont = nullptr;

        // Start from small to large so we end on the biggest that fits.
        for (int candidate = 8; candidate <= 96; ++candidate) {
            TTF_Font* f = nullptr;
            if (!sdl_try_open_font(f, candidate)) {
                // If we can't open at this size, try next.
                continue;
            }

            int cw = 0, ch = 0;
            measure_cell(f, cw, ch);

            const int needW = termCols * cw;
            const int needH = termRows * ch;

            if (needW <= availW && needH <= availH) {
                // Candidate fits, keep it.
                if (bestFont) TTF_CloseFont(bestFont);
                bestFont = f;
                bestPt = candidate;
                bestCW = cw;
                bestCH = ch;
            } else {
                // Doesn't fit, discard.
                TTF_CloseFont(f);
            }
        }

        if (!bestFont) return false;

        font = bestFont;
        ptSize = bestPt;
        charW = bestCW;
        charH = bestCH;
        return true;
    };

    // --- Fullscreen/windowed helpers ---

    auto open_fixed_font = [&](int fixedPt) -> bool {
        if (font) { TTF_CloseFont(font); font = nullptr; }
        if (!sdl_try_open_font(font, fixedPt)) return false;
        ptSize = fixedPt;
        measure_cell(font, charW, charH);
        return true;
    };

    auto recompute_insets = [&]() {
        SDL_GetWindowSize(win, &winW_pts, &winH_pts);
        SDL_GetRendererOutputSize(renderer, &outW_px, &outH_px);

        padScaleX = (winW_pts > 0) ? ((float)outW_px / (float)winW_pts) : 1.0f;
        padScaleY = (winH_pts > 0) ? ((float)outH_px / (float)winH_pts) : 1.0f;
        if (padScaleX <= 0.0f) padScaleX = 1.0f;
        if (padScaleY <= 0.0f) padScaleY = 1.0f;

        insetX = (int)lroundf(16.0f * padScaleX);
        insetY = (int)lroundf(16.0f * padScaleY);
    };

    auto size_window_for_80x25 = [&]() {
        // Size the *window points* so the *renderer output pixels* have enough room for 80x25 + insets.
        SDL_GetWindowSize(win, &winW_pts, &winH_pts);
        SDL_GetRendererOutputSize(renderer, &outW_px, &outH_px);

        float scaleX = (winW_pts > 0) ? ((float)outW_px / (float)winW_pts) : 1.0f;
        float scaleY = (winH_pts > 0) ? ((float)outH_px / (float)winH_pts) : 1.0f;
        if (scaleX <= 0.0f) scaleX = 1.0f;
        if (scaleY <= 0.0f) scaleY = 1.0f;

        // Insets in output pixels.
        int insetPxX = (int)lroundf(16.0f * scaleX);
        int insetPxY = (int)lroundf(16.0f * scaleY);

        const int desiredOutW = insetPxX * 2 + termCols * charW;
        const int desiredOutH = insetPxY * 2 + termRows * charH;

        int desiredWinW = (int)lroundf((float)desiredOutW / scaleX);
        int desiredWinH = (int)lroundf((float)desiredOutH / scaleY);

        SDL_SetWindowSize(win, desiredWinW, desiredWinH);
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

        // Recompute insets based on final sizes.
        recompute_insets();
    };

    bool isFullscreen = true;
    bool applyingDisplayMode = false;

    auto apply_display_mode = [&](bool fullscreen) -> bool {
        if (applyingDisplayMode) return true;
        applyingDisplayMode = true;
        isFullscreen = fullscreen;

        if (fullscreen) {
            if (SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) {
                std::cout << "SDL_SetWindowFullscreen failed: " << SDL_GetError() << "\n";
                applyingDisplayMode = false;
                return false;
            }
            recompute_insets();

            // Re-pick the best font size for the current fullscreen output.
            if (!open_best_font()) {
                applyingDisplayMode = false;
                return false;
            }
            // Insets depend on output/window scale; recompute after any changes.
            recompute_insets();
            applyingDisplayMode = false;
            return true;
        }

        // Windowed mode: fixed 18pt font and window sized for exactly 80x25.
        if (SDL_SetWindowFullscreen(win, 0) != 0) {
            std::cout << "SDL_SetWindowFullscreen(off) failed: " << SDL_GetError() << "\n";
            applyingDisplayMode = false;
            return false;
        }
        if (!open_fixed_font(18)) {
            std::cout << "Could not open a monospace font at 18pt. TTF error: " << TTF_GetError() << "\n";
            applyingDisplayMode = false;
            return false;
        }

        // First recompute insets, then size window appropriately.
        recompute_insets();
        size_window_for_80x25();
        applyingDisplayMode = false;
        return true;
    };

    if (!open_best_font()) {
        // Fall back to a fixed size if the search failed.
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
        measure_cell(font, charW, charH);
    }

    // Start in fullscreen mode by default.
    if (!apply_display_mode(true)) {
        std::cout << "Failed to apply initial display mode. Falling back to console REPL.\n";
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(win);
        TTF_Quit();
        SDL_Quit();
        repl();
        return;
    }


    SDLTerminalBuffer term;
    term.pushLine("GW-BASIC-like interpreter. Use RUN, LIST, EDIT, NEW, CLEAR, CONT, DELETE n, SAVE \"file\", LOAD \"file\", and QUIT.");

    std::mutex termMutex;

    // Wire BASIC screen driver to the SDL terminal buffer, locking for thread safety
    env.screen.putChar = [&](char c) { std::lock_guard<std::mutex> lock(termMutex); term.putChar(c); };
    env.screen.cls = [&]() { std::lock_guard<std::mutex> lock(termMutex); term.clear(); };
    env.screen.locate = [&](int row, int col) { std::lock_guard<std::mutex> lock(termMutex); term.locate1(row, col); };
    env.screen.showCursor = [&](bool show) { std::lock_guard<std::mutex> lock(termMutex); term.showCursor(show); };
    env.screen.color = [&](int fg, int bg) {
        std::lock_guard<std::mutex> lock(termMutex);
        int useFg = (fg < 0) ? term.curFg : fg;
        int useBg = (bg < 0) ? term.curBg : bg;
        term.setColor(useFg, useBg);
    };
    env.screen.beep = [&]() { };

    SDLTerminalStreamBuf sb(&term, &termMutex);
    std::streambuf* oldCout = std::cout.rdbuf(&sb);

    // REPL input state
    std::string line;
    std::deque<std::string> history;
    std::string historyDraft;
    int historyIndex = -1;
    bool historyNav = false;

    int inputAnchorRow = term.curRow;
    int inputAnchorCol = term.curCol;

    // Program INPUT capture (while a BASIC program is waiting for INPUT)
    std::string programInput;
    int programInputAnchorRow = 0;
    int programInputAnchorCol = 0;
    bool programInputActive = false;

    auto putAt0 = [&](int r, int c, char ch) {
        if (r < 0 || c < 0 || r >= term.rows || c >= term.cols) return;
        auto& cell = term.grid[(size_t)(r * term.cols + c)];
        cell.ch = ch;
        cell.fg = term.curFg;
        cell.bg = term.curBg;
    };

    auto beginPrompt = [&]() {
        std::lock_guard<std::mutex> lock(termMutex);
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
        std::lock_guard<std::mutex> lock(termMutex);
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

    auto beginProgramInput = [&]() {
        std::lock_guard<std::mutex> lock(termMutex);
        programInputActive = true;
        programInput.clear();
        programInputAnchorRow = term.curRow;
        programInputAnchorCol = term.curCol;
    };

    auto eraseProgramInput = [&]() {
        int pos0 = programInputAnchorCol;
        int len = (int)programInput.size();
        for (int i = 0; i < len; ++i) {
            int pos = pos0 + i;
            int r = programInputAnchorRow + (pos / term.cols);
            int c = (pos % term.cols);
            if (r >= term.rows) break;
            putAt0(r, c, ' ');
        }
    };

    auto redrawProgramInput = [&](const std::string& newLine) {
        std::lock_guard<std::mutex> lock(termMutex);
        eraseProgramInput();
        programInput = newLine;
        int pos0 = programInputAnchorCol;
        for (size_t i = 0; i < programInput.size(); ++i) {
            int pos = pos0 + (int)i;
            int r = programInputAnchorRow + (pos / term.cols);
            int c = (pos % term.cols);
            if (r >= term.rows) break;
            putAt0(r, c, programInput[i]);
        }
        // Move cursor to end
        int pos = programInputAnchorCol + (int)programInput.size();
        int r = programInputAnchorRow + (pos / term.cols);
        int c = (pos % term.cols);
        if (r >= term.rows) r = term.rows - 1;
        term.curRow = r;
        term.curCol = c;
    };

    bool running = true;

    bool programRunning = false;
    bool sdlDebugPaused = false;
    bool sdlDebugNeedPrint = false;
    std::thread programThread;
    std::atomic<bool> programDone{false};

    auto finishProgramRun = [&]() {
        programRunning = false;
        sdlDebugPaused = false;
        sdlDebugNeedPrint = false;
        debugStepping = false;
        programInputActive = false;
        programInput.clear();
        {
            std::lock_guard<std::mutex> lock(termMutex);
            term.putChar('\n');
        }
        beginPrompt();
    };

    auto executeStep = [&]() {
        if (!env.running || env.stopped) {
            finishProgramRun();
            return;
        }

        if (debugStepping) {
            if (sdlDebugNeedPrint) {
                if (env.pc != env.program.end()) {
                    int ln = env.pc->first;
                    const std::string& full = env.pc->second;

                    std::cout << "\n[DEBUG] Line " << ln << ": " << full << "\n";
                    if (env.posInLine > 0 && env.posInLine < (int)full.size()) {
                        std::cout << "[DEBUG] At: " << full.substr((size_t)env.posInLine) << "\n";
                    }
                    std::cout << "[DEBUG] Variables:\n";
                    basic_dump_vars(env, 0);
                    std::cout << "[DEBUG] SPACE=next, ESC=stop\n";
                }
                sdlDebugNeedPrint = false;
                sdlDebugPaused = true;
                return;
            }
            if (sdlDebugPaused) return;
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

        if (env.pc == env.program.end()) {
            env.running = false;
            env.contAvailable = false;
            finishProgramRun();
            return;
        }

        int currentLineNumber = env.pc->first;
        std::string lineText = env.pc->second;

        std::string toParse;
        if (env.posInLine > 0 && env.posInLine < (int)lineText.size())
            toParse = lineText.substr((size_t)env.posInLine);
        else { env.posInLine = 0; toParse = lineText; }

        Parser p(toParse, env);
        p.currentLine = lineText;
        p.linePosBase = env.posInLine;

        try {
            p.parseAndExecLine();
            env.pc++;
            env.posInLine = 0;

            SDL_Delay(0);

            if (debugStepping) {
                sdlDebugNeedPrint = true;
                sdlDebugPaused = false;
                return;
            }
        } catch (const RuntimeError& e) {
            if (std::string(e.what()) == "__JUMP__") {
                if (debugStepping) { sdlDebugNeedPrint = true; sdlDebugPaused = false; }
                return;
            }
            std::cout << "Runtime error in " << currentLineNumber << ": " << e.what() << "\n";
            env.running = false;
            env.contAvailable = true;
            finishProgramRun();
        } catch (const ParseError& e) {
            std::cout << "Syntax error in " << currentLineNumber << ": " << e.what() << "\n";
            env.running = false;
            env.contAvailable = true;
            finishProgramRun();
        }
    };

    auto commitLine = [&](const std::string& raw) {
        std::string t = trim(raw);

        moveCursorToInputEnd();
        {
            std::lock_guard<std::mutex> lock(termMutex);
            term.putChar('\n');
        }

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
            if (ln <= 0) { std::cout << "Bad line number\n"; beginPrompt(); return; }
            std::string rest;
            std::getline(iss, rest);
            storeProgramLine(ln, trim(rest));
            beginPrompt();
            return;
        }

        std::string upper;
        upper.reserve(t.size());
        for (char c : t) upper.push_back((char)std::toupper((unsigned char)c));

        if (upper == "RUN") {
            startRun();
            debugStepping = false;
            programDone.store(false, std::memory_order_relaxed);
            programRunning = true;
            if (programThread.joinable()) programThread.join();
            programThread = std::thread([&]() {
                this->execute();
                programDone.store(true, std::memory_order_relaxed);
            });
            return;
        }

        if (upper == "DEBUG") {
            startRun();
            debugStepping = true;
            sdlDebugPaused = false;
            sdlDebugNeedPrint = true;
            programRunning = true;
            return;
        }

        if (istartswith(upper, "LIST")) { cmd_LIST(trim(t.substr(4))); beginPrompt(); return; }
        if (upper == "NEW") { cmd_NEW(); beginPrompt(); return; }
        if (upper == "CLEAR") { cmd_CLEAR(); beginPrompt(); return; }

        if (upper == "CONT") {
            startCont();
            if (env.running) {
                debugStepping = false;
                programDone.store(false, std::memory_order_relaxed);
                programRunning = true;
                if (programThread.joinable()) programThread.join();
                programThread = std::thread([&]() {
                    this->execute();
                    programDone.store(true, std::memory_order_relaxed);
                });
            } else {
                beginPrompt();
            }
            return;
        }

        if (upper == "QUIT" || upper == "EXIT") { std::cout << "Bye\n"; running = false; return; }

        if (istartswith(upper, "SAVE")) {
            std::string rest = trim(t.substr(4));
            if (rest.empty() || rest[0] != '"') { std::cout << "SAVE requires a filename in quotes\n"; beginPrompt(); return; }
            size_t endq = rest.find('"', 1);
            if (endq == std::string::npos) { std::cout << "SAVE requires a filename in quotes\n"; beginPrompt(); return; }
            cmd_SAVE(rest.substr(1, endq - 1));
            beginPrompt();
            return;
        }

        if (istartswith(upper, "LOAD")) {
            std::string rest = trim(t.substr(4));
            if (rest.empty() || rest[0] != '"') { std::cout << "LOAD requires a filename in quotes\n"; beginPrompt(); return; }
            size_t endq = rest.find('"', 1);
            if (endq == std::string::npos) { std::cout << "LOAD requires a filename in quotes\n"; beginPrompt(); return; }

            std::string fn = rest.substr(1, endq - 1);

            bool runAfterLoad = false;
            std::string tail = trim(rest.substr(endq + 1));
            if (!tail.empty()) {
                if (tail[0] != ',') { std::cout << "LOAD: unexpected text after filename\n"; beginPrompt(); return; }
                tail = trim(tail.substr(1));
                std::string tailUpper;
                tailUpper.reserve(tail.size());
                for (char c : tail) tailUpper.push_back((char)std::toupper((unsigned char)c));
                if (tailUpper == "R" || tailUpper == "RUN") runAfterLoad = true;
                else { std::cout << "LOAD: unknown option '" << tail << "'\n"; beginPrompt(); return; }
            }

            cmd_LOAD(fn);
            if (runAfterLoad) {
                startRun();
                debugStepping = false;
                programDone.store(false, std::memory_order_relaxed);
                programRunning = true;
                if (programThread.joinable()) programThread.join();
                programThread = std::thread([&]() {
                    this->execute();
                    programDone.store(true, std::memory_order_relaxed);
                });
                return;
            }
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
            // NOTE: editor currently expects SDL renderer + TTF font.
            // You can migrate editor later to OpenGL; for now we keep it here.
            std::cout.rdbuf(oldCout);
            run_editor_inplace(env, win, renderer, font,
                               termCols, termRows,
                               charW, charH,
                               insetX, insetY);
            std::cout.rdbuf(&sb);

            SDL_StartTextInput();
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
            if (e.type == SDL_QUIT) { running = false; break; }
            if (e.type == SDL_WINDOWEVENT) {
                if (applyingDisplayMode) continue;

                // If the user toggles fullscreen via the title-bar green button,
                // SDL will update window flags; sync our mode and re-fit fonts/sizing.
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    e.window.event == SDL_WINDOWEVENT_MAXIMIZED ||
                    e.window.event == SDL_WINDOWEVENT_RESTORED) {

                    const Uint32 flags = SDL_GetWindowFlags(win);
                    const bool nowFullscreen = (flags & SDL_WINDOW_FULLSCREEN) || (flags & SDL_WINDOW_FULLSCREEN_DESKTOP);

                    if (nowFullscreen != isFullscreen) {
                        (void)apply_display_mode(nowFullscreen);
                    } else if (!isFullscreen) {
                        // Windowed mode is fixed to 80x25; if the OS resized the window (zoom button), snap back.
                        size_window_for_80x25();
                    } else {
                        // Fullscreen: recompute insets to keep rendering aligned.
                        recompute_insets();
                    }
                }
                continue;
            }

            if (e.type == SDL_TEXTINPUT) {
                if (programRunning) {
                    if (!sdl_waiting_input_flag().load(std::memory_order_relaxed)) continue;
                    if (!programInputActive) beginProgramInput();
                    const char* t = e.text.text;
                    if (t) {
                        std::lock_guard<std::mutex> lock(termMutex);
                        for (const char* p = t; *p; ++p) {
                            programInput.push_back(*p);
                            term.putChar(*p);
                        }
                    }
                    continue;
                }
                if (historyNav) { historyNav = false; historyIndex = -1; }
                const char* t = e.text.text;
                if (t) {
                    std::lock_guard<std::mutex> lock(termMutex);
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
                    // If BASIC is waiting for INPUT, capture typing and deliver it to the worker thread.
                    if (sdl_waiting_input_flag().load(std::memory_order_relaxed)) {
                        if (!programInputActive) beginProgramInput();

                        if (sym == SDLK_BACKSPACE) {
                            if (!programInput.empty()) {
                                std::string nl = programInput;
                                nl.pop_back();
                                redrawProgramInput(nl);
                            }
                            continue;
                        }

                        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                            sdl_post_input_line(programInput);
                            programInputActive = false;
                            {
                                std::lock_guard<std::mutex> lock(termMutex);
                                term.putChar('\n');
                            }
                            continue;
                        }
                    }

                    // Debug stepping keys
                    if (debugStepping) {
                        if (sym == SDLK_SPACE) sdlDebugPaused = false;
                        else if (sym == SDLK_ESCAPE) { env.running=false; env.stopped=false; env.contAvailable=false; finishProgramRun(); }
                        continue;
                    }

                    if (sym == SDLK_ESCAPE) g_sigint_requested.store(true, std::memory_order_relaxed);
                    continue;
                }

                if (sym == SDLK_ESCAPE) { running = false; break; }

                // Toggle fullscreen/windowed.
                // Note: on macOS F11 is often captured by the system (Show Desktop), so provide alternatives.
                const bool toggleFullscreen =
                    (sym == SDLK_F11) ||
                    ((mod & KMOD_GUI) && (sym == SDLK_f)) ||
                    ((mod & KMOD_ALT) && (sym == SDLK_RETURN || sym == SDLK_KP_ENTER));

                if (toggleFullscreen) {
                    if (!apply_display_mode(!isFullscreen)) {
                        std::cout << "Display mode toggle failed.\n";
                    }
                    continue;
                }

                if (sym == SDLK_F5) { redrawInput("RUN"); commitLine("RUN"); continue; }

                if ((mod & KMOD_CTRL) && (sym == SDLK_l)) {
                    std::lock_guard<std::mutex> lock(termMutex);
                    term.clear();
                    term.pushLine("(cleared)");
                    beginPrompt();
                    continue;
                }

                if (sym == SDLK_BACKSPACE) {
                    if (historyNav) { historyNav = false; historyIndex = -1; }
                    if (!line.empty()) { std::string newLine = line; newLine.pop_back(); redrawInput(newLine); }
                    continue;
                }

                if (sym == SDLK_UP) {
                    if (!history.empty()) {
                        if (!historyNav) { historyNav = true; historyDraft = line; historyIndex = (int)history.size() - 1; }
                        else if (historyIndex > 0) historyIndex--;
                        redrawInput(history[(size_t)historyIndex]);
                    }
                    continue;
                }

                if (sym == SDLK_DOWN) {
                    if (historyNav) {
                        if (historyIndex < (int)history.size() - 1) { historyIndex++; redrawInput(history[(size_t)historyIndex]); }
                        else { historyNav=false; historyIndex=-1; redrawInput(historyDraft); }
                    }
                    continue;
                }

                if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) { commitLine(line); continue; }
            }
        }

        // If a background run finished, join and return to prompt.
        if (programRunning && !debugStepping && programDone.load(std::memory_order_relaxed)) {
            if (programThread.joinable()) programThread.join();
            finishProgramRun();
        }

        // DEBUG still runs step-by-step on the UI thread.
        if (programRunning && debugStepping) {
            for (int i = 0; i < 200 && programRunning; ++i) executeStep();
        }

        // Render
        auto basicPalette = [&](uint8_t idx) -> SDL_Color {
            static const SDL_Color pal[16] = {
                {0,0,0,255},{0,0,170,255},{0,170,0,255},{0,170,170,255},
                {170,0,0,255},{170,0,170,255},{170,85,0,255},{170,170,170,255},
                {85,85,85,255},{85,85,255,255},{85,255,85,255},{85,255,255,255},
                {255,85,85,255},{255,85,255,255},{255,255,85,255},{255,255,255,255}
            };
            return pal[idx & 15];
        };

        // Clear with actual background (COLOR bg)
        {
            std::lock_guard<std::mutex> lock(termMutex);
            SDL_Color bgc = basicPalette(term.curBg);
            SDL_SetRenderDrawColor(renderer, bgc.r, bgc.g, bgc.b, 255);
            SDL_RenderClear(renderer);
        }

        const int cellW = charW;
        const int cellH = charH;

        std::lock_guard<std::mutex> lock(termMutex);

        for (int r = 0; r < term.rows; ++r) {
            int c = 0;
            while (c < term.cols) {
                const auto& cell0 = term.grid[(size_t)(r * term.cols + c)];
                uint8_t fg = cell0.fg;
                uint8_t bg = cell0.bg;

                int cStart = c;
                std::string run;
                run.reserve((size_t)term.cols);

                while (c < term.cols) {
                    const auto& cell = term.grid[(size_t)(r * term.cols + c)];
                    if (cell.fg != fg || cell.bg != bg) break;
                    run.push_back(cell.ch ? cell.ch : ' ');
                    ++c;
                }

                SDL_Color bgc = basicPalette(bg);
                SDL_SetRenderDrawColor(renderer, bgc.r, bgc.g, bgc.b, bgc.a);
                SDL_Rect bgRect{ insetX + cStart*cellW, insetY + r*cellH, (c - cStart)*cellW, cellH };
                SDL_RenderFillRect(renderer, &bgRect);

                bool allSpace = true;
                for (char ch : run) { if (ch != ' ') { allSpace = false; break; } }

                if (!allSpace) {
                    SDL_Color fgc = basicPalette(fg);
                    int tw=0, th=0;
                    SDL_Texture* tex = sdl_make_text_texture(renderer, font, run, fgc, tw, th);
                    if (tex) {
                        SDL_Rect dst{ insetX + cStart*cellW, insetY + r*cellH, tw, th };
                        SDL_RenderCopy(renderer, tex, nullptr, &dst);
                        SDL_DestroyTexture(tex);
                    }
                }
            }
        }

        if (term.cursorVisible) {
            SDL_Color cc = basicPalette(term.curFg);
            SDL_SetRenderDrawColor(renderer, cc.r, cc.g, cc.b, 255);
            SDL_Rect curRect{ insetX + term.curCol*cellW, insetY + term.curRow*cellH, cellW, cellH };
            SDL_RenderDrawRect(renderer, &curRect);
        }

        SDL_RenderPresent(renderer);
    }

    if (programThread.joinable()) {
        // Request stop and wait.
        g_sigint_requested.store(true, std::memory_order_relaxed);
        programThread.join();
    }

    SDL_StopTextInput();

    env.screen = {};
    std::cout.rdbuf(oldCout);
    sdl_ui_active_flag().store(false, std::memory_order_relaxed);
    TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
}

