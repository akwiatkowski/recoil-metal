#include "core/lua/LuaTable.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace {

using rm::lua::ParseError;
using rm::lua::Value;

// Recursive-descent scanner over a Lua source buffer.
//
// Only ever *reads* values. Any construct that would require evaluation is
// reported as an error rather than approximated — see LuaTable.hpp.
class Scanner {
public:
    explicit Scanner(std::string_view source) : source_{source} {}

    /// Skips ahead to the first table literal and parses it.
    [[nodiscard]] std::expected<Value, ParseError> parseFirstTable() {
        skipTrivia();
        while (pos_ < source_.size() && source_[pos_] != '{') {
            advance();
            skipTrivia();
        }
        if (pos_ >= source_.size()) {
            return fail("no table literal found");
        }
        return parseTable();
    }

private:
    std::string_view source_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;

    [[nodiscard]] std::unexpected<ParseError> fail(std::string message) const {
        return std::unexpected(
            ParseError{"line " + std::to_string(line_) + ": " + std::move(message), line_});
    }

    [[nodiscard]] bool atEnd() const noexcept { return pos_ >= source_.size(); }
    [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept {
        return pos_ + ahead < source_.size() ? source_[pos_ + ahead] : '\0';
    }

    void advance() noexcept {
        if (pos_ < source_.size() && source_[pos_] == '\n') {
            ++line_;
        }
        ++pos_;
    }

    void advanceBy(std::size_t count) noexcept {
        for (std::size_t i = 0; i < count && !atEnd(); ++i) {
            advance();
        }
    }

    /// Whitespace plus both comment forms.
    void skipTrivia() noexcept {
        while (!atEnd()) {
            const char c = peek();
            if (std::isspace(static_cast<unsigned char>(c)) != 0) {
                advance();
                continue;
            }
            if (c == '-' && peek(1) == '-') {
                advanceBy(2);
                // --[[ long comment ]] versus -- line comment.
                if (peek() == '[' && (peek(1) == '[' || peek(1) == '=')) {
                    skipLongBracket();
                } else {
                    while (!atEnd() && peek() != '\n') {
                        advance();
                    }
                }
                continue;
            }
            return;
        }
    }

    /// Steps over a [[...]] / [==[...]==] region, leaving pos_ just past it.
    /// Returns the content when one was present.
    std::optional<std::string> skipLongBracket() {
        if (peek() != '[') {
            return std::nullopt;
        }

        std::size_t level = 0;
        while (peek(1 + level) == '=') {
            ++level;
        }
        if (peek(1 + level) != '[') {
            return std::nullopt;
        }
        advanceBy(2 + level);

        // Lua drops a newline immediately after the opening bracket.
        if (peek() == '\n') {
            advance();
        }

        const std::size_t start = pos_;
        while (!atEnd()) {
            if (peek() == ']') {
                std::size_t closing = 0;
                while (peek(1 + closing) == '=') {
                    ++closing;
                }
                if (closing == level && peek(1 + closing) == ']') {
                    std::string content{source_.substr(start, pos_ - start)};
                    advanceBy(2 + level);
                    return content;
                }
            }
            advance();
        }
        return std::string{source_.substr(start)};
    }

    [[nodiscard]] static bool isNameStart(char c) noexcept {
        return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
    }
    [[nodiscard]] static bool isNameChar(char c) noexcept {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    }

    [[nodiscard]] std::string readName() {
        const std::size_t start = pos_;
        while (!atEnd() && isNameChar(peek())) {
            advance();
        }
        return std::string{source_.substr(start, pos_ - start)};
    }

    [[nodiscard]] std::expected<std::string, ParseError> readQuotedString() {
        const char quote = peek();
        advance();

        std::string out;
        while (!atEnd() && peek() != quote) {
            if (peek() == '\\') {
                advance();
                const char escape = peek();
                switch (escape) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case '\\': out.push_back('\\'); break;
                    case '"': out.push_back('"'); break;
                    case '\'': out.push_back('\''); break;
                    default: out.push_back(escape); break;
                }
                advance();
                continue;
            }
            out.push_back(peek());
            advance();
        }

        if (atEnd()) {
            return fail("unterminated string");
        }
        advance();  // closing quote
        return out;
    }

    [[nodiscard]] std::expected<Value, ParseError> parseNumber() {
        const std::size_t start = pos_;
        if (peek() == '-' || peek() == '+') {
            advance();
        }

        // strtod handles decimal, exponent and 0x hex forms, which covers every
        // numeric literal Lua 5.x accepts here.
        const std::string buffer{source_.substr(start)};
        char* parseEnd = nullptr;
        const double parsed = std::strtod(buffer.c_str(), &parseEnd);
        if (parseEnd == buffer.c_str()) {
            return fail("expected a number");
        }

        pos_ = start;
        advanceBy(static_cast<std::size_t>(parseEnd - buffer.c_str()));

        Value value;
        value.type = Value::Type::Number;
        value.number = parsed;
        return value;
    }

