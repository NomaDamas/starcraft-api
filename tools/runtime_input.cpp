#include <BWAPI/Runtime/RuntimeProcess.h>

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <variant>
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
      << "  --clicks <sequence>     macOS mouse clicks as button:x:y tokens; x/y are 0..1 window-relative\n"
      << "                           e.g. \"left:0.35:0.45 right:0.70:0.55\"\n"
      << "  --hid-events            also post events through the foreground HID event tap\n"
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

  struct ClickToken
  {
    std::string button;
    double x = 0.0;
    double y = 0.0;
  };

  bool parseNormalizedDouble(const std::string& value, double& output)
  {
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0' || parsed < 0.0 || parsed > 1.0)
      return false;
    output = parsed;
    return true;
  }

  bool parseClickToken(const std::string& token, ClickToken& output)
  {
    const std::size_t firstColon = token.find(':');
    if (firstColon == std::string::npos)
      return false;
    const std::size_t secondColon = token.find(':', firstColon + 1);
    if (secondColon == std::string::npos || token.find(':', secondColon + 1) != std::string::npos)
      return false;

    output.button = lower(token.substr(0, firstColon));
    if (output.button != "left" && output.button != "right")
      return false;
    return parseNormalizedDouble(token.substr(firstColon + 1, secondColon - firstColon - 1), output.x)
      && parseNormalizedDouble(token.substr(secondColon + 1), output.y);
  }

  std::vector<ClickToken> splitClickSequence(const std::string& sequence, std::string& reason)
  {
    std::vector<ClickToken> clicks;
    for (const std::string& token : splitKeySequence(sequence))
    {
      ClickToken click;
      if (!parseClickToken(token, click))
      {
        reason = "unsupported click token: " + token;
        return {};
      }
      clicks.push_back(click);
    }
    return clicks;
  }

