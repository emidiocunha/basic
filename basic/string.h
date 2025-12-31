//
//  string.h
//  basic
//
//  Created by Emídio Cunha on 28/12/2025.
//
#pragma once

#include <string>
#include <vector>

using std::string;
using std::vector;

static inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b-1]))) --b;
    return s.substr(a, b - a);
}

static inline bool iequal(char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
}

static inline bool istartswith(const std::string& s, const std::string& pfx) {
    if (s.size() < pfx.size()) return false;
    for (size_t i = 0; i < pfx.size(); ++i) {
        if (!iequal(s[i], pfx[i])) return false;
    }
    return true;
}

static inline std::string upper_ascii(const std::string& s) {
    std::string u; u.reserve(s.size());
    for (char c : s) u.push_back(std::toupper(static_cast<unsigned char>(c)));
    return u;
}
