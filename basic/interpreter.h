//
//  interpreter.h
//  basic
//
//  Created by Emídio Cunha on 28/12/2025.
//

#pragma once

#include "token.h"
#include "lexer.h"

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

struct Interpreter {
    void resetAfterProgramEdit() {
        // Any edit to the program invalidates execution state and CONT.
        env.running = false;
        env.stopped = false;
        env.contAvailable = false;
        env.posInLine = 0;
        env.pc = env.program.end();
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

    void cmd_LIST() {
        for (auto& [ln, text] : env.program) std::cout << ln << " " << text << "\n";
    }

    void cmd_NEW() {
        env.program.clear();
        env.clearVars();
        env.running = false;
        env.stopped = false;
        env.contAvailable = false;
        env.posInLine = 0;
        env.pc = env.program.end();
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
        env.running = true;
        env.stopped = false;
        env.pc = env.program.begin();
        env.posInLine = 0;
        execute();
    }

    void cont() {
        if (!env.contAvailable) {
            std::cout << "Cannot CONTINUE\n";
            return;
        }
        env.running = true;
        env.stopped = false;
        execute();
    }

    void execute() {
        while (env.running && !env.stopped) {
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
        Parser p(line, env);
        try {
            p.parseAndExecLine();
        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << "\n";
        }
    }

    void repl() {
        std::string line;
        while (true) {
            std::cout << "OK> ";
            if (!std::getline(std::cin, line)) break;
            std::string t = trim(line);
            if (t.empty()) continue;

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
            if (upper == "LIST") { cmd_LIST(); continue; }
            if (upper == "NEW") { cmd_NEW(); continue; }
            if (upper == "CLEAR") { cmd_CLEAR(); continue; }
            if (upper == "CONT") { cont(); continue; }
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
                cmd_LOAD(fn);
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

