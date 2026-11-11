#pragma once

// Minimal command-line parsing for the tools: "--name value" options and "--name" flags.

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace itchbt {

class Args {
public:
    Args(int argc, const char* const* argv) {
        for (int i = 1; i < argc; ++i) {
            args_.emplace_back(argv[i]); // NOLINT(*-pointer-arithmetic)
        }
        used_.assign(args_.size(), false);
    }

    /// The value following `--name`, if present.
    [[nodiscard]] std::optional<std::string_view> value(std::string_view name) {
        for (std::size_t i = 0; i + 1 < args_.size(); ++i) {
            if (args_[i] == name) {
                used_[i] = used_[i + 1] = true;
                return args_[i + 1];
            }
        }
        return std::nullopt;
    }

    /// True if `--name` is present.
    [[nodiscard]] bool flag(std::string_view name) {
        for (std::size_t i = 0; i < args_.size(); ++i) {
            if (args_[i] == name) {
                used_[i] = true;
                return true;
            }
        }
        return false;
    }

    /// The value of `--name` as an unsigned integer. Sets `bad` if present but malformed.
    [[nodiscard]] std::optional<std::uint64_t> number(std::string_view name, bool& bad) {
        const auto text = value(name);
        if (!text) {
            return std::nullopt;
        }
        std::uint64_t result = 0;
        const auto [end, ec] = std::from_chars(text->data(), text->data() + text->size(), result);
        if (ec != std::errc{} || end != text->data() + text->size()) {
            bad = true;
            return std::nullopt;
        }
        return result;
    }

    /// Arguments no accessor has claimed; call after reading every expected option.
    [[nodiscard]] std::vector<std::string> unused() const {
        std::vector<std::string> out;
        for (std::size_t i = 0; i < args_.size(); ++i) {
            if (!used_[i]) {
                out.push_back(args_[i]);
            }
        }
        return out;
    }

private:
    std::vector<std::string> args_;
    std::vector<bool> used_;
};

/// Runs a tool's body and turns an escaping exception (in practice only std::bad_alloc or a
/// filesystem error during start-up) into a message and a non-zero exit code.
template <typename Body>
int run_tool(const char* name, Body&& body) noexcept {
    try {
        return body();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", name, e.what());
    } catch (...) {
        std::fprintf(stderr, "%s: unknown error\n", name);
    }
    return 1;
}

/// Splits "A,B,C" into its non-empty parts.
[[nodiscard]] inline std::vector<std::string> split_commas(std::string_view text) {
    std::vector<std::string> parts;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view part = text.substr(0, comma);
        if (!part.empty()) {
            parts.emplace_back(part);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    return parts;
}

} // namespace itchbt
