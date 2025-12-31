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
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include "env.h"

namespace {

struct RawMode {
    termios orig{};
    bool active = false;

    void enable() {
        if (active || !isatty(STDIN_FILENO)) return;
        tcgetattr(STDIN_FILENO, &orig);
        termios raw = orig;
        raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_iflag &= ~(IXON | ICRNL);
        raw.c_oflag &= ~(OPOST);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 1;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        active = true;
    }

    void disable() {
        if (!active) return;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
        active = false;
    }

    ~RawMode() { disable(); }
};

void clear_screen() {
    std::cout << "\033[2J\033[H";
}

void move_cursor(int r, int c) {
    std::cout << "\033[" << r << ";" << c << "H";
}

bool peek_input(int ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    timeval tv{ ms / 1000, (ms % 1000) * 1000 };
    return select(STDIN_FILENO + 1, &set, nullptr, nullptr, &tv) > 0;
}

enum class Key { None, Up, Down, Left, Right, Enter, Backspace, Esc, Char };

Key read_key(char& out) {
    char c;
    if (read(STDIN_FILENO, &c, 1) <= 0) return Key::None;

    if (c == 27) {
        if (!peek_input(20)) return Key::Esc;
        char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return Key::Esc;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return Key::Esc;
        if (seq[0] == '[') {
            if (seq[1] == 'A') return Key::Up;
            if (seq[1] == 'B') return Key::Down;
            if (seq[1] == 'C') return Key::Right;
            if (seq[1] == 'D') return Key::Left;
        }
        return Key::Esc;
    }

    if (c == 127 || c == 8) return Key::Backspace;
    if (c == '\n' || c == '\r') return Key::Enter;

    out = c;
    return Key::Char;
}

} // namespace

void run_editor(Env& env) {
    RawMode rm;
    rm.enable();

    std::vector<std::string> lines;
    for (auto& [ln, txt] : env.program) {
        lines.push_back(std::to_string(ln) + " " + txt);
    }
    if (lines.empty()) lines.push_back("");

    int row = 0, col = 0;

    while (true) {
        clear_screen();
        for (size_t i = 0; i < lines.size(); ++i) {
            move_cursor(i + 1, 1);
            std::cout << lines[i];
        }
        move_cursor(row + 1, col + 1);
        std::cout.flush();

        char ch = 0;
        Key k = read_key(ch);

        if (k == Key::Esc) break;
        if (k == Key::Up && row > 0) row--;
        if (k == Key::Down && row + 1 < (int)lines.size()) row++;
        if (k == Key::Left && col > 0) col--;
        if (k == Key::Right && col < (int)lines[row].size()) col++;

        if (k == Key::Backspace && col > 0) {
            lines[row].erase(col - 1, 1);
            col--;
        }

        if (k == Key::Enter) {
            lines.insert(lines.begin() + row + 1,
                         lines[row].substr(col));
            lines[row].erase(col);
            row++; col = 0;
        }

        if (k == Key::Char && std::isprint((unsigned char)ch)) {
            lines[row].insert(lines[row].begin() + col, ch);
            col++;
        }
    }

    rm.disable();
    clear_screen();

    // Rebuild program
    env.program.clear();
    for (auto& l : lines) {
        std::istringstream iss(l);
        int ln;
        if (!(iss >> ln)) continue;
        std::string rest;
        std::getline(iss, rest);
        if (!rest.empty())
            env.program[ln] = rest.substr(1);
    }
}
