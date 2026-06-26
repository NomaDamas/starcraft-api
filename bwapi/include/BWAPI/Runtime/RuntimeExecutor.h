#pragma once

#include <BWAPI/Runtime/RuntimeContract.h>
#include <BWAPI/Runtime/RuntimeCommandQueue.h>

#include <cstddef>
#include <string>
#include <vector>

namespace BWAPI::Runtime
{
  inline constexpr const char* RuntimeExecutorBridgeProtocol = "starcraft-api-file-bridge-v1";
  inline constexpr const char* RuntimeExecutorBridgeBootstrapMode = "launch-attach-bootstrap";
  inline constexpr const char* RuntimeExecutorBridgeValidatedAdapterMode = "validated-runtime-adapter";
  inline constexpr const char* RuntimeExecutorBridgeCommandSurfaceLine =
    "proof.command_surface=runtime-command-surface-v1";
  inline constexpr const char* RuntimeExecutorBridgeActiveCommandReceiverLine = "command.receiver=active";
  inline constexpr const char* RuntimeExecutorBridgeRuntimeCommandQueueSinkLine = "command.sink=runtime-command-queue-v1";
  inline constexpr const char* RuntimeExecutorBridgeReadyFile = "ready";
  inline constexpr const char* RuntimeExecutorBridgeCommandFile = "commands.log";
  inline constexpr const char* RuntimeExecutorBridgeIssueCommandProofPendingFile =
    "issue_commands.command.pending.tsv";

  struct RuntimeExecutorBehaviorProof
  {
    const char* id = "";
    Capability capability = Capability::ReadGameState;
    const char* readyFileLine = "";
    const char* description = "";
  };

  struct RuntimeExecutorPreflightResult
  {
    bool contractValid = false;
    bool processIdentified = false;
    bool memoryAccessible = false;
    bool targetLocated = false;
    bool executorAvailable = false;
    std::string executorName;
    std::string executorBridgeMode;
    std::string memoryAccessReason;
    std::vector<Capability> provenCapabilities;
    std::vector<std::string> missingBehaviorProofs;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
  };

  struct RuntimeExecutorSubmitResult
  {
    bool submitted = false;
    std::size_t submittedCommands = 0;
    std::string reason;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
  };

  const std::vector<RuntimeExecutorBehaviorProof>& requiredRuntimeExecutorBehaviorProofs();
  RuntimeContract applyRuntimeExecutorBridgeContractProofs(
    const RuntimeEnvironment& environment,
    RuntimeContract contract);
  RuntimeExecutorPreflightResult preflightRuntimeExecutor(
    const RuntimeEnvironment& environment,
    const RuntimeContract& contract);
  RuntimeExecutorSubmitResult submitRuntimeCommands(
    const RuntimeEnvironment& environment,
    const std::vector<RuntimeCommandRequest>& commands);
}
