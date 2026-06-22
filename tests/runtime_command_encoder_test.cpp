#include <BWAPI/Runtime/RuntimeCommandEncoder.h>
#include <BWAPI/Runtime/RuntimeCommandSurface.h>

#include <BWAPI/Order.h>
#include <BWAPI/TechType.h>

#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace BWAPI::Runtime;

namespace
{
  RuntimeCommandRequest unitCommand(std::string name, std::vector<int> arguments = {})
  {
    RuntimeCommandRequest request;
    request.kind = RuntimeCommandKind::UnitCommand;
    request.name = std::move(name);
    request.targetUnitId = 0;
    request.arguments = std::move(arguments);
    return request;
  }

  RuntimeCommandRequest targetUnitCommand(
    std::string name,
    int targetUnitId,
    std::vector<int> arguments = {})
  {
    RuntimeCommandRequest request = unitCommand(std::move(name), std::move(arguments));
    request.targetUnitId = targetUnitId;
    return request;
  }

  RuntimeCommandRequest gameAction(std::string name, std::vector<int> arguments = {})
  {
    RuntimeCommandRequest request;
    request.kind = RuntimeCommandKind::GameAction;
    request.name = std::move(name);
    request.arguments = std::move(arguments);
    return request;
  }

  void expectBytes(const RuntimeEncodedCommand& encoded, const std::string& expected)
  {
    assert(encoded.encoded);
    assert(encoded.reason.empty());
    assert(formatCommandBytesHex(encoded.bytes) == expected);
  }

  RuntimeCommandRequest sampleUnitCommand(const std::string& name)
  {
    if (name == "Move" || name == "Attack_Move" || name == "Patrol" || name == "Unload_All_Position")
      return unitCommand(name, { 32, 64, 0 });
    if (name == "Set_Rally_Position")
      return unitCommand(name, { 32, 64 });
    if (name == "Attack_Unit" || name == "Follow" || name == "Gather" || name == "Repair")
      return targetUnitCommand(name, 0x0801, { 0 });
    if (name == "Set_Rally_Unit" || name == "Right_Click_Unit" || name == "Unload")
      return targetUnitCommand(name, 0x0801);
    if (name == "Right_Click_Position")
      return unitCommand(name, { 32, 64, 0 });
    if (name == "Build")
      return unitCommand(name, { 1, 2, 106, 30 });
    if (name == "Build_Addon" || name == "Land" || name == "Place_COP")
      return unitCommand(name, { 1, 2, 106 });
    if (name == "Train")
      return unitCommand(name, { 0 });
    if (name == "Morph")
      return unitCommand(name, { 37 });
    if (name == "Research")
      return unitCommand(name, { 7 });
    if (name == "Upgrade")
      return unitCommand(name, { 9 });
    if (name == "Cancel_Train_Slot")
      return unitCommand(name, { 2 });
    if (name == "Load")
      return targetUnitCommand(name, 0x0801, { BWAPI::Orders::Enum::PickupTransport, 0 });
    if (name == "Use_Tech")
      return unitCommand(name, { BWAPI::TechTypes::Enum::Stim_Packs });
    if (name == "Use_Tech_Position")
      return unitCommand(name, { 32, 64, BWAPI::Orders::Enum::CastScannerSweep });
    if (name == "Use_Tech_Unit")
      return targetUnitCommand(name, 0x0801, { BWAPI::Orders::Enum::CastInfestation });
    return unitCommand(name);
  }

  RuntimeCommandRequest sampleGameAction(const std::string& name)
  {
    if (name == "pingMinimap")
      return gameAction(name, { 12, 34 });
    if (name == "setAlliance")
      return gameAction(name, { 5 });
    if (name == "setVision")
      return gameAction(name, { 3 });
    return gameAction(name);
  }
}

