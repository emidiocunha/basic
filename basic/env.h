//
//  env.h
//  basic
//
//  Created by Emídio Cunha on 28/12/2025.
//
#pragma once

#include <map>
#include <unordered_map>
#include <vector>
#include <variant>
#include <cstdint>
#include <string>
#include <sstream>
#include <stdexcept>
#include <cstdlib>
#include <chrono>
#include <cctype>
#include <cstring>
#include <ctime>
#include <cmath>

struct Parser;

struct RuntimeError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ParseError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Value {
    // BASIC values: integer (16-bit), double, string.
    std::variant<int16_t, double, std::string> data;

    Value() : data(0.0) {}
    explicit Value(double d) : data(d) {}
    explicit Value(int16_t i) : data(i) {}
    explicit Value(const std::string& s) : data(s) {}

    bool isString() const { return std::holds_alternative<std::string>(data); }
    bool isInt() const { return std::holds_alternative<int16_t>(data); }
    bool isDouble() const { return std::holds_alternative<double>(data); }
    bool isNumber() const { return isInt() || isDouble(); }

    double asNumber() const {
        if (isDouble()) return std::get<double>(data);
        if (isInt()) return static_cast<double>(std::get<int16_t>(data));
        // string-to-number: GW-BASIC tries to convert leading numeric
        const std::string& s = std::get<std::string>(data);
        char* end = nullptr;
        double v = std::strtod(s.c_str(), &end);
        if (end == s.c_str()) return 0.0;
        return v;
    }

    static int16_t toInt16Checked(double x) {
        // GW/QBASIC-style integer range check
        double t = std::trunc(x);
        if (t < -32768.0 || t > 32767.0) throw RuntimeError("Overflow");
        return static_cast<int16_t>(static_cast<int>(t));
    }

    int16_t asInt() const {
        if (isInt()) return std::get<int16_t>(data);
        if (isDouble()) return toInt16Checked(std::get<double>(data));
        return toInt16Checked(asNumber());
    }

    const std::string& asString() const {
        if (isString()) return std::get<std::string>(data);
        static thread_local std::string temp;
        std::ostringstream oss;
        if (isInt()) oss << static_cast<int>(std::get<int16_t>(data));
        else {
            oss.setf(std::ios::fmtflags(0), std::ios::floatfield);
            oss << std::get<double>(data);
        }
        temp = oss.str();
        return temp;
    }

    static Value fromBool(bool b) { return Value(static_cast<int16_t>(b ? 1 : 0)); }
};

struct Env {
    // Variables
    std::unordered_map<std::string, Value> vars;

    // Program: line number -> original line text (after number)
    std::map<int, std::string> program;

    // For stack (FOR/NEXT)
    struct ForFrame {
        std::string var;
        double endValue;
        double step;
        std::map<int, std::string>::iterator returnIt; // line iterator to resume
        size_t posInLine; // character position within the line to restart after FOR body
    };
    std::vector<ForFrame> forStack;

    // Gosub stack
    std::vector<std::pair<std::map<int, std::string>::iterator, size_t>> gosubStack;

    // Execution state
    std::map<int, std::string>::iterator pc;
    size_t posInLine = 0; // position within current line text
    bool running = false;
    bool stopped = false;
    bool contAvailable = false;
    
    struct DataItem {
        int line = 0;
        std::string raw;
        bool wasQuotedString = false;
    };

    std::vector<DataItem> dataCache;
    size_t dataPtr = 0;
    bool dataCacheBuilt = false;

    // ON INTERVAL / INTERVAL ON|OFF|STOP support
    bool intervalArmed = false;
    bool intervalEnabled = false;
    bool inIntervalISR = false;
    double intervalSeconds = 0.0;
    int intervalGosubLine = 0;
    std::chrono::steady_clock::time_point nextIntervalFire = std::chrono::steady_clock::now();

    // Console print state (for TAB/PRINT column alignment)
    int printCol = 0; // 0-based column index on the current line

    // DEFINT: when true for a starting letter, numeric variables default to 16-bit integer.
    // Indexed 0..25 for 'A'..'Z'
    bool defInt[26] = {false};

    enum class VarType { Double, Int16, String };

    bool defIntForName(const std::string& name) const {
        if (name.empty()) return false;
        char c = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        if (c < 'A' || c > 'Z') return false;
        return defInt[c - 'A'];
    }

    VarType varTypeForName(const std::string& name) const {
        if (!name.empty()) {
            char last = name.back();
            if (last == '$') return VarType::String;
            if (last == '%') return VarType::Int16;
        }
        if (defIntForName(name)) return VarType::Int16;
        return VarType::Double;
    }

    void setDefIntRange(char a, char b, bool on = true) {
        a = static_cast<char>(std::toupper(static_cast<unsigned char>(a)));
        b = static_cast<char>(std::toupper(static_cast<unsigned char>(b)));
        if (a < 'A') a = 'A';
        if (b > 'Z') b = 'Z';
        if (a > b) std::swap(a, b);
        for (char c = a; c <= b; ++c) defInt[c - 'A'] = on;
    }

