#include <BWAPI/Runtime/RuntimeProcess.h>

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <ApplicationServices/ApplicationServices.h>
#endif

namespace
{
  void printUsage()
  {
    std::cout
      << "usage: starcraft-runtime-input --process-id <pid> --keys <sequence> [options]\n"
      << "  --process-id <pid>      target StarCraft process id\n"
      << "  --keys <sequence>       comma/space separated key tokens, e.g. \"s enter c\"\n"
      << "  --delay-ms <ms>         delay between keys (default: 150)\n"
      << "  --post-timeout-ms <ms>  maximum time to wait for each key post (default: 3000)\n"
      << "  --allow-untrusted       send even when macOS Accessibility trust is unavailable\n"
      << "  --dry-run               parse and print tokens without sending input\n"
      << "  --help                  show this help\n";
  }

  bool parsePositiveInt(const std::string& value, int& output)
  {
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed <= 0 || parsed > std::numeric_limits<int>::max())
      return false;
    output = static_cast<int>(parsed);
    return true;
  }

  std::string lower(std::string value)
  {
    for (char& ch : value)
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
  }

  std::vector<std::string> splitKeySequence(const std::string& sequence)
  {
    std::vector<std::string> tokens;
    std::string current;
    for (char ch : sequence)
    {
      if (std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == ',')
      {
        if (!current.empty())
        {
          tokens.push_back(lower(current));
          current.clear();
        }
        continue;
      }
      current.push_back(ch);
    }
    if (!current.empty())
      tokens.push_back(lower(current));
    return tokens;
  }

#if defined(__APPLE__)
  bool macVirtualKeyCode(const std::string& token, CGKeyCode& code)
  {
    if (token.size() == 1)
    {
      switch (token[0])
      {
      case 'a': code = 0; return true;
      case 's': code = 1; return true;
      case 'd': code = 2; return true;
      case 'f': code = 3; return true;
      case 'h': code = 4; return true;
      case 'g': code = 5; return true;
      case 'z': code = 6; return true;
      case 'x': code = 7; return true;
      case 'c': code = 8; return true;
      case 'v': code = 9; return true;
      case 'b': code = 11; return true;
      case 'q': code = 12; return true;
      case 'w': code = 13; return true;
      case 'e': code = 14; return true;
      case 'r': code = 15; return true;
      case 'y': code = 16; return true;
      case 't': code = 17; return true;
      case '1': code = 18; return true;
      case '2': code = 19; return true;
      case '3': code = 20; return true;
      case '4': code = 21; return true;
      case '6': code = 22; return true;
      case '5': code = 23; return true;
      case '9': code = 25; return true;
      case '7': code = 26; return true;
      case '8': code = 28; return true;
      case '0': code = 29; return true;
      case 'o': code = 31; return true;
      case 'u': code = 32; return true;
      case 'i': code = 34; return true;
      case 'p': code = 35; return true;
      case 'l': code = 37; return true;
      case 'j': code = 38; return true;
      case 'k': code = 40; return true;
      case 'n': code = 45; return true;
      case 'm': code = 46; return true;
      default: break;
      }
    }

    if (token == "enter" || token == "return")
    {
      code = 36;
      return true;
    }
    if (token == "tab")
    {
      code = 48;
      return true;
    }
    if (token == "space")
    {
      code = 49;
      return true;
    }
    if (token == "escape" || token == "esc")
    {
      code = 53;
      return true;
    }
    if (token == "left")
    {
      code = 123;
      return true;
    }
    if (token == "right")
    {
      code = 124;
      return true;
    }
    if (token == "down")
    {
      code = 125;
      return true;
    }
    if (token == "up")
    {
      code = 126;
      return true;
    }

    return false;
  }

  bool postKeyToPid(int processId, CGKeyCode code, std::string& reason)
  {
    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    if (source == nullptr)
    {
      reason = "CGEventSourceCreate failed";
      return false;
    }

    CGEventRef keyDown = CGEventCreateKeyboardEvent(source, code, true);
    CGEventRef keyUp = CGEventCreateKeyboardEvent(source, code, false);
    if (keyDown == nullptr || keyUp == nullptr)
    {
      if (keyDown != nullptr)
        CFRelease(keyDown);
      if (keyUp != nullptr)
        CFRelease(keyUp);
      CFRelease(source);
      reason = "CGEventCreateKeyboardEvent failed";
      return false;
    }

    CGEventPostToPid(static_cast<pid_t>(processId), keyDown);
    CGEventPostToPid(static_cast<pid_t>(processId), keyUp);
    CFRelease(keyDown);
    CFRelease(keyUp);
    CFRelease(source);
    return true;
  }

  bool postKeyToPidWithTimeout(
    int processId,
    CGKeyCode code,
    int timeoutMs,
    std::string& reason)
  {
    auto completion = std::make_shared<std::promise<std::pair<bool, std::string>>>();
    std::future<std::pair<bool, std::string>> future = completion->get_future();
    std::thread(
      [completion, processId, code]()
      {
        std::string postReason;
        const bool posted = postKeyToPid(processId, code, postReason);
        try
        {
          completion->set_value({ posted, postReason });
        }
        catch (const std::future_error&)
        {
        }
      }).detach();

    if (future.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready)
    {
      reason = "timed out while posting keyboard input to target process";
      return false;
    }

    const std::pair<bool, std::string> result = future.get();
    reason = result.second;
    return result.first;
  }
