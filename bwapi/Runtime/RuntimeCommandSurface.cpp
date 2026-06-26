#include <BWAPI/Runtime/RuntimeCommandSurface.h>

#include <algorithm>

namespace BWAPI::Runtime
{
  namespace
  {
    RuntimeCommandEvidence evidence(
      const std::string& name,
      RuntimeCommandEvidenceStatus status,
      const char* detail)
    {
      RuntimeCommandEvidence result;
      result.name = name;
      result.status = status;
      result.detail = detail;
      return result;
    }

    bool isAdapterLocalGameActionName(const std::string& name)
    {
      static const std::vector<std::string> adapterLocalActions = {
        "setScreenPosition",
        "enableFlag",
        "setLastError",
        "vPrintf",
        "vSendTextEx",
        "leaveGame",
        "setLocalSpeed",
        "issueCommand",
        "setTextSize",
        "vDrawText",
        "drawBox",
        "drawTriangle",
        "drawCircle",
        "drawEllipse",
        "drawDot",
        "drawLine",
        "setLatCom",
        "setGUI",
        "setMap",
        "setFrameSkip",
        "setCommandOptimizationLevel",
        "setRevealAll"
      };
      return containsCommandSurfaceEntry(adapterLocalActions, name);
    }
  }

  int RuntimeCommandSurface::totalEntries() const
  {
    return static_cast<int>(unitCommands.size() + gameActions.size());
  }

  const char* toString(RuntimeCommandEvidenceStatus status)
  {
    switch (status)
    {
    case RuntimeCommandEvidenceStatus::LiveProven:
      return "live-proven";
    case RuntimeCommandEvidenceStatus::MockTested:
      return "mock-tested";
    case RuntimeCommandEvidenceStatus::DocumentedScenario:
      return "documented-scenario";
    case RuntimeCommandEvidenceStatus::FailClosed:
      return "fail-closed";
    case RuntimeCommandEvidenceStatus::AdapterLocal:
      return "adapter-local";
    case RuntimeCommandEvidenceStatus::Unknown:
    default:
      return "unknown";
    }
  }

  bool parseRuntimeCommandEvidenceStatus(const std::string& value, RuntimeCommandEvidenceStatus& status)
  {
    if (value == "live-proven")
      status = RuntimeCommandEvidenceStatus::LiveProven;
    else if (value == "mock-tested")
      status = RuntimeCommandEvidenceStatus::MockTested;
    else if (value == "documented-scenario")
      status = RuntimeCommandEvidenceStatus::DocumentedScenario;
    else if (value == "fail-closed")
      status = RuntimeCommandEvidenceStatus::FailClosed;
    else if (value == "adapter-local")
      status = RuntimeCommandEvidenceStatus::AdapterLocal;
    else
      return false;
    return true;
  }

  bool isProductionCommandEvidenceStatus(RuntimeCommandEvidenceStatus status)
  {
    return status == RuntimeCommandEvidenceStatus::LiveProven;
  }

  RuntimeCommandSurface makeBWAPICommandSurface()
  {
    RuntimeCommandSurface surface;
    surface.unitCommands = {
      "Attack_Move",
      "Attack_Unit",
      "Build",
      "Build_Addon",
      "Train",
      "Morph",
      "Research",
      "Upgrade",
      "Set_Rally_Position",
      "Set_Rally_Unit",
      "Move",
      "Patrol",
      "Hold_Position",
      "Stop",
      "Follow",
      "Gather",
      "Return_Cargo",
      "Repair",
      "Burrow",
      "Unburrow",
      "Cloak",
      "Decloak",
      "Siege",
      "Unsiege",
      "Lift",
      "Land",
      "Load",
      "Unload",
      "Unload_All",
      "Unload_All_Position",
      "Right_Click_Position",
      "Right_Click_Unit",
      "Halt_Construction",
      "Cancel_Construction",
      "Cancel_Addon",
      "Cancel_Train",
      "Cancel_Train_Slot",
      "Cancel_Morph",
      "Cancel_Research",
      "Cancel_Upgrade",
      "Use_Tech",
      "Use_Tech_Position",
      "Use_Tech_Unit",
      "Place_COP"
    };

    surface.gameActions = {
      "setScreenPosition",
      "pingMinimap",
      "enableFlag",
      "setLastError",
      "vPrintf",
      "vSendTextEx",
      "pauseGame",
      "resumeGame",
      "leaveGame",
      "restartGame",
      "setLocalSpeed",
      "issueCommand",
      "setTextSize",
      "vDrawText",
      "drawBox",
      "drawTriangle",
      "drawCircle",
      "drawEllipse",
      "drawDot",
      "drawLine",
      "setLatCom",
      "setGUI",
      "setMap",
      "setFrameSkip",
      "setAlliance",
      "setVision",
      "setCommandOptimizationLevel",
      "setRevealAll"
    };

    for (const std::string& command : surface.unitCommands)
    {
      surface.unitCommandEvidence.push_back(
        evidence(command, RuntimeCommandEvidenceStatus::MockTested, "runtime-command-encoder-test"));
    }

    for (const std::string& action : surface.gameActions)
    {
      const RuntimeCommandEvidenceStatus status =
        isAdapterLocalGameActionName(action)
          ? RuntimeCommandEvidenceStatus::AdapterLocal
          : RuntimeCommandEvidenceStatus::MockTested;
      surface.gameActionEvidence.push_back(
        evidence(action, status, status == RuntimeCommandEvidenceStatus::AdapterLocal
          ? "adapter-local-runtime-facade"
          : "runtime-command-encoder-test"));
    }

    return surface;
  }

  bool containsCommandSurfaceEntry(const std::vector<std::string>& entries, const std::string& name)
  {
    return std::find(entries.begin(), entries.end(), name) != entries.end();
  }

  bool containsCommandEvidenceEntry(const std::vector<RuntimeCommandEvidence>& entries, const std::string& name)
  {
    return std::find_if(
      entries.begin(),
      entries.end(),
      [&](const RuntimeCommandEvidence& entry)
      {
        return entry.name == name;
      }) != entries.end();
  }

  RuntimeCommandEvidenceStatus commandEvidenceStatusFor(
    const std::vector<RuntimeCommandEvidence>& entries,
    const std::string& name)
  {
    const auto entry = std::find_if(
      entries.begin(),
      entries.end(),
      [&](const RuntimeCommandEvidence& candidate)
      {
        return candidate.name == name;
      });
    if (entry == entries.end())
      return RuntimeCommandEvidenceStatus::Unknown;
    return entry->status;
  }
}
