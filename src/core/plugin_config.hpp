#pragma once

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace BuffPanel::Core {

// Where a group of icons sits: its top-left corner as a fraction (0..1) of the
// game's UI screen. A negative value means the default spot by the globe.
struct PanelPosition final {
    float x{-1.0F};
    float y{-1.0F};
    [[nodiscard]] bool IsSet() const noexcept { return x >= 0.0F && y >= 0.0F; }
};

// buff-panel.toml is small: [buff_panel] enabled = true|false,
// hide_with_panels = true|false, the saved group positions (hd_buffs_x,
// hd_buffs_y, hd_debuffs_*, legacy_buffs_*, legacy_debuffs_*) and
// reset_buffs / reset_debuffs = true|false. A missing key, or a value of the
// wrong kind, keeps the default. Only lines inside [buff_panel] count; # starts
// a comment.
struct PluginConfig final {
    bool enabled{true};
    // Hide the icons while a big game panel (stash, cube, vendor, ...) is open.
    bool hideWithPanels{true};
    PanelPosition hdBuffs{};
    PanelPosition hdDebuffs{};
    PanelPosition legacyBuffs{};
    PanelPosition legacyDebuffs{};
    // Put the group back to its default spot (HD and legacy) at the next start.
    bool resetBuffs{};
    bool resetDebuffs{};
};

[[nodiscard]] inline PluginConfig ParsePluginConfig(const char* text) noexcept {
    PluginConfig config{};
    if (text == nullptr) return config;
    bool inSection = false;
    for (const char* line = text; *line != '\0';) {
        const char* end = line;
        while (*end != '\0' && *end != '\n') ++end;
        const char* s = line;
        while (s < end && (*s == ' ' || *s == '\t')) ++s;
        const char* e = s;
        while (e < end && *e != '#' && *e != '\r') ++e;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) --e;
        const auto length = static_cast<std::size_t>(e - s);
        auto is = [&](const char* literal) noexcept {
            return length == std::strlen(literal) && std::strncmp(s, literal, length) == 0;
        };
        if (length > 0 && *s == '[') {
            inSection = is("[buff_panel]");
        } else if (inSection && length > 0) {
            const char* eq = s;
            while (eq < e && *eq != '=') ++eq;
            if (eq < e) {
                const char* keyEnd = eq;
                while (keyEnd > s && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t')) --keyEnd;
                const char* value = eq + 1;
                while (value < e && (*value == ' ' || *value == '\t')) ++value;
                const auto keyLength = static_cast<std::size_t>(keyEnd - s);
                const auto valueLength = static_cast<std::size_t>(e - value);
                auto keyIs = [&](const char* key) noexcept {
                    return keyLength == std::strlen(key) && std::strncmp(s, key, keyLength) == 0;
                };
                auto setBool = [&](const char* key, bool& target) noexcept {
                    if (!keyIs(key)) return;
                    if (valueLength == 4 && std::strncmp(value, "true", 4) == 0) target = true;
                    if (valueLength == 5 && std::strncmp(value, "false", 5) == 0) target = false;
                };
                auto setFloat = [&](const char* key, float& target) noexcept {
                    if (!keyIs(key) || valueLength == 0 || valueLength >= 32) return;
                    char number[32]{};
                    std::memcpy(number, value, valueLength);
                    char* parsedEnd = nullptr;
                    const double parsed = std::strtod(number, &parsedEnd);
                    if (parsedEnd != number + valueLength || !(parsed >= -1.0 && parsed <= 1.0)) return;
                    target = static_cast<float>(parsed);
                };
                setBool("enabled", config.enabled);
                setBool("hide_with_panels", config.hideWithPanels);
                setBool("reset_buffs", config.resetBuffs);
                setBool("reset_debuffs", config.resetDebuffs);
                setFloat("hd_buffs_x", config.hdBuffs.x);
                setFloat("hd_buffs_y", config.hdBuffs.y);
                setFloat("hd_debuffs_x", config.hdDebuffs.x);
                setFloat("hd_debuffs_y", config.hdDebuffs.y);
                setFloat("legacy_buffs_x", config.legacyBuffs.x);
                setFloat("legacy_buffs_y", config.legacyBuffs.y);
                setFloat("legacy_debuffs_x", config.legacyDebuffs.x);
                setFloat("legacy_debuffs_y", config.legacyDebuffs.y);
            }
        }
        line = *end == '\n' ? end + 1 : end;
    }
    return config;
}

// Sets `key = value` inside [buff_panel] of a buff-panel.toml text: replaces the
// value of an existing line (keeping its comment and line ending) or adds the
// line at the end of the section. Adds the section when the text has none.
[[nodiscard]] inline std::string SetConfigValue(std::string_view text, std::string_view key, std::string_view value) {
    auto isBlank = [](char c) noexcept { return c == ' ' || c == '\t'; };
    std::string out;
    out.reserve(text.size() + key.size() + value.size() + 16);
    bool inSection = false;
    bool sectionSeen = false;
    bool written = false;
    std::size_t sectionEnd = std::string::npos;
    std::size_t position = 0;
    while (position < text.size()) {
        auto end = text.find('\n', position);
        const bool lastLine = end == std::string_view::npos;
        if (lastLine) end = text.size();
        const std::string_view line = text.substr(position, end - position);
        std::string_view body = line;
        while (!body.empty() && isBlank(body.front())) body.remove_prefix(1);
        bool replaced = false;
        if (!body.empty() && body.front() == '[') {
            if (inSection && sectionEnd == std::string::npos) sectionEnd = out.size();
            std::string_view header = body;
            while (!header.empty() && (header.back() == '\r' || isBlank(header.back()))) header.remove_suffix(1);
            inSection = header == "[buff_panel]";
            sectionSeen = sectionSeen || inSection;
        } else if (inSection && !written && body.substr(0, key.size()) == key) {
            std::string_view rest = body.substr(key.size());
            while (!rest.empty() && isBlank(rest.front())) rest.remove_prefix(1);
            if (!rest.empty() && rest.front() == '=') {
                const auto comment = line.find('#');
                const bool crlf = !line.empty() && line.back() == '\r';
                out.append(key).append(" = ").append(value);
                if (comment != std::string_view::npos) {
                    std::string_view tail = line.substr(comment);
                    if (crlf) tail.remove_suffix(1);
                    out.append(" ").append(tail);
                }
                if (crlf) out.push_back('\r');
                written = true;
                replaced = true;
            }
        }
        if (!replaced) out.append(line);
        if (!lastLine) out.push_back('\n');
        position = lastLine ? text.size() : end + 1;
    }
    if (written) return out;
    std::string added;
    added.append(key).append(" = ").append(value).push_back('\n');
    if (inSection) {
        if (!out.empty() && out.back() != '\n') out.push_back('\n');
        out.append(added);
    } else if (sectionEnd != std::string::npos) {
        out.insert(sectionEnd, added);
    } else {
        if (!out.empty() && out.back() != '\n') out.push_back('\n');
        if (!sectionSeen) out.append("[buff_panel]\n");
        out.append(added);
    }
    return out;
}

} // namespace BuffPanel::Core
