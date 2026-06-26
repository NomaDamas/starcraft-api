#pragma once

#include <string>
#include <vector>

namespace BWAPI::Runtime
{
  enum class RuntimeCommandEvidenceStatus
  {
    Unknown,
    LiveProven,
    MockTested,
    DocumentedScenario,
    FailClosed,
    AdapterLocal
  };

  struct RuntimeCommandEvidence
  {
    std::string name;
    RuntimeCommandEvidenceStatus status = RuntimeCommandEvidenceStatus::Unknown;
    std::string detail;
  };

  struct RuntimeCommandSurface
  {
    std::vector<std::string> unitCommands;
    std::vector<std::string> gameActions;
    std::vector<RuntimeCommandEvidence> unitCommandEvidence;
    std::vector<RuntimeCommandEvidence> gameActionEvidence;

    int totalEntries() const;
  };

  const char* toString(RuntimeCommandEvidenceStatus status);
  bool parseRuntimeCommandEvidenceStatus(const std::string& value, RuntimeCommandEvidenceStatus& status);
  bool isProductionCommandEvidenceStatus(RuntimeCommandEvidenceStatus status);
  RuntimeCommandSurface makeBWAPICommandSurface();
  bool containsCommandSurfaceEntry(const std::vector<std::string>& entries, const std::string& name);
  bool containsCommandEvidenceEntry(const std::vector<RuntimeCommandEvidence>& entries, const std::string& name);
  RuntimeCommandEvidenceStatus commandEvidenceStatusFor(
    const std::vector<RuntimeCommandEvidence>& entries,
    const std::string& name);
}