int main()
{
  expectBytes(encodeRuntimeCommandRequest(unitCommand("Stop")), "1a 00");
  expectBytes(encodeRuntimeCommandRequest(unitCommand("Stop", { 1 })), "1a 01");
  expectBytes(encodeRuntimeCommandRequest(unitCommand("Hold_Position", { 1 })), "2b 01");
  expectBytes(encodeRuntimeCommandRequest(unitCommand("Return_Cargo", { 0 })), "1e 00");

  expectBytes(
    encodeRuntimeCommandRequest(unitCommand("Move", { 100, 200, 0 })),
    "15 64 00 c8 00 00 00 e4 00 06 00");
  expectBytes(
    encodeRuntimeCommandRequest(unitCommand("Attack_Move", { 10, 20, 1 })),
    "15 0a 00 14 00 00 00 e4 00 0e 01");
  expectBytes(
    encodeRuntimeCommandRequest(unitCommand("Set_Rally_Position", { 48, 64 })),
    "15 30 00 40 00 00 00 e4 00 28 00");
  expectBytes(
    encodeRuntimeCommandRequest(unitCommand("Right_Click_Position", { 320, 96, 1 })),
    "14 40 01 60 00 00 00 01");

  expectBytes(
    encodeRuntimeCommandRequest(targetUnitCommand("Attack_Unit", 0x0801, { 1 })),
    "15 00 00 00 00 01 08 e4 00 08 01");
  expectBytes(
    encodeRuntimeCommandRequest(targetUnitCommand("Set_Rally_Unit", 0x0801)),
    "15 00 00 00 00 01 08 e4 00 27 00");
  expectBytes(
    encodeRuntimeCommandRequest(targetUnitCommand("Right_Click_Unit", 0x0802)),
    "14 00 00 00 00 02 08 00");
  expectBytes(
    encodeRuntimeCommandRequest(targetUnitCommand("Unload", 0x0803)),
    "29 03 08");
  expectBytes(encodeRuntimeCommandRequest(unitCommand("Unload_All")), "28 00");

  expectBytes(encodeRuntimeCommandRequest(unitCommand("Burrow")), "2c 00");
  expectBytes(encodeRuntimeCommandRequest(unitCommand("Unburrow")), "2d 00");
  expectBytes(encodeRuntimeCommandRequest(unitCommand("Cloak")), "21 00");
  expectBytes(encodeRuntimeCommandRequest(unitCommand("Decloak")), "22 00");
  expectBytes(encodeRuntimeCommandRequest(unitCommand("Siege")), "26 00");
  expectBytes(encodeRuntimeCommandRequest(unitCommand("Unsiege")), "25 00");
  expectBytes(encodeRuntimeCommandRequest(unitCommand("Lift")), "2f 00 00 00 00");

  expectBytes(encodeRuntimeCommandRequest(unitCommand("Cancel_Construction")), "18");
  expectBytes(encodeRuntimeCommandRequest(unitCommand("Cancel_Addon")), "34");
  expectBytes(encodeRuntimeCommandRequest(unitCommand("Cancel_Research")), "31");
  expectBytes(encodeRuntimeCommandRequest(unitCommand("Cancel_Upgrade")), "33");
  expectBytes(encodeRuntimeCommandRequest(unitCommand("Cancel_Morph")), "19");
  expectBytes(encodeRuntimeCommandRequest(unitCommand("Cancel_Train")), "20 fe 00");
  expectBytes(encodeRuntimeCommandRequest(unitCommand("Cancel_Train_Slot", { 2 })), "20 02 00");
  expectBytes(encodeRuntimeCommandRequest(unitCommand("Halt_Construction")), "1a 00");

  RuntimeEncodedCommand train = encodeRuntimeCommandRequest(unitCommand("Train", { 0 }));
  expectBytes(train, "1f 00 00");
  assert(!train.warnings.empty());
  RuntimeEncodedCommand morph = encodeRuntimeCommandRequest(unitCommand("Morph", { 37 }));
  expectBytes(morph, "23 25 00");
  assert(!morph.warnings.empty());
  expectBytes(encodeRuntimeCommandRequest(unitCommand("Research", { 7 })), "30 07");
  expectBytes(encodeRuntimeCommandRequest(unitCommand("Upgrade", { 9 })), "32 09");

  RuntimeEncodedCommand build = encodeRuntimeCommandRequest(unitCommand("Build", { 1, 2, 106, 30 }));
  expectBytes(build, "0c 1e 01 00 02 00 6a 00");
  assert(!build.warnings.empty());
  RuntimeEncodedCommand load = encodeRuntimeCommandRequest(
    targetUnitCommand("Load", 0x0804, { BWAPI::Orders::Enum::PickupTransport, 1 }));
  assert(load.encoded);
  assert(load.reason.empty());
  assert(load.bytes.size() == 11);
  assert(load.bytes[0] == 0x15);
  assert(load.bytes[5] == 0x04);
  assert(load.bytes[6] == 0x08);
  assert(load.bytes[9] == BWAPI::Orders::Enum::PickupTransport);
  assert(load.bytes[10] == 1);
  assert(!load.warnings.empty());
  expectBytes(
    encodeRuntimeCommandRequest(unitCommand("Land", { 3, 4, 106 })),
    "0c 47 03 00 04 00 6a 00");
  expectBytes(
    encodeRuntimeCommandRequest(unitCommand("Use_Tech", { BWAPI::TechTypes::Enum::Stim_Packs })),
    "36");

  expectBytes(encodeRuntimeCommandRequest(gameAction("pauseGame")), "10");
  expectBytes(encodeRuntimeCommandRequest(gameAction("resumeGame")), "11");
  expectBytes(encodeRuntimeCommandRequest(gameAction("restartGame")), "08");
  expectBytes(encodeRuntimeCommandRequest(gameAction("pingMinimap", { 12, 34 })), "58 0c 00 22 00");
  expectBytes(encodeRuntimeCommandRequest(gameAction("setAlliance", { 5 })), "0e 05 00 00 00");
  expectBytes(encodeRuntimeCommandRequest(gameAction("setVision", { 3 })), "0d 03 00");

  expectBytes(encodeRuntimeSelectCommand({ 0x0801, 0x0802 }), "09 02 01 08 02 08");
  expectBytes(encodeRuntimeSelectCommand({ 0x0801 }, true), "0a 01 01 08");

  RuntimeEncodedCommand unsupported = encodeRuntimeCommandRequest(unitCommand("Load"));
  assert(!unsupported.encoded);
  assert(!unsupported.reason.empty());

  RuntimeEncodedCommand stateDependentTech = encodeRuntimeCommandRequest(
    unitCommand("Use_Tech", { BWAPI::TechTypes::Enum::Tank_Siege_Mode }));
  assert(!stateDependentTech.encoded);
  assert(!stateDependentTech.reason.empty());

  RuntimeEncodedCommand badQueued = encodeRuntimeCommandRequest(unitCommand("Stop", { 2 }));
  assert(!badQueued.encoded);
  assert(!badQueued.reason.empty());

  RuntimeEncodedCommand badPosition = encodeRuntimeCommandRequest(unitCommand("Move", { 40000, 0 }));
  assert(!badPosition.encoded);
  assert(!badPosition.reason.empty());

  RuntimeEncodedCommand badTarget = encodeRuntimeCommandRequest(targetUnitCommand("Attack_Unit", 0));
  assert(!badTarget.encoded);
  assert(!badTarget.reason.empty());

  RuntimeEncodedCommand badRally = encodeRuntimeCommandRequest(unitCommand("Set_Rally_Position", { 48, 64, 1 }));
  assert(!badRally.encoded);
  assert(!badRally.reason.empty());

  RuntimeEncodedCommand badHalt = encodeRuntimeCommandRequest(unitCommand("Halt_Construction", { 1 }));
  assert(!badHalt.encoded);
  assert(!badHalt.reason.empty());

  RuntimeEncodedCommand emptySelection = encodeRuntimeSelectCommand({});
  assert(!emptySelection.encoded);
  assert(!emptySelection.reason.empty());

  RuntimeCommandSurface surface = makeBWAPICommandSurface();
  for (const std::string& name : surface.unitCommands)
  {
    RuntimeEncodedCommand encoded = encodeRuntimeCommandRequest(sampleUnitCommand(name));
    assert(encoded.encoded);
    assert(!encoded.bytes.empty());
  }

  int encodedGameActions = 0;
  int adapterLocalGameActions = 0;
  for (const std::string& name : surface.gameActions)
  {
    RuntimeEncodedCommand encoded = encodeRuntimeCommandRequest(sampleGameAction(name));
    if (encoded.encoded)
    {
      ++encodedGameActions;
      assert(!encoded.bytes.empty());
      continue;
    }
    assert(isRuntimeAdapterLocalGameAction(name));
    ++adapterLocalGameActions;
  }
  assert(encodedGameActions == 6);
  assert(adapterLocalGameActions == 22);

  return 0;
}