    void clearDefInt() {
        for (bool &v : defInt) v = false;
    }

    // Random state for RND/RANDOMIZE behavior
    double lastRnd = 0.0;
    bool hasLastRnd = false;

    // Arrays: 1-D only (GW-BASIC style), indexed 0..N
    struct Array {
        VarType type = VarType::Double;
        std::vector<Value> elems; // size N+1
    };
    std::unordered_map<std::string, Array> arrays;

    void clearVars() {
        // CLEAR/CLEAR-like: reset variables/arrays but keep program + control-flow intact.
        // Many GW-BASIC programs use CLEAR n as a memory-tuning hint and do not expect
        // it to break active FOR/NEXT or GOSUB/RETURN state.
        vars.clear();
        arrays.clear();

        // DATA/READ state
        dataCacheBuilt = false;
        dataCache.clear();
        dataPtr = 0;

        // Keep FOR/GOSUB stacks, interval state, and execution cursor as-is.
    }

    void clearProgramAndState() {
        // NEW: clear the stored program and reset runtime state.
        program.clear();
        clearDefInt();

        // Control-flow stacks
        forStack.clear();
        gosubStack.clear();

        // DATA/READ state
        dataCacheBuilt = false;
        dataCache.clear();
        dataPtr = 0;

        // ON INTERVAL state
        intervalArmed = false;
        intervalEnabled = false;
        inIntervalISR = false;
        intervalSeconds = 0.0;
        intervalGosubLine = 0;
        nextIntervalFire = std::chrono::steady_clock::now();

        // Console/PRINT state
        printCol = 0;

        // RND state
        lastRnd = 0.0;
        hasLastRnd = false;

        // Variables and arrays
        vars.clear();
        arrays.clear();

        pc = program.end();
        running = false;
        stopped = false;
        contAvailable = false;
        posInLine = 0;
        // Do not call clearVars() here; already cleared above.
    }

    Value getVar(const std::string& name) {
        auto it = vars.find(name);
        if (it != vars.end()) return it->second;

        switch (varTypeForName(name)) {
            case VarType::String: return Value(std::string(""));
            case VarType::Int16:  return Value(static_cast<int16_t>(0));
            case VarType::Double: return Value(0.0);
        }
        return Value(0.0);
    }

    void setVar(const std::string& name, const Value& v) {
        switch (varTypeForName(name)) {
            case VarType::String:
                vars[name] = v.isString() ? v : Value(v.asString());
                return;
            case VarType::Int16:
                vars[name] = Value(v.asInt());
                return;
            case VarType::Double:
                vars[name] = Value(v.asNumber());
                return;
        }
    }

    void dimArray(const std::string& name, int upperBound) {
        if (upperBound < 0) throw RuntimeError("Bad subscript");

        // GW-BASIC: DIM is only allowed once per array name (REDIM requires ERASE; not implemented)
        if (arrays.find(name) != arrays.end()) {
            throw RuntimeError("Duplicate definition");
        }

        Array a;
        a.type = varTypeForName(name);
        Value init = (a.type == VarType::String) ? Value(std::string(""))
                    : (a.type == VarType::Int16) ? Value(static_cast<int16_t>(0))
                    : Value(0.0);
        a.elems.assign(static_cast<size_t>(upperBound) + 1, init);
        arrays.emplace(name, std::move(a));
    }

    void ensureArrayImplicitDim(const std::string& name) {
        // Implicit dimensioning: if referenced before DIM, create 0..10
        if (arrays.find(name) != arrays.end()) return;
        Array a;
        a.type = varTypeForName(name);
        Value init = (a.type == VarType::String) ? Value(std::string(""))
                    : (a.type == VarType::Int16) ? Value(static_cast<int16_t>(0))
                    : Value(0.0);
        a.elems.assign(11, init);
        arrays.emplace(name, std::move(a));
    }

    Value getArrayElem(const std::string& name, int idx) {
        if (idx < 0) throw RuntimeError("Bad subscript");
        ensureArrayImplicitDim(name);
        auto it = arrays.find(name);
        if (it == arrays.end()) throw RuntimeError("Subscripted variable not DIMensioned");
        if (static_cast<size_t>(idx) >= it->second.elems.size()) throw RuntimeError("Subscript out of range");
        return it->second.elems[static_cast<size_t>(idx)];
    }

    void setArrayElem(const std::string& name, int idx, const Value& v) {
        if (idx < 0) throw RuntimeError("Bad subscript");
        ensureArrayImplicitDim(name);
        auto it = arrays.find(name);
        if (it == arrays.end()) throw RuntimeError("Subscripted variable not DIMensioned");
        if (static_cast<size_t>(idx) >= it->second.elems.size()) throw RuntimeError("Subscript out of range");

        switch (it->second.type) {
            case VarType::String:
                it->second.elems[static_cast<size_t>(idx)] = v.isString() ? v : Value(v.asString());
                break;
            case VarType::Int16:
                it->second.elems[static_cast<size_t>(idx)] = Value(v.asInt());
                break;
            case VarType::Double:
                it->second.elems[static_cast<size_t>(idx)] = Value(v.asNumber());
                break;
        }
    }
    