#endif
}

int main(int argc, char** argv)
{
  int processId = 0;
  int delayMs = 150;
  int postTimeoutMs = 3000;
  bool dryRun = false;
  bool allowUntrusted = false;
  std::string sequence;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h")
    {
      printUsage();
      return 0;
    }
    if (arg == "--dry-run")
    {
      dryRun = true;
      continue;
    }
    if (arg == "--allow-untrusted")
    {
      allowUntrusted = true;
      continue;
    }
    if (arg == "--process-id")
    {
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], processId))
      {
        std::cerr << "--process-id requires a positive integer\n";
        return 64;
      }
      continue;
    }
    if (arg == "--keys")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--keys requires a sequence\n";
        return 64;
      }
      sequence = argv[++i];
      continue;
    }
    if (arg == "--delay-ms")
    {
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], delayMs))
      {
        std::cerr << "--delay-ms requires a positive integer\n";
        return 64;
      }
      continue;
    }
    if (arg == "--post-timeout-ms")
    {
      if (i + 1 >= argc || !parsePositiveInt(argv[++i], postTimeoutMs))
      {
        std::cerr << "--post-timeout-ms requires a positive integer\n";
        return 64;
      }
      continue;
    }

    std::cerr << "unknown argument: " << arg << '\n';
    return 64;
  }

  if (processId <= 0)
  {
    std::cerr << "process id is required\n";
    return 64;
  }
  if (sequence.empty())
  {
    std::cerr << "key sequence is required\n";
    return 64;
  }

  const std::vector<std::string> tokens = splitKeySequence(sequence);
  if (tokens.empty())
  {
    std::cerr << "key sequence did not contain any tokens\n";
    return 64;
  }

  std::cout << "input.process_id=" << processId << '\n';
  std::cout << "input.token_count=" << tokens.size() << '\n';
  std::cout << "input.post_timeout_ms=" << postTimeoutMs << '\n';
  for (std::size_t i = 0; i < tokens.size(); ++i)
    std::cout << "input.token." << i << '=' << tokens[i] << '\n';
  const bool processVisible = BWAPI::Runtime::runtimeProcessExists(processId);
  std::cout << "input.process_visible=" << (processVisible ? "true" : "false") << '\n';
  if (!processVisible)
  {
    std::cout << "input.sent=0\n";
    std::cout << "input.success=false\n";
    std::cout << "input.reason=target process is not visible\n";
    return 66;
  }

#if defined(__APPLE__)
  std::vector<CGKeyCode> keyCodes;
  for (const std::string& token : tokens)
  {
    CGKeyCode code = 0;
    if (!macVirtualKeyCode(token, code))
    {
      std::cerr << "unsupported key token for macOS: " << token << '\n';
      return 65;
    }
    keyCodes.push_back(code);
  }

  if (dryRun)
  {
    std::cout << "input.dry_run=true\n";
    return 0;
  }

  const bool accessibilityTrusted = AXIsProcessTrusted();
  std::cout << "input.accessibility_trusted="
            << (accessibilityTrusted ? "true" : "false") << '\n';
  if (!accessibilityTrusted && !allowUntrusted)
  {
    std::cout << "input.sent=0\n";
    std::cout << "input.success=false\n";
    std::cout << "input.reason=macOS Accessibility trust is required to post keyboard input\n";
    return 3;
  }

  int sent = 0;
  for (CGKeyCode code : keyCodes)
  {
    std::string reason;
    if (!postKeyToPidWithTimeout(processId, code, postTimeoutMs, reason))
    {
      std::cout << "input.sent=" << sent << '\n';
      std::cout << "input.success=false\n";
      std::cout << "input.reason=" << reason << '\n';
      return 2;
    }
    ++sent;
    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
  }
  std::cout << "input.sent=" << sent << '\n';
  std::cout << "input.success=true\n";
  return 0;
#else
  (void)dryRun;
  (void)delayMs;
  (void)allowUntrusted;
  std::cout << "input.success=false\n";
  std::cout << "input.reason=runtime input posting is not implemented on this platform\n";
  return 2;
#endif
}
