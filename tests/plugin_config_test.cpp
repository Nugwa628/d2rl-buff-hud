#include "core/plugin_config.hpp"

#include <cstdio>

using BuffPanel::Core::ParsePluginConfig;

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
    if (failures == 0) std::printf("PASS: plugin config\n");
    return failures == 0 ? 0 : 1;
}