    static inline std::string trim_copy(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace((unsigned char)s[a])) a++;
        while (b > a && std::isspace((unsigned char)s[b-1])) b--;
        return s.substr(a, b-a);
    }

    static inline bool ieq_at_word(const std::string& s, size_t pos, const char* w) {
        // match word w at pos with word-boundary rules (A-Z0-9_$)
        size_t n = std::strlen(w);
        if (pos + n > s.size()) return false;
        for (size_t i=0;i<n;i++) {
            char a = (char)std::toupper((unsigned char)s[pos+i]);
            char b = (char)w[i];
            if (a != b) return false;
        }
        auto isWord = [](char c){
            return std::isalnum((unsigned char)c) || c=='_' || c=='$';
        };
        if (pos > 0 && isWord(s[pos-1])) return false;
        if (pos + n < s.size() && isWord(s[pos+n])) return false;
        return true;
    }

    static inline void split_data_items(const std::string& body, std::vector<std::string>& out, std::vector<bool>& quoted) {
        out.clear(); quoted.clear();
        std::string cur;
        bool inQ = false;
        for (size_t i=0;i<body.size();i++) {
            char c = body[i];
            if (c == '"') {
                // GW-style: "" inside quoted -> literal "
                if (inQ && i+1 < body.size() && body[i+1] == '"') {
                    cur.push_back('"'); i++;
                } else {
                    inQ = !inQ;
                }
                continue;
            }
            if (!inQ && c == ',') {
                auto t = trim_copy(cur);
                out.push_back(t);
                quoted.push_back(!t.empty() && t.front()=='"' && t.back()=='"'); // fallback (not relied on)
                cur.clear();
                continue;
            }
            cur.push_back(c);
        }
        auto t = trim_copy(cur);
        out.push_back(t);
        quoted.push_back(!t.empty() && t.front()=='"' && t.back()=='"');
    }

    inline void rebuildDataCache(const std::map<int,std::string>& program) {
        dataCache.clear();
        dataPtr = 0;
        dataCacheBuilt = true;

        for (auto& [ln, text] : program) {
            // scan statement-by-statement for DATA (start or after ':')
            size_t i = 0;
            bool stmtStart = true;
            bool inQ = false;
            while (i < text.size()) {
                char c = text[i];
                if (c == '"') inQ = !inQ;
                if (!inQ) {
                    if (stmtStart) {
                        while (i < text.size() && std::isspace((unsigned char)text[i])) i++;
                        if (i < text.size() && ieq_at_word(text, i, "DATA")) {
                            i += 4;
                            // capture until ':' (not in quotes) or end
                            while (i < text.size() && std::isspace((unsigned char)text[i])) i++;
                            size_t start = i;
                            bool q2 = false;
                            while (i < text.size()) {
                                char d = text[i];
                                if (d == '"') q2 = !q2;
                                if (!q2 && d == ':') break;
                                i++;
                            }
                            std::string body = text.substr(start, i - start);

                            std::vector<std::string> items;
                            std::vector<bool> dummy;
                            split_data_items(body, items, dummy);

                            for (auto& raw0 : items) {
                                std::string raw = trim_copy(raw0);
                                bool wasQuoted = false;
                                if (raw.size() >= 2 && raw.front()=='"' && raw.back()=='"') {
                                    wasQuoted = true;
                                    raw = raw.substr(1, raw.size()-2);
                                    // unescape "" -> "
                                    std::string un;
                                    for (size_t k=0;k<raw.size();k++) {
                                        if (raw[k]=='"' && k+1<raw.size() && raw[k+1]=='"') { un.push_back('"'); k++; }
                                        else un.push_back(raw[k]);
                                    }
                                    raw = std::move(un);
                                }
                                dataCache.push_back(DataItem{ln, raw, wasQuoted});
                            }

                            // if we stopped at ':', next stmt starts after it
                            stmtStart = true;
                            if (i < text.size() && text[i] == ':') { i++; continue; }
                            break; // end line
                        }
                    }

                    if (c == ':') stmtStart = true;
                    else if (!std::isspace((unsigned char)c)) stmtStart = false;
                }
                i++;
            }
        }
    }

    inline void ensureDataCache(const std::map<int,std::string>& program) {
        if (!dataCacheBuilt) rebuildDataCache(program);
    }

    inline void restoreData(int lineOr0, const std::map<int,std::string>& program) {
        ensureDataCache(program);
        if (lineOr0 <= 0) { dataPtr = 0; return; }
        size_t i = 0;
        while (i < dataCache.size() && dataCache[i].line < lineOr0) i++;
        dataPtr = i;
    }

    inline Value readNextData(bool wantString, const std::map<int,std::string>& program) {
        ensureDataCache(program);
        if (dataPtr >= dataCache.size()) throw RuntimeError("Out of data");
        const auto& it = dataCache[dataPtr++];
        if (wantString) return Value(it.raw);
        // numeric: try parse
        char* end = nullptr;
        double d = std::strtod(it.raw.c_str(), &end);
        if (end == it.raw.c_str()) d = 0.0;
        return Value(d);
    }
};
