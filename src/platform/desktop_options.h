#pragma once

#include "../api/xbox_api_client.h"
#include <string>
#include <vector>

namespace lunar::desktop {

struct Options {
    std::string profile;
    std::string console;
    bool help = false;
};

inline bool parseOptions(const std::vector<std::string>& args, Options& options,
                         std::string& error) {
    for (size_t i = 1; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--help" || arg == "-h") {
            options.help = true;
        } else if (arg == "--profile" || arg == "--console") {
            auto& value = arg == "--profile" ? options.profile : options.console;
            if (!value.empty() || i + 1 == args.size() || args[i + 1].empty() ||
                args[i + 1].rfind("--", 0) == 0) {
                error = "Missing or duplicate value for " + arg + ".";
                return false;
            }
            value = args[++i];
        } else {
            error = "Unknown argument: " + arg;
            return false;
        }
    }
    if (!options.profile.empty()) {
        if (options.profile.size() > 64) {
            error = "Profile name must be at most 64 characters.";
            return false;
        }
        if (options.profile.back() == '.') {
            error = "Profile name must not end with '.'.";
            return false;
        }
        for (const char c : options.profile) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                  c == '.' || c == '@' || c == '+')) {
                error = "Profile name may only contain ASCII letters, digits, '_', '-', '.', '@' and '+'.";
                return false;
            }
        }
        std::string device = options.profile.substr(0, options.profile.find('.'));
        for (char& c : device) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
        if (device == "CON" || device == "PRN" || device == "AUX" ||
            device == "NUL" ||
            (device.size() == 4 &&
             (device.compare(0, 3, "COM") == 0 || device.compare(0, 3, "LPT") == 0) &&
             device[3] >= '1' && device[3] <= '9')) {
            error = "Profile name is a reserved Windows device name.";
            return false;
        }
    }
    return true;
}

inline bool selectConsole(const std::vector<api::XboxConsole>& consoles,
                          const std::string& selector, size_t& selected,
                          std::string& error) {
    for (size_t i = 0; i < consoles.size(); ++i) {
        if (consoles[i].id == selector) {
            selected = i;
            return true;
        }
    }
    bool found = false;
    for (size_t i = 0; i < consoles.size(); ++i) {
        if (consoles[i].name != selector) continue;
        if (found) {
            error = "Multiple consoles have that name; specify the console ID.";
            return false;
        }
        selected = i;
        found = true;
    }
    if (!found) error = "Console not found; check the name or ID shown in the list.";
    return found;
}

} // namespace lunar::desktop
