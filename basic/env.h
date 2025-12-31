//
//  env.h
//  basic
//
//  Created by Emídio Cunha on 28/12/2025.
//
#pragma once

#include <map>

struct Parser;

struct RuntimeError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ParseError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Value {
    // BASIC has numeric and string types; we keep double and std::string
    std::variant<double, std::string> data;

    Value() : data(0.0) {}
    explicit Value(double d) : data(d) {}
    explicit Value(const std::string& s) : data(s) {}

    bool isString() const { return std::holds_alternative<std::string>(data); }
    bool isNumber() const { return std::holds_alternative<double>(data); }

    double asNumber() const {
        if (isNumber()) return std::get<double>(data);
        // string-to-number: GW-BASIC tries to convert leading numeric
        const std::string& s = std::get<std::string>(data);
        char* end = nullptr;
        double v = std::strtod(s.c_str(), &end);
        if (end == s.c_str()) return 0.0;
        return v;
    }

    const std::string& asString() const {
        if (isString()) return std::get<std::string>(data);
        // number to string
        static thread_local std::string temp;
        std::ostringstream oss;
        oss.setf(std::ios::fmtflags(0), std::ios::floatfield);
        oss << std::get<double>(data);
        temp = oss.str();
        return temp;
    }

    static Value fromBool(bool b) { return Value(b ? 1.0 : 0.0); }
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

    // Console print state (for TAB/PRINT column alignment)
    int printCol = 0; // 0-based column index on the current line

    // Random state for RND/RANDOMIZE behavior
    double lastRnd = 0.0;
    bool hasLastRnd = false;

    // Arrays: 1-D only (GW-BASIC style), indexed 0..N
    struct Array {
        bool isString = false;
        std::vector<Value> elems; // size N+1
    };
    std::unordered_map<std::string, Array> arrays;

    void clearVars() {
        vars.clear();
        arrays.clear();
        forStack.clear();
        gosubStack.clear();
    }

    Value getVar(const std::string& name) {
        auto it = vars.find(name);
        if (it != vars.end()) return it->second;
        // default initialization
        if (!name.empty() && name.back() == '$') return Value(std::string(""));
        return Value(0.0);
    }

    void setVar(const std::string& name, const Value& v) {
        // Type rules: $ variables must be string; others numeric
        if (!name.empty() && name.back() == '$') {
            if (!v.isString()) vars[name] = Value(v.asString());
            else vars[name] = v;
        } else {
            if (!v.isNumber()) vars[name] = Value(v.asNumber());
            else vars[name] = v;
        }
    }

    void dimArray(const std::string& name, int upperBound) {
        if (upperBound < 0) throw RuntimeError("Bad subscript");
        Array a;
        a.isString = (!name.empty() && name.back() == '$');
        a.elems.assign(static_cast<size_t>(upperBound) + 1, a.isString ? Value(std::string("")) : Value(0.0));
        arrays[name] = std::move(a);
    }

    Value getArrayElem(const std::string& name, int idx) {
        auto it = arrays.find(name);
        if (it == arrays.end()) throw RuntimeError("Subscripted variable not DIMensioned");
        if (idx < 0 || static_cast<size_t>(idx) >= it->second.elems.size()) throw RuntimeError("Bad subscript");
        return it->second.elems[static_cast<size_t>(idx)];
    }

    void setArrayElem(const std::string& name, int idx, const Value& v) {
        auto it = arrays.find(name);
        if (it == arrays.end()) throw RuntimeError("Subscripted variable not DIMensioned");
        if (idx < 0 || static_cast<size_t>(idx) >= it->second.elems.size()) throw RuntimeError("Bad subscript");
        if (!name.empty() && name.back() == '$') {
            it->second.elems[static_cast<size_t>(idx)] = v.isString() ? v : Value(v.asString());
        } else {
            it->second.elems[static_cast<size_t>(idx)] = v.isNumber() ? v : Value(v.asNumber());
        }
    }
};
