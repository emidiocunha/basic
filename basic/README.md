# GW-BASIC–like Interpreter (C++)

A lightweight, cross-platform **GW-BASIC / MSX-BASIC–inspired interpreter** written in modern C++.

This project focuses on **language compatibility, correctness, and clarity**, not on emulating old hardware.  
It is designed to run in a modern terminal while preserving the *look & feel* and semantics of classic BASIC.

---

## ✨ Features

### ✅ Language Core
- Line-numbered programs
- Immediate (REPL) mode
- Numeric (`Double`) and string variables (`$`)
- Expressions with correct precedence
- IF / THEN
- GOTO / GOSUB / RETURN
- FOR / NEXT (supports nested loops)
- DIM (1D arrays, implicit DIM 0..10)
- READ / DATA / RESTORE
- DEFINT (accepted and ignored, for compatibility)
- REM comments

### ✅ Built-in Functions
- Math: `SIN`, `COS`, `TAN`, `ATN`, `LOG`, `EXP`, `SQR`, `ABS`, `INT`, `SGN`
- Strings: `LEN`, `LEFT$`, `RIGHT$`, `MID$`, `CHR$`, `ASC`, `STR$`, `VAL`
- Random: `RND()` (GW-BASIC–style behavior)
- Time: `TIME` / `TIME()`
- Formatting: `TAB(n)`

### 🎲 Random Numbers
- `RND()` → next random number
- `RND(0)` → repeat last random number
- `RND(x > 0)` → next random number
- `RND(x < 0)` → reseed using `abs(x)`
- `RANDOMIZE [seed]` supported

### ⏱️ Timers
- `ON INTERVAL n GOSUB line`
  - `n` is in **1/60th second ticks** (GW/MSX compatible)
- `INTERVAL ON`
- `INTERVAL OFF`
- `INTERVAL STOP`
- Safe interrupt handling with proper resume semantics

### 🖥️ Screen & Terminal
- `PRINT` with `,` and `;`
- `CLS`
- `LOCATE row, col [, cursor]`
  - cursor: `0 = hide`, `1 = show`
- `COLOR f, b` (ANSI-mapped, GW-BASIC color palette)
- Cursor-aware printing (`TAB`, column tracking)

### 🧾 Program Editing
- Built-in line editor
- Scrolls correctly when program exceeds screen height
- Handles insertion, deletion, navigation

### ⌨️ REPL
- Immediate execution
- `RUN`, `LIST`, `NEW`, `CLEAR`, `CONT`
- `QUIT` / `EXIT`
- **Ctrl+C** stops a running program (returns to REPL)
- **UP arrow recalls last command** (in real terminals)
- Graceful fallback in non-TTY environments (e.g. Xcode console)

---

## 🛠️ Build

### macOS / Linux
```bash
clang++ -std=c++20 -O2 *.cpp -o basic
./basic
