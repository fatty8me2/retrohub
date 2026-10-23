#pragma once
/* The one JSON reader/writer for RetroHub (hub, net, saves, runners). Small, dependency-free, no
 * exceptions escape, safe on malformed or hostile input (depth-limited, no recursion past
 * ParseOptions::max_depth). Strings are UTF-8; \uXXXX escapes (incl. surrogate pairs) decode to
 * UTF-8, lone surrogates become U+FFFD. Objects keep insertion order; duplicate keys: first wins. */

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    bool b = false;
    double n = 0;           // Number: as double
    int64_t i = 0;          // Number: exact when is_int (saturates at int64 limits), else (int64_t)n
    bool is_int = false;    // Number token had no '.', 'e' or 'E'
    std::string s;          // String contents
    std::string raw;        // Number: the original token text
    std::vector<Value> a;                                   // Array items
    std::vector<std::pair<std::string, Value>> o;           // Object members (in order)

    bool is_null() const { return type == Null; }
    bool is_bool() const { return type == Bool; }
    bool is_number() const { return type == Number; }
    bool is_string() const { return type == String; }
    bool is_array() const { return type == Array; }
    bool is_object() const { return type == Object; }

    /* Object lookup; null when not an object or the key is missing. */
    const Value* get(const std::string& key) const;
    /* Typed lookups: the default when the key is missing or has another type. */
    std::string str(const std::string& key, const std::string& def = std::string()) const;
    double num(const std::string& key, double def = 0) const;
    int64_t i64(const std::string& key, int64_t def = 0) const;
    bool boolean(const std::string& key, bool def = false) const;
};

struct ParseOptions {
    int max_depth = 64;           // nesting limit (arrays/objects); deeper input fails cleanly
    bool allow_trailing = false;  // accept (and ignore) anything after the first value
};

/* Parse `text` into *out. Returns false (and leaves *out as Null) on malformed input. A UTF-8 BOM is skipped. */
bool parse(const std::string& text, Value* out, const ParseOptions& opt = ParseOptions());
bool parse(const char* data, size_t len, Value* out, const ParseOptions& opt = ParseOptions());

/* ---- writer ---- */
std::string escape(const std::string& s);   // string body escaped, no surrounding quotes
std::string quote(const std::string& s);    // "\"" + escape(s) + "\""
/* Serialize a Value (compact). Non-finite numbers are written as null; int numbers keep their exact digits. */
std::string stringify(const Value& v);
/* Number formatting used by stringify (integers without a decimal point, doubles with up to 17 significant digits). */
std::string number_to_string(double d);

/* Convenience constructors for building values to stringify. */
Value make_null();
Value make_bool(bool b);
Value make_number(double d);
Value make_int(int64_t i);
Value make_string(const std::string& s);
Value make_array();
Value make_object();

}  // namespace json
