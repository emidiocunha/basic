//
//  editor.h
//  basic
//
//  Created by Emídio Cunha on 28/12/2025.
//

#pragma once

struct Env;

// Launches the full-screen editor and updates env.program on exit.
// ESC returns to the REPL.
void run_editor(Env& env);
