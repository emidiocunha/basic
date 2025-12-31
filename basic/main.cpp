//
//  main.cpp
//  basic
//
//  Created by Emidio Cunha on 24/12/2025.
//

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
#include "parser.h"
#include "interpreter.h"

int main(int argc, const char * argv[]) {
    Interpreter interp;
    std::cout << "GW-BASIC-like interpreter. Use RUN, LIST, EDIT, NEW, CLEAR, CONT, DELETE n, SAVE \"file\", LOAD \"file\".\n";

    // Optional: auto LOAD+RUN a program file passed on the command line.
    // Example: ./basic demo.bas
    if (argc >= 2 && argv[1] && argv[1][0] != '\0') {
        std::string filename = argv[1];
        interp.cmd_LOAD(filename);
        if (!interp.env.program.empty()) {
            interp.runFromStart();
        }
    }

    interp.repl();
    return EXIT_SUCCESS;
}