#if defined(__APPLE__)
  struct TargetWindow
  {
    CGRect bounds = CGRectZero;
  };

  std::optional<TargetWindow> findTargetWindowWithOptions(
    int processId,
    CGWindowListOption options)
  {
    CFArrayRef windows = CGWindowListCopyWindowInfo(options, kCGNullWindowID);
    if (windows == nullptr)
      return std::nullopt;

    std::optional<TargetWindow> result;
    double bestArea = 0.0;
    double bestLayerPenalty = std::numeric_limits<double>::max();
    const CFIndex count = CFArrayGetCount(windows);
    for (CFIndex i = 0; i < count; ++i)
    {
      CFDictionaryRef window =
        static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, i));
      if (window == nullptr)
        continue;

      int64_t ownerPid = 0;
      CFNumberRef ownerPidRef =
        static_cast<CFNumberRef>(CFDictionaryGetValue(window, kCGWindowOwnerPID));
      if (ownerPidRef == nullptr
          || !CFNumberGetValue(ownerPidRef, kCFNumberSInt64Type, &ownerPid)
          || ownerPid != processId)
        continue;

      int64_t layer = 0;
      CFNumberRef layerRef =
        static_cast<CFNumberRef>(CFDictionaryGetValue(window, kCGWindowLayer));
      if (layerRef != nullptr)
        CFNumberGetValue(layerRef, kCFNumberSInt64Type, &layer);

      CFDictionaryRef boundsRef =
        static_cast<CFDictionaryRef>(CFDictionaryGetValue(window, kCGWindowBounds));
      CGRect bounds = CGRectZero;
      if (boundsRef == nullptr || !CGRectMakeWithDictionaryRepresentation(boundsRef, &bounds))
        continue;
      if (bounds.size.width <= 0 || bounds.size.height <= 0)
        continue;

      const double area = bounds.size.width * bounds.size.height;
      const double layerPenalty = layer == 0 ? 0.0 : 1.0;
      if (!result.has_value()
          || layerPenalty < bestLayerPenalty
          || (layerPenalty == bestLayerPenalty && area > bestArea))
      {
        result = TargetWindow { bounds };
        bestArea = area;
        bestLayerPenalty = layerPenalty;
      }
    }

    CFRelease(windows);
    return result;
  }

  std::optional<TargetWindow> findTargetWindow(int processId)
  {
    std::optional<TargetWindow> result =
      findTargetWindowWithOptions(processId, kCGWindowListOptionOnScreenOnly);
    if (result.has_value())
      return result;
    return findTargetWindowWithOptions(processId, kCGWindowListOptionAll);
  }

  bool macFunctionKeyCode(const std::string& token, CGKeyCode& code)
  {
    if (token.size() < 2 || token[0] != 'f')
      return false;

    int number = 0;
    for (std::size_t i = 1; i < token.size(); ++i)
    {
      if (std::isdigit(static_cast<unsigned char>(token[i])) == 0)
        return false;
      number = (number * 10) + (token[i] - '0');
    }

    switch (number)
    {
    case 1: code = 122; return true;
    case 2: code = 120; return true;
    case 3: code = 99; return true;
    case 4: code = 118; return true;
    case 5: code = 96; return true;
    case 6: code = 97; return true;
    case 7: code = 98; return true;
    case 8: code = 100; return true;
    case 9: code = 101; return true;
    case 10: code = 109; return true;
    case 11: code = 103; return true;
    case 12: code = 111; return true;
    default: return false;
    }
  }

  bool macVirtualKeyCode(const std::string& token, CGKeyCode& code)
  {
    if (macFunctionKeyCode(token, code))
      return true;

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
    if (token == "pause" || token == "break")
    {
      code = 113;
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

  bool postKeyToPid(int processId, CGKeyCode code, bool hidEvents, std::string& reason)
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

    if (hidEvents)
    {
      CGEventPost(kCGHIDEventTap, keyDown);
      CGEventPost(kCGHIDEventTap, keyUp);
    }
    else
    {
      CGEventPostToPid(static_cast<pid_t>(processId), keyDown);
      CGEventPostToPid(static_cast<pid_t>(processId), keyUp);
    }
    CFRelease(keyDown);
    CFRelease(keyUp);
    CFRelease(source);
    return true;
  }

  bool postKeyToPidWithTimeout(
    int processId,
    CGKeyCode code,
    bool hidEvents,
    int timeoutMs,
    std::string& reason)
  {
    auto completion = std::make_shared<std::promise<std::pair<bool, std::string>>>();
    std::future<std::pair<bool, std::string>> future = completion->get_future();
    std::thread(
      [completion, processId, code, hidEvents]()
      {
        std::string postReason;
        const bool posted = postKeyToPid(processId, code, hidEvents, postReason);
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

  bool postClickToPid(
    int processId,
    const TargetWindow& window,
    const ClickToken& click,
    bool hidEvents,
    std::string& reason)
  {
    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    if (source == nullptr)
    {
      reason = "CGEventSourceCreate failed";
      return false;
    }

    const CGPoint point = CGPointMake(
      window.bounds.origin.x + window.bounds.size.width * click.x,
      window.bounds.origin.y + window.bounds.size.height * click.y);
    const bool right = click.button == "right";
    const CGMouseButton button = right ? kCGMouseButtonRight : kCGMouseButtonLeft;
    const CGEventType downType = right ? kCGEventRightMouseDown : kCGEventLeftMouseDown;
    const CGEventType upType = right ? kCGEventRightMouseUp : kCGEventLeftMouseUp;

    CGEventRef mouseDown = CGEventCreateMouseEvent(source, downType, point, button);
    CGEventRef mouseUp = CGEventCreateMouseEvent(source, upType, point, button);
    if (mouseDown == nullptr || mouseUp == nullptr)
    {
      if (mouseDown != nullptr)
        CFRelease(mouseDown);
      if (mouseUp != nullptr)
        CFRelease(mouseUp);
      CFRelease(source);
      reason = "CGEventCreateMouseEvent failed";
      return false;
    }

    CGEventSetIntegerValueField(mouseDown, kCGMouseEventClickState, 1);
    CGEventSetIntegerValueField(mouseUp, kCGMouseEventClickState, 1);
    if (hidEvents)
    {
      CGWarpMouseCursorPosition(point);
      CGEventPost(kCGHIDEventTap, mouseDown);
      CGEventPost(kCGHIDEventTap, mouseUp);
    }
    else
    {
      CGEventPostToPid(static_cast<pid_t>(processId), mouseDown);
      CGEventPostToPid(static_cast<pid_t>(processId), mouseUp);
    }
    CFRelease(mouseDown);
    CFRelease(mouseUp);
    CFRelease(source);
    return true;
  }

  bool postClickToPidWithTimeout(
    int processId,
    const TargetWindow& window,
    const ClickToken& click,
    bool hidEvents,
    int timeoutMs,
    std::string& reason)
  {
    auto completion = std::make_shared<std::promise<std::pair<bool, std::string>>>();
    std::future<std::pair<bool, std::string>> future = completion->get_future();
    std::thread(
      [completion, processId, window, click, hidEvents]()
      {
        std::string postReason;
        const bool posted = postClickToPid(processId, window, click, hidEvents, postReason);
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
      reason = "timed out while posting mouse input to target process";
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
  bool hidEvents = false;
  std::string sequence;
  std::string clickSequence;

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
    if (arg == "--hid-events")
    {
      hidEvents = true;
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
    if (arg == "--clicks")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "--clicks requires a sequence\n";
        return 64;
      }
      clickSequence = argv[++i];
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
  if (sequence.empty() && clickSequence.empty())
  {
    std::cerr << "key or click sequence is required\n";
    return 64;
  }

  const std::vector<std::string> tokens = splitKeySequence(sequence);
  if (!sequence.empty() && tokens.empty())
  {
    std::cerr << "key sequence did not contain any tokens\n";
    return 64;
  }
  std::string clickParseReason;
  const std::vector<ClickToken> clicks = splitClickSequence(clickSequence, clickParseReason);
  if (!clickSequence.empty() && clicks.empty())
  {
    std::cerr << clickParseReason << '\n';
    return 65;
  }

  std::cout << "input.process_id=" << processId << '\n';
  std::cout << "input.token_count=" << (tokens.size() + clicks.size()) << '\n';
  std::cout << "input.post_timeout_ms=" << postTimeoutMs << '\n';
  std::cout << "input.hid_events=" << (hidEvents ? "true" : "false") << '\n';
  for (std::size_t i = 0; i < tokens.size(); ++i)
    std::cout << "input.token." << i << '=' << tokens[i] << '\n';
  for (std::size_t i = 0; i < clicks.size(); ++i)
    std::cout << "input.click." << i << '='
              << clicks[i].button << ':' << clicks[i].x << ':' << clicks[i].y << '\n';
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
  std::optional<TargetWindow> targetWindow;
  if (!clicks.empty())
  {
    targetWindow = findTargetWindow(processId);
    std::cout << "input.window_found=" << (targetWindow.has_value() ? "true" : "false") << '\n';
    if (targetWindow.has_value())
    {
      std::cout << "input.window.x=" << targetWindow->bounds.origin.x << '\n';
      std::cout << "input.window.y=" << targetWindow->bounds.origin.y << '\n';
      std::cout << "input.window.width=" << targetWindow->bounds.size.width << '\n';
      std::cout << "input.window.height=" << targetWindow->bounds.size.height << '\n';
    }
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
    if (!postKeyToPidWithTimeout(processId, code, hidEvents, postTimeoutMs, reason))
    {
      std::cout << "input.sent=" << sent << '\n';
      std::cout << "input.success=false\n";
      std::cout << "input.reason=" << reason << '\n';
      return 2;
    }
    ++sent;
    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
  }
  if (!clicks.empty() && !targetWindow.has_value())
  {
    std::cout << "input.sent=" << sent << '\n';
    std::cout << "input.success=false\n";
    std::cout << "input.reason=target process window was not found\n";
    return 2;
  }
  for (const ClickToken& click : clicks)
  {
    std::string reason;
    if (!postClickToPidWithTimeout(processId, *targetWindow, click, hidEvents, postTimeoutMs, reason))
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
  (void)delayMs;
  (void)allowUntrusted;
  if (dryRun)
  {
    std::cout << "input.dry_run=true\n";
    return 0;
  }
  std::cout << "input.success=false\n";
  std::cout << "input.reason=runtime input posting is not implemented on this platform\n";
  return 2;
#endif
}
