#include "core/plugin_config.hpp"

#include <cstdio>
#include <string>

using BuffPanel::Core::ParsePluginConfig;
using BuffPanel::Core::SetConfigValue;

int main() {
    int failures = 0;
    auto check = [&](bool condition, const char* what) {
        if (!condition) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
    };
    check(ParsePluginConfig(nullptr).enabled, "no config keeps the panel on");
    check(ParsePluginConfig("").enabled, "an empty config keeps the panel on");
    check(!ParsePluginConfig("[buff_panel]\nenabled = false\n").enabled, "enabled = false turns it off");
    check(!ParsePluginConfig("# c\r\n[buff_panel]\r\n  enabled=false   # off\r\n").enabled, "CRLF, comments and spacing");
    check(ParsePluginConfig("[buff_panel]\nenabled = true\n").enabled, "enabled = true");
    check(ParsePluginConfig("[other]\nenabled = false\n").enabled, "another section does not count");
    check(ParsePluginConfig("[buff_panel]\nenabled = no\n").enabled, "a bad value keeps the default");
    check(ParsePluginConfig("[buff_panel]\nenabled_x = false\n").enabled, "another key does not count");
    check(ParsePluginConfig("enabled = false\n").enabled, "a key outside any section does not count");
    check(ParsePluginConfig(nullptr).hideWithPanels, "no config hides the icons with big panels");
    check(ParsePluginConfig("[buff_panel]\nenabled = true\n").hideWithPanels, "an old config keeps the default");
    check(!ParsePluginConfig("[buff_panel]\nhide_with_panels = false\n").hideWithPanels, "hide_with_panels = false");
    check(ParsePluginConfig("[buff_panel]\nhide_with_panels = false\n").enabled, "hide_with_panels leaves enabled alone");
    check(!ParsePluginConfig("[buff_panel]\nenabled = false\nhide_with_panels = true\n").enabled,
        "enabled = false next to hide_with_panels");
    check(ParsePluginConfig("[buff_panel]\nhide_with_panels = 0\n").hideWithPanels, "a bad hide value keeps the default");
    check(ParsePluginConfig("[other]\nhide_with_panels = false\n").hideWithPanels, "hide_with_panels in another section");
    check(!ParsePluginConfig("").hdBuffs.IsSet(), "no position means the default spot");
    {
        const auto c = ParsePluginConfig("[buff_panel]\nhd_buffs_x = 0.25\nhd_buffs_y = 0.75\nlegacy_debuffs_x = 0.5 # c\n");
        check(c.hdBuffs.IsSet() && c.hdBuffs.x == 0.25F && c.hdBuffs.y == 0.75F, "hd buff position");
        check(!c.legacyDebuffs.IsSet() && c.legacyDebuffs.x == 0.5F, "half a position stays default");
        check(!c.hdDebuffs.IsSet() && !c.legacyBuffs.IsSet(), "other groups stay default");
    }
    check(!ParsePluginConfig("[buff_panel]\nhd_buffs_x = 2\nhd_buffs_y = 0.5\n").hdBuffs.IsSet(), "out of range");
    check(!ParsePluginConfig("[buff_panel]\nhd_buffs_x = abc\nhd_buffs_y = 0.5\n").hdBuffs.IsSet(), "not a number");
    check(ParsePluginConfig("[buff_panel]\nreset_buffs = true\n").resetBuffs, "reset_buffs");
    check(!ParsePluginConfig("[buff_panel]\nreset_buffs = true\n").resetDebuffs, "reset_debuffs stays off");
    {
        const std::string text = "# head\r\n[buff_panel]\r\nenabled = true\r\nhd_buffs_x = -1 # keep\r\n[other]\r\nx = 1\r\n";
        const auto replaced = SetConfigValue(text, "hd_buffs_x", "0.4");
        check(replaced == "# head\r\n[buff_panel]\r\nenabled = true\r\nhd_buffs_x = 0.4 # keep\r\n[other]\r\nx = 1\r\n",
            "replace keeps comment and CRLF");
        const auto added = SetConfigValue(text, "hd_buffs_y", "0.6");
        check(added == "# head\r\n[buff_panel]\r\nenabled = true\r\nhd_buffs_x = -1 # keep\r\nhd_buffs_y = 0.6\n[other]\r\nx = 1\r\n",
            "add at the end of the section");
        check(ParsePluginConfig(SetConfigValue(added, "hd_buffs_x", "0.4").c_str()).hdBuffs.IsSet(), "round trip");
        check(SetConfigValue("", "reset_buffs", "false") == "[buff_panel]\nreset_buffs = false\n", "empty text gets the section");
        check(SetConfigValue("[buff_panel]\nenabled = true", "x", "1") == "[buff_panel]\nenabled = true\nx = 1\n", "no final newline");
        check(SetConfigValue("[buff_panel]\nhd_buffs_xy = 1\n", "hd_buffs_x", "0") == "[buff_panel]\nhd_buffs_xy = 1\nhd_buffs_x = 0\n",
            "a longer key is another key");
    }
    if (failures == 0) std::printf("PASS: plugin config\n");
    return failures == 0 ? 0 : 1;
}
