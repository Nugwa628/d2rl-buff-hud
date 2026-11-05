#pragma once

#include <string>

namespace BuffPanel::Core {

// Removes D2's colour codes from localized text: "ÿc" and the code character
// after it ("ÿc1Elixir of Power" -> "Elixir of Power"). The "ÿ" is UTF-8
// (C3 BF) in D2R's string tables; a lone 0xFF byte counts too.
inline void StripColourCodes(std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        std::size_t marker = 0;
        if (static_cast<unsigned char>(text[i]) == 0xC3 && i + 1 < text.size()
            && static_cast<unsigned char>(text[i + 1]) == 0xBF) {
            marker = 2;
        } else if (static_cast<unsigned char>(text[i]) == 0xFF) {
            marker = 1;
        }
        if (marker != 0 && i + marker < text.size() && text[i + marker] == 'c') {
            i += marker + 1;
            // The code is one ASCII character; a multi-byte one is left to show.
            if (i < text.size() && static_cast<unsigned char>(text[i]) < 0x80) ++i;
            continue;
        }
        out.push_back(text[i++]);
    }
    text.swap(out);
}

} // namespace BuffPanel::Core
