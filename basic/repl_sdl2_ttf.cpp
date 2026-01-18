//
//  repl_sdl2_ttf.cpp
//  basic
//
//  Created by Emídio Cunha on 15/01/2026.
//

#pragma once

#include <iostream>
#include <deque>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cmath>

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
        std::fill(grid.begin(), grid.end(), Cell{});
        curRow = 0; curCol = 0;
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
    std::string pending;

    explicit SDLTerminalStreamBuf(SDLTerminalBuffer* b) : buf(b) {}

    int overflow(int ch) override {
        if (ch == EOF) return EOF;
        char c = (char)ch;
        if (c == '\n') {
            if (!pending.empty()) { buf->write(pending); pending.clear(); }
            buf->putChar('\n');
        } else {
            pending.push_back(c);
            if (pending.size() > 4096) { buf->write(pending); pending.clear(); }
        }
        return ch;
    }

    int sync() override {
        if (!pending.empty()) { buf->write(pending); pending.clear(); }
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

    // Determine character cell size
    int charW = 0, charH = 0;
    {
        int w = 0, h = 0;
        SDL_Color fg{255,255,255,255};
        SDL_Texture* t = sdl_make_text_texture(renderer, font, "M", fg, w, h);
        if (t) SDL_DestroyTexture(t);

        int minx=0,maxx=0,miny=0,maxy=0,advance=0;
        if (TTF_GlyphMetrics(font, 'M', &minx,&maxx,&miny,&maxy,&advance) == 0 && advance > 0)
            charW = advance;
        else
            charW = (w > 0 ? w : 10);

        charH = TTF_FontLineSkip(font);
        if (charH <= 0) charH = (h > 0 ? h : 18);
    }

    // Fixed console geometry: 80x25 characters
    termCols = 80;
    termRows = 25;

    if (charW > 0 && charH > 0) {
        const int desiredPxW = termCols * charW;
        const int desiredPxH = termRows * charH;

        int winW=0, winH=0, outW=0, outH=0;
        SDL_GetWindowSize(win, &winW, &winH);
        SDL_GetRendererOutputSize(renderer, &outW, &outH);

        float scaleX = (winW > 0) ? ((float)outW / (float)winW) : 1.0f;
        float scaleY = (winH > 0) ? ((float)outH / (float)winH) : 1.0f;
        if (scaleX <= 0.0f) scaleX = 1.0f;
        if (scaleY <= 0.0f) scaleY = 1.0f;

        int desiredWinW = (int)lroundf((float)desiredPxW / scaleX);
        int desiredWinH = (int)lroundf((float)desiredPxH / scaleY);

        SDL_SetWindowSize(win, desiredWinW + 32, desiredWinH + 32);
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

        SDL_RaiseWindow(win);
#if SDL_VERSION_ATLEAST(2,0,5)
        (void)SDL_SetWindowInputFocus(win);
#endif
        SDL_PumpEvents();
    }

    int winW_pts=0, winH_pts=0, outW_px=0, outH_px=0;
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

    // Wire BASIC screen driver to the SDL terminal buffer
    env.screen.putChar = [&](char c) { term.putChar(c); };
    env.screen.cls = [&]() { term.clear(); };
    env.screen.locate = [&](int row, int col) { term.locate1(row, col); };
    env.screen.showCursor = [&](bool show) { term.showCursor(show); };
    env.screen.color = [&](int fg, int bg) {
        int useFg = (fg < 0) ? term.curFg : fg;
        int useBg = (bg < 0) ? term.curBg : bg;
        term.setColor(useFg, useBg);
    };
    env.screen.beep = [&]() { };

    SDLTerminalStreamBuf sb(&term);
    std::streambuf* oldCout = std::cout.rdbuf(&sb);

    // REPL input state
    std::string line;
    std::deque<std::string> history;
    std::string historyDraft;
    int historyIndex = -1;
    bool historyNav = false;

    int inputAnchorRow = term.curRow;
    int inputAnchorCol = term.curCol;

    auto putAt0 = [&](int r, int c, char ch) {
        if (r < 0 || c < 0 || r >= term.rows || c >= term.cols) return;
        auto& cell = term.grid[(size_t)(r * term.cols + c)];
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
    bool sdlDebugPaused = false;
    bool sdlDebugNeedPrint = false;

    auto finishProgramRun = [&]() {
        programRunning = false;
        sdlDebugPaused = false;
        sdlDebugNeedPrint = false;
        debugStepping = false;
        term.putChar('\n');
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

        if (upper == "RUN") { startRun(); programRunning = true; return; }

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
            if (env.running) programRunning = true;
            else beginPrompt();
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
            if (runAfterLoad) { startRun(); programRunning = true; return; }
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
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) { continue; }

            if (e.type == SDL_TEXTINPUT) {
                if (programRunning) continue;
                if (historyNav) { historyNav = false; historyIndex = -1; }
                const char* t = e.text.text;
                if (t) for (const char* p = t; *p; ++p) { line.push_back(*p); term.putChar(*p); }
                continue;
            }

            if (e.type == SDL_KEYDOWN) {
                SDL_Keycode sym = e.key.keysym.sym;
                SDL_Keymod mod = (SDL_Keymod)e.key.keysym.mod;

                if (programRunning) {
                    if (debugStepping) {
                        if (sym == SDLK_SPACE) sdlDebugPaused = false;
                        else if (sym == SDLK_ESCAPE) { env.running=false; env.stopped=false; env.contAvailable=false; finishProgramRun(); }
                        continue;
                    }
                    if (sym == SDLK_ESCAPE) g_sigint_requested.store(true, std::memory_order_relaxed);
                    continue;
                }

                if (sym == SDLK_ESCAPE) { running = false; break; }

                if (sym == SDLK_F5) { redrawInput("RUN"); commitLine("RUN"); continue; }

                if ((mod & KMOD_CTRL) && (sym == SDLK_l)) {
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

        if (programRunning) {
            for (int i = 0; i < 200 && programRunning; ++i) executeStep();
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

    SDL_StopTextInput();

    env.screen = {};
    std::cout.rdbuf(oldCout);

    TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
}
