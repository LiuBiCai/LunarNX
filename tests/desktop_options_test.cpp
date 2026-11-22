#include "../src/platform/desktop_options.h"
#include <cstdio>

int main() {
    using lunar::desktop::Options;
    using lunar::desktop::parseOptions;
    using lunar::desktop::selectConsole;

    Options options;
    std::string error;
    if (!parseOptions({"app", "--profile", "account-1", "--console", "Xbox One"},
                      options, error) ||
        options.profile != "account-1" || options.console != "Xbox One") return 1;
    Options email_profile;
    if (!parseOptions({"app", "--profile", "account+1@example.invalid"},
                      email_profile, error)) return 7;
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"app", "--profile"}, {"app", "--profile", "../other"},
             {"app", "--profile", "."}, {"app", "--profile", ".."},
             {"app", "--profile", "account."}, {"app", "--profile", "con"},
             {"app", "--profile", "Lpt1.txt"},
             {"app", "--profile", "a", "--profile", "b"},
             {"app", "--console", ""}, {"app", "--unknown"}}) {
        Options invalid;
        if (parseOptions(args, invalid, error)) return 2;
    }

    std::vector<lunar::api::XboxConsole> consoles(3);
    consoles[0].id = "A";
    consoles[0].name = "Living Room";
    consoles[1].id = "B";
    consoles[1].name = "Living Room";
    consoles[2].id = "C";
    consoles[2].name = u8"\u6e38\u620f\u673a";
    size_t selected = 0;
    if (!selectConsole(consoles, "B", selected, error) || selected != 1) return 3;
    if (!selectConsole(consoles, u8"\u6e38\u620f\u673a", selected, error) || selected != 2) return 4;
    if (selectConsole(consoles, "Living Room", selected, error)) return 5;
    if (selectConsole(consoles, "Missing", selected, error)) return 6;

    std::puts("Desktop option and console selection tests passed");
    return 0;
}
