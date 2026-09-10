#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace rm {

/// Logical keys the game currently binds. AppKit characters are used deliberately: this keeps the
/// existing keyboard-layout behavior and does not smuggle in a physical-key policy.
enum class Key : std::uint8_t {
    A,
    C,
    D,
    E,
    F,
    I,
    N,
    O,
    P,
    R,
    S,
    W,
    Space,
    Escape,
    Digit0,
    Digit1,
    Digit2,
    Digit3,
    Digit4,
    Digit5,
    Digit6,
    Digit7,
    Digit8,
    Digit9,
};

enum class KeyPhase : std::uint8_t { Press, Release };

struct KeyModifiers {
    bool shift = false;
    bool command = false;
    bool control = false;
};

/// One complete keyboard fact from the platform boundary.
struct KeyEvent {
    Key key;
    KeyPhase phase;
    bool repeat = false;
    KeyModifiers modifiers;
};

[[nodiscard]] constexpr std::optional<Key> keyForCharacter(char character) noexcept {
    switch (character) {
        case 'a': return Key::A;
        case 'c': return Key::C;
        case 'd': return Key::D;
        case 'e': return Key::E;
        case 'f': return Key::F;
        case 'i': return Key::I;
        case 'n': return Key::N;
        case 'o': return Key::O;
        case 'p': return Key::P;
        case 'r': return Key::R;
        case 's': return Key::S;
        case 'w': return Key::W;
        case ' ': return Key::Space;
        case '\x1b': return Key::Escape;
        case '0': return Key::Digit0;
        case '1': return Key::Digit1;
        case '2': return Key::Digit2;
        case '3': return Key::Digit3;
        case '4': return Key::Digit4;
        case '5': return Key::Digit5;
        case '6': return Key::Digit6;
        case '7': return Key::Digit7;
        case '8': return Key::Digit8;
        case '9': return Key::Digit9;
        default: return std::nullopt;
    }
}

[[nodiscard]] constexpr std::optional<KeyEvent> keyEventForCharacter(
    char character, KeyPhase phase, bool repeat, KeyModifiers modifiers = {}) noexcept {
    const std::optional<Key> key = keyForCharacter(character);
    if (!key) {
        return std::nullopt;
    }
    return KeyEvent{.key = *key, .phase = phase, .repeat = repeat, .modifiers = modifiers};
}

[[nodiscard]] constexpr std::optional<std::size_t> digitForKey(Key key) noexcept {
    switch (key) {
        case Key::Digit0: return 0;
        case Key::Digit1: return 1;
        case Key::Digit2: return 2;
        case Key::Digit3: return 3;
        case Key::Digit4: return 4;
        case Key::Digit5: return 5;
        case Key::Digit6: return 6;
        case Key::Digit7: return 7;
        case Key::Digit8: return 8;
        case Key::Digit9: return 9;
        default: return std::nullopt;
    }
}

} // namespace rm
