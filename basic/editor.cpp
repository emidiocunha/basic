//
//  editor.cpp
//  basic
//
//  Created by Emídio Cunha on 28/12/2025.
//

#include "editor.h"

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <cctype>
#include <cmath>
#include <algorithm>
#include "env.h"
#include "SDL.h"
#include "SDL_ttf.h"

static TTF_Font* basic_open_mono_font(int pt) {
    // Try a few common monospace font locations on macOS.
    const char* candidates[] = {
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Monaco.ttf",
        "/Library/Fonts/Menlo.ttc",
        "/Library/Fonts/Courier New.ttf"
    };
    for (const char* p : candidates) {
        if (!p) continue;
        TTF_Font* f = TTF_OpenFont(p, pt);
        if (f) return f;
    }
    return nullptr;
}

static void sdl_draw_text(SDL_Renderer* r, TTF_Font* font, const std::string& s, int x, int y, SDL_Color c) {
    if (s.empty()) return;
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, s.c_str(), c);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
    if (!tex) { SDL_FreeSurface(surf); return; }
    SDL_Rect dst{ x, y, surf->w, surf->h };
    SDL_RenderCopy(r, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}

// Run the editor using the *existing* REPL window/renderer/font.
// The REPL owns `win`, `ren`, and `font` and is responsible for init/teardown.
void run_editor_inplace(Env& env,
                        SDL_Window* win,
                        SDL_Renderer* ren,
                        TTF_Font* font,
                        int cols,
                        int rows,
                        int cellW,
                        int cellH,
                        int insetX,
                        int insetY) {
    (void)win; // kept for future use (e.g. title changes)

    // Build editable lines from program.
    std::vector<std::string> lines;
    for (auto& [ln, txt] : env.program) {
        lines.push_back(std::to_string(ln) + " " + txt);
    }
    if (lines.empty()) lines.push_back("");

    int row = 0, col = 0;
    int top = 0;

    SDL_StartTextInput();

    auto clampCursor = [&]() {
        if (row < 0) row = 0;
        if (row >= (int)lines.size()) row = (int)lines.size() - 1;
        if (row < 0) row = 0;
        int len = (row >= 0 && row < (int)lines.size()) ? (int)lines[row].size() : 0;
        if (col < 0) col = 0;
        if (col > len) col = len;
    };

    bool running = true;
    while (running) {
        // Keep viewport aligned.
        if (row < top) top = row;
        if (row >= top + rows) top = row - rows + 1;
        if (top < 0) top = 0;
        if (top > (int)lines.size() - 1) top = std::max(0, (int)lines.size() - 1);

        // Events
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = false;
                break;
            }
            if (e.type == SDL_KEYDOWN) {
                SDL_Keycode kc = e.key.keysym.sym;
                SDL_Keymod mod = (SDL_Keymod)e.key.keysym.mod;

                if (kc == SDLK_ESCAPE) {
                    running = false;
                    break;
                }
                if (kc == SDLK_UP) { row--; clampCursor(); }
                else if (kc == SDLK_DOWN) { row++; clampCursor(); }
                else if (kc == SDLK_LEFT) { col--; clampCursor(); }
                else if (kc == SDLK_RIGHT) { col++; clampCursor(); }
                else if (kc == SDLK_BACKSPACE) {
                    clampCursor();
                    if (col > 0 && row >= 0 && row < (int)lines.size()) {
                        lines[row].erase((size_t)col - 1, 1);
                        col--;
                    }
                }
                else if (kc == SDLK_RETURN || kc == SDLK_KP_ENTER) {
                    clampCursor();
                    if (row >= 0 && row < (int)lines.size()) {
                        std::string tail = lines[row].substr((size_t)col);
                        lines[row].erase((size_t)col);
                        lines.insert(lines.begin() + row + 1, tail);
                        row++; col = 0;
                    }
                }
                else if ((mod & KMOD_CTRL) && (kc == SDLK_k)) {
                    // Ctrl+K delete current line
                    if (!lines.empty() && row >= 0 && row < (int)lines.size()) {
                        lines.erase(lines.begin() + row);
                        if (lines.empty()) {
                            lines.push_back("");
                            row = 0; col = 0;
                        } else {
                            if (row >= (int)lines.size()) row = (int)lines.size() - 1;
                            if (row < 0) row = 0;
                            clampCursor();
                        }
                    }
                }
            }
            if (e.type == SDL_TEXTINPUT) {
                clampCursor();
                if (row >= 0 && row < (int)lines.size()) {
                    // Insert as UTF-8 bytes (editor is ASCII-ish; keep bytes).
                    std::string t = e.text.text;
                    if (!t.empty()) {
                        lines[row].insert((size_t)col, t);
                        col += (int)t.size();
                        clampCursor();
                    }
                }
            }
        }

        // Render
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);

        SDL_Color fg{ 220, 220, 220, 255 };
        SDL_Color dim{ 120, 120, 120, 255 };

        for (int screenR = 0; screenR < rows; ++screenR) {
            int i = top + screenR;
            if (i < 0 || i >= (int)lines.size()) continue;

            std::string s = lines[i];
            if ((int)s.size() > cols) s.resize((size_t)cols);
            sdl_draw_text(ren, font, s, insetX, insetY + screenR * cellH, fg);
        }

        // Cursor (block outline)
        clampCursor();
        int cursorScreenRow = row - top;
        if (cursorScreenRow < 0) cursorScreenRow = 0;
        if (cursorScreenRow >= rows) cursorScreenRow = rows - 1;
        int cursorScreenCol = col;
        if (cursorScreenCol < 0) cursorScreenCol = 0;
        if (cursorScreenCol >= cols) cursorScreenCol = cols - 1;

        SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        SDL_Rect cur{ insetX + cursorScreenCol * cellW, insetY + cursorScreenRow * cellH, cellW, cellH };
        SDL_RenderDrawRect(ren, &cur);

        // Status line hint (last row overlay)
        std::string hint = "ESC=exit  CTRL+K=delete line";
        if ((int)hint.size() > cols) hint.resize((size_t)cols);
        sdl_draw_text(ren, font, hint, insetX, insetY + (rows - 1) * cellH, dim);

        SDL_RenderPresent(ren);
    }

    SDL_StopTextInput();

    // Rebuild program from edited lines.
    env.program.clear();
    for (auto& l : lines) {
        std::istringstream iss(l);
        int ln;
        if (!(iss >> ln)) continue;
        std::string rest;
        std::getline(iss, rest);
        if (!rest.empty()) env.program[ln] = rest.substr(1);
    }
}

void run_editor(Env& env) {
    (void)env;
    std::cout << "Editor: in-place SDL editor requires the SDL REPL to call run_editor_inplace(...).\n";
}