    [[nodiscard]] std::expected<Value, ParseError> parseValue() {
        skipTrivia();
        if (atEnd()) {
            return fail("unexpected end of input");
        }

        const char c = peek();

        if (c == '{') {
            return parseTable();
        }

        if (c == '"' || c == '\'') {
            auto text = readQuotedString();
            if (!text) {
                return std::unexpected(text.error());
            }
            Value value;
            value.type = Value::Type::Text;
            value.text = std::move(*text);
            return value;
        }

        if (c == '[') {
            if (auto content = skipLongBracket()) {
                Value value;
                value.type = Value::Type::Text;
                value.text = std::move(*content);
                return value;
            }
            return fail("unexpected '['");
        }

        if ((std::isdigit(static_cast<unsigned char>(c)) != 0) || c == '-' || c == '+'
            || (c == '.' && std::isdigit(static_cast<unsigned char>(peek(1))) != 0)) {
            return parseNumber();
        }

        if (isNameStart(c)) {
            const std::size_t nameStart = pos_;
            const std::string name = readName();

            if (name == "true" || name == "false") {
                Value value;
                value.type = Value::Type::Bool;
                value.boolean = (name == "true");
                return value;
            }
            if (name == "nil") {
                return Value{};
            }

            // An identifier here means a variable, a call, or an expression —
            // all of which need evaluation. Refuse rather than guess.
            pos_ = nameStart;
            return fail("value '" + name
                        + "' requires evaluation; this reader parses data, not expressions");
        }

        return fail(std::string{"unexpected character '"} + c + "'");
    }

    [[nodiscard]] std::expected<Value, ParseError> parseTable() {
        advance();  // '{'

        Value table;
        table.type = Value::Type::Table;

        while (true) {
            skipTrivia();
            if (atEnd()) {
                return fail("unterminated table");
            }
            if (peek() == '}') {
                advance();
                return table;
            }

            std::optional<std::string> key;

            if (peek() == '[') {
                // [expr] = value. Only literal keys are supported.
                advance();
                skipTrivia();
                if (peek() == '"' || peek() == '\'') {
                    auto text = readQuotedString();
                    if (!text) {
                        return std::unexpected(text.error());
                    }
                    key = std::move(*text);
                } else {
                    auto index = parseNumber();
                    if (!index) {
                        return std::unexpected(index.error());
                    }
                    // Stored under its decimal spelling — documented collision
                    // between [0] and ["0"], harmless for map metadata.
                    std::string spelled = std::to_string(index->number);
                    while (spelled.size() > 1 && spelled.back() == '0') {
                        spelled.pop_back();
                    }
                    if (!spelled.empty() && spelled.back() == '.') {
                        spelled.pop_back();
                    }
                    key = spelled;
                }
                skipTrivia();
                if (peek() != ']') {
                    return fail("expected ']' after a bracketed key");
                }
                advance();
                skipTrivia();
                if (peek() != '=') {
                    return fail("expected '=' after a bracketed key");
                }
                advance();
            } else if (isNameStart(peek())) {
                // Either `name = value` or a bare identifier value. Look ahead
                // for the '=' before committing, and do not treat '==' as one.
                const std::size_t save = pos_;
                const std::size_t saveLine = line_;
                const std::string name = readName();
                skipTrivia();
                if (peek() == '=' && peek(1) != '=') {
                    advance();
                    key = name;
                } else {
                    pos_ = save;
                    line_ = saveLine;
                }
            }

            auto value = parseValue();
            if (!value) {
                return std::unexpected(value.error());
            }

            if (key.has_value()) {
                table.fields.push_back(rm::lua::Field{std::move(*key), std::move(*value)});
            } else {
                table.items.push_back(std::move(*value));
            }

            skipTrivia();
            if (atEnd()) {
                // Distinct from a stray token: the table simply never closed.
                return fail("unterminated table");
            }
            if (peek() == ',' || peek() == ';') {
                advance();
            } else if (peek() != '}') {
                return fail("expected ',' or '}' in table");
            }
        }
    }
};

} // namespace

namespace rm::lua {

std::expected<Value, ParseError> parseTable(std::string_view source) {
    Scanner scanner{source};
    return scanner.parseFirstTable();
}

std::expected<Value, ParseError> parseTableFile(std::string_view path) {
    std::ifstream in{std::string{path}, std::ios::binary};
    if (!in) {
        return std::unexpected(ParseError{"could not open \"" + std::string{path} + "\"", 0});
    }

    const std::string contents{std::istreambuf_iterator<char>{in},
                               std::istreambuf_iterator<char>{}};
    return parseTable(contents);
}

} // namespace rm::lua
