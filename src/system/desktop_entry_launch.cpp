#include "system/desktop_entry_launch.h"

#include "core/log.h"
#include "core/process.h"
#include "util/file_utils.h"

#include <array>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <utility>

namespace {

  const Logger log("desktop_entry_launch");

  std::string stripFieldCodes(std::string_view exec) {
    std::string result;
    result.reserve(exec.size());
    for (std::size_t i = 0; i < exec.size(); ++i) {
      if (exec[i] == '%' && i + 1 < exec.size()) {
        const char next = exec[i + 1];
        if (next == 'f'
            || next == 'F'
            || next == 'u'
            || next == 'U'
            || next == 'd'
            || next == 'D'
            || next == 'n'
            || next == 'N'
            || next == 'i'
            || next == 'c'
            || next == 'k') {
          ++i;
          if (i + 1 < exec.size() && exec[i + 1] == ' ') {
            ++i;
          }
          continue;
        }
        if (next == '%') {
          result += '%';
          ++i;
          continue;
        }
      }
      result += exec[i];
    }

    while (!result.empty() && result.back() == ' ') {
      result.pop_back();
    }
    return result;
  }

  std::vector<std::string> tokenize(std::string_view cmd) {
    std::vector<std::string> args;
    std::string current;
    bool inSingle = false;
    bool inDouble = false;

    for (const char c : cmd) {
      if (c == '\'' && !inDouble) {
        inSingle = !inSingle;
        continue;
      }
      if (c == '"' && !inSingle) {
        inDouble = !inDouble;
        continue;
      }
      if (c == ' ' && !inSingle && !inDouble) {
        if (!current.empty()) {
          args.push_back(std::move(current));
          current.clear();
        }
        continue;
      }
      current += c;
    }
    if (!current.empty()) {
      args.push_back(std::move(current));
    }
    return args;
  }

  std::string expandExecutablePath(std::string_view binary) {
    if (binary.empty() || binary.front() != '~') {
      return std::string(binary);
    }
    return FileUtils::expandUserPath(std::string(binary)).string();
  }

  bool isExecutableOnPath(std::string_view binary) {
    if (binary.empty()) {
      return false;
    }
    if (binary.find('/') != std::string_view::npos) {
      const std::string expanded = expandExecutablePath(binary);
      return access(expanded.c_str(), X_OK) == 0;
    }

    const char* pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr || pathEnv[0] == '\0') {
      return false;
    }

    std::string_view path(pathEnv);
    std::size_t start = 0;
    while (start <= path.size()) {
      const auto sep = path.find(':', start);
      const auto segment = sep == std::string_view::npos ? path.substr(start) : path.substr(start, sep - start);
      if (!segment.empty()) {
        std::string candidate(segment);
        candidate.push_back('/');
        candidate.append(binary);
        if (access(candidate.c_str(), X_OK) == 0) {
          return true;
        }
      }
      if (sep == std::string_view::npos) {
        break;
      }
      start = sep + 1;
    }
    return false;
  }

  std::vector<std::string> discoverTerminal(const desktop_entry_launch::PrepareOptions& options) {
    if (!options.terminalCandidates.empty()) {
      for (const auto& candidate : options.terminalCandidates) {
        std::vector<std::string> terminal = tokenize(candidate);
        if (!terminal.empty() && isExecutableOnPath(terminal.front())) {
          return terminal;
        }
      }
      return {};
    }
    if (!options.useSystemTerminalDiscovery) {
      return {};
    }

    if (const char* envTerminal = std::getenv("TERMINAL"); envTerminal != nullptr && envTerminal[0] != '\0') {
      std::vector<std::string> terminal = tokenize(envTerminal);
      if (!terminal.empty() && isExecutableOnPath(terminal.front())) {
        return terminal;
      }
    }

    static constexpr std::array<std::string_view, 11> kTerminalCandidates = {
        "x-terminal-emulator", "ghostty", "kitty",  "alacritty", "wezterm", "foot", "konsole",
        "gnome-terminal",      "kgx",     "ptyxis", "xterm",
    };
    for (const auto candidate : kTerminalCandidates) {
      if (isExecutableOnPath(candidate)) {
        return {std::string(candidate)};
      }
    }
    return {};
  }

  bool usesCommandSeparator(std::string_view terminal) {
    return terminal == "gnome-terminal" || terminal == "kgx" || terminal == "ptyxis";
  }

  std::vector<std::string>
  terminalLaunchArgs(std::string_view command, const desktop_entry_launch::PrepareOptions& options) {
    std::vector<std::string> terminal = discoverTerminal(options);
    if (terminal.empty()) {
      return {};
    }

    const std::string termBin = terminal.front();
    if (usesCommandSeparator(termBin)) {
      terminal.emplace_back("--");
    } else {
      terminal.emplace_back("-e");
    }
    terminal.emplace_back("sh");
    terminal.emplace_back("-lc");
    terminal.emplace_back(command);
    return terminal;
  }

  std::string appNameOrDefault(std::string_view appName) {
    return appName.empty() ? "desktop-entry" : std::string(appName);
  }

} // namespace

namespace desktop_entry_launch {

  std::optional<PreparedCommand> prepareCommand(std::string_view exec, bool terminal, const PrepareOptions& options) {
    std::string cleanExec = stripFieldCodes(exec);
    std::vector<std::string> args = terminal ? terminalLaunchArgs(cleanExec, options) : tokenize(cleanExec);

    if (!args.empty() && args.front().find('/') != std::string::npos) {
      args.front() = expandExecutablePath(args.front());
    }

    if (args.empty()) {
      return std::nullopt;
    }
    return PreparedCommand{std::move(args)};
  }

  bool launchEntry(const DesktopEntry& entry, const LaunchOptions& options) {
    auto prepared = prepareCommand(entry.exec, entry.terminal);
    if (!prepared.has_value()) {
      log.warn("Failed to prepare launch command for desktop entry '{}'", entry.id.empty() ? entry.name : entry.id);
      return false;
    }

    const std::string appName = !entry.id.empty() ? entry.id : appNameOrDefault(entry.name);
    if (options.runAsSystemdService) {
      process::runAsyncAsSystemdService(prepared->args, appName, options.activationToken, entry.workingDir);
      return true;
    }
    return process::runAsync(prepared->args, options.activationToken, entry.workingDir);
  }

  bool launchAction(
      const DesktopAction& action, std::string_view appName, std::string_view workingDir, bool terminal,
      const LaunchOptions& options
  ) {
    auto prepared = prepareCommand(action.exec, terminal);
    if (!prepared.has_value()) {
      log.warn("Failed to prepare launch command for desktop action '{}'", action.id.empty() ? action.name : action.id);
      return false;
    }

    if (options.runAsSystemdService) {
      process::runAsyncAsSystemdService(
          prepared->args, appNameOrDefault(appName), options.activationToken, std::string(workingDir)
      );
      return true;
    }
    return process::runAsync(prepared->args, options.activationToken, std::string(workingDir));
  }

} // namespace desktop_entry_launch
