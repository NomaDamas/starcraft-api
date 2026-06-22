#include <BWAPI/Runtime/RuntimeCommandEncoder.h>

#include <BWAPI/Order.h>
#include <BWAPI/TechType.h>
#include <BWAPI/UnitType.h>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace BWAPI::Runtime
{
  namespace
  {
    constexpr std::uint8_t AttackOpcode = 0x15;
    constexpr std::uint8_t RightClickOpcode = 0x14;
    constexpr std::uint16_t UnitTypeNone = static_cast<std::uint16_t>(BWAPI::UnitTypes::Enum::None);
    constexpr int MaxSelectionCount = 12;

    RuntimeEncodedCommand reject(std::string reason)
    {
      RuntimeEncodedCommand result;
      result.encoded = false;
      result.reason = std::move(reason);
      return result;
    }

    RuntimeEncodedCommand accept(std::vector<std::uint8_t> bytes)
    {
      RuntimeEncodedCommand result;
      result.encoded = true;
      result.bytes = std::move(bytes);
      return result;
    }

    bool inRange(int value, int min, int max)
    {
      return value >= min && value <= max;
    }

    bool readArg(
      const RuntimeCommandRequest& request,
      std::size_t index,
      const char* label,
      int min,
      int max,
      int& value,
      RuntimeEncodedCommand& failure)
    {
      if (index >= request.arguments.size())
      {
        failure = reject(std::string("missing argument: ") + label);
        return false;
      }
      value = request.arguments[index];
      if (!inRange(value, min, max))
      {
        std::ostringstream reason;
        reason << label << " is out of range [" << min << ", " << max << "]: " << value;
        failure = reject(reason.str());
        return false;
      }
      return true;
    }

    bool requireArgCount(
      const RuntimeCommandRequest& request,
      std::size_t minCount,
      std::size_t maxCount,
      RuntimeEncodedCommand& failure)
    {
      if (request.arguments.size() < minCount || request.arguments.size() > maxCount)
      {
        std::ostringstream reason;
        reason << request.name << " expects ";
        if (minCount == maxCount)
          reason << minCount;
        else
          reason << minCount << ".." << maxCount;
        reason << " argument(s), got " << request.arguments.size();
        failure = reject(reason.str());
        return false;
      }
      return true;
    }

    void appendU8(std::vector<std::uint8_t>& bytes, int value)
    {
      bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    }

    void appendS8(std::vector<std::uint8_t>& bytes, int value)
    {
      bytes.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(value)));
    }

    void appendU16LE(std::vector<std::uint8_t>& bytes, int value)
    {
      const auto encoded = static_cast<std::uint16_t>(value);
      bytes.push_back(static_cast<std::uint8_t>(encoded & 0xff));
      bytes.push_back(static_cast<std::uint8_t>((encoded >> 8) & 0xff));
    }

    void appendS16LE(std::vector<std::uint8_t>& bytes, int value)
    {
      appendU16LE(bytes, static_cast<std::uint16_t>(static_cast<std::int16_t>(value)));
    }

    void appendU32LE(std::vector<std::uint8_t>& bytes, std::uint32_t value)
    {
      bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
      bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
      bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
      bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
    }

    void appendPositionUnitTarget(std::vector<std::uint8_t>& bytes, int x, int y, int unitTarget)
    {
      appendS16LE(bytes, x);
      appendS16LE(bytes, y);
      appendU16LE(bytes, unitTarget);
    }

    RuntimeEncodedCommand readQueued(
      const RuntimeCommandRequest& request,
      std::size_t index,
      int& queued)
    {
      queued = 0;
      if (request.arguments.size() <= index)
        return accept({});
      if (request.arguments.size() != index + 1)
        return reject(request.name + " has unexpected extra argument(s)");
      if (!inRange(request.arguments[index], 0, 1))
        return reject("queued flag must be 0 or 1");
      queued = request.arguments[index];
      return accept({});
    }

    RuntimeEncodedCommand encodeAttackTarget(int x, int y, int unitTarget, int order, int queued)
    {
      std::vector<std::uint8_t> bytes;
      appendU8(bytes, AttackOpcode);
      appendPositionUnitTarget(bytes, x, y, unitTarget);
      appendU16LE(bytes, UnitTypeNone);
      appendU8(bytes, order);
      appendU8(bytes, queued);
      return accept(std::move(bytes));
    }

    RuntimeEncodedCommand encodeRightClickTarget(int x, int y, int unitTarget, int queued)
    {
      std::vector<std::uint8_t> bytes;
      appendU8(bytes, RightClickOpcode);
      appendPositionUnitTarget(bytes, x, y, unitTarget);
      appendU8(bytes, queued);
      return accept(std::move(bytes));
    }

    RuntimeEncodedCommand encodePositionAttackCommand(const RuntimeCommandRequest& request, int order)
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 2, 3, failure))
        return failure;

      int x = 0;
      int y = 0;
      if (!readArg(request, 0, "x", std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max(), x, failure)
          || !readArg(request, 1, "y", std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max(), y, failure))
        return failure;

      int queued = 0;
      RuntimeEncodedCommand queuedResult = readQueued(request, 2, queued);
      if (!queuedResult.encoded)
        return queuedResult;

      return encodeAttackTarget(x, y, 0, order, queued);
    }

    RuntimeEncodedCommand encodeUnitAttackCommand(const RuntimeCommandRequest& request, int order)
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 0, 1, failure))
        return failure;
      if (!inRange(request.targetUnitId, 1, std::numeric_limits<std::uint16_t>::max()))
        return reject(request.name + " requires --unit to be a raw 16-bit BW UnitTarget id");

      int queued = 0;
      RuntimeEncodedCommand queuedResult = readQueued(request, 0, queued);
      if (!queuedResult.encoded)
        return queuedResult;

      return encodeAttackTarget(0, 0, request.targetUnitId, order, queued);
    }

    RuntimeEncodedCommand encodePositionAttackCommandUnqueued(const RuntimeCommandRequest& request, int order)
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 2, 2, failure))
        return failure;

      int x = 0;
      int y = 0;
      if (!readArg(request, 0, "x", std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max(), x, failure)
          || !readArg(request, 1, "y", std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max(), y, failure))
        return failure;

      return encodeAttackTarget(x, y, 0, order, 0);
    }

    RuntimeEncodedCommand encodeUnitAttackCommandUnqueued(const RuntimeCommandRequest& request, int order)
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 0, 0, failure))
        return failure;
      if (!inRange(request.targetUnitId, 1, std::numeric_limits<std::uint16_t>::max()))
        return reject(request.name + " requires --unit to be a raw 16-bit BW UnitTarget id");

      return encodeAttackTarget(0, 0, request.targetUnitId, order, 0);
    }

    RuntimeEncodedCommand encodeRightClickPositionCommand(const RuntimeCommandRequest& request)
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 2, 3, failure))
        return failure;

      int x = 0;
      int y = 0;
      if (!readArg(request, 0, "x", std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max(), x, failure)
          || !readArg(request, 1, "y", std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max(), y, failure))
        return failure;

      int queued = 0;
      RuntimeEncodedCommand queuedResult = readQueued(request, 2, queued);
      if (!queuedResult.encoded)
        return queuedResult;

      return encodeRightClickTarget(x, y, 0, queued);
    }

    RuntimeEncodedCommand encodeRightClickUnitCommand(const RuntimeCommandRequest& request)
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 0, 1, failure))
        return failure;
      if (!inRange(request.targetUnitId, 1, std::numeric_limits<std::uint16_t>::max()))
        return reject(request.name + " requires --unit to be a raw 16-bit BW UnitTarget id");

      int queued = 0;
      RuntimeEncodedCommand queuedResult = readQueued(request, 0, queued);
      if (!queuedResult.encoded)
        return queuedResult;

      return encodeRightClickTarget(0, 0, request.targetUnitId, queued);
    }

    RuntimeEncodedCommand encodeQueuedOpcodeCommand(
      const RuntimeCommandRequest& request,
      std::uint8_t opcode)
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 0, 1, failure))
        return failure;

      int queued = 0;
      RuntimeEncodedCommand queuedResult = readQueued(request, 0, queued);
      if (!queuedResult.encoded)
        return queuedResult;

      std::vector<std::uint8_t> bytes;
      appendU8(bytes, opcode);
      appendU8(bytes, queued);
      return accept(std::move(bytes));
    }

    RuntimeEncodedCommand encodeUnusedByteCommand(const RuntimeCommandRequest& request, std::uint8_t opcode)
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 0, 0, failure))
        return failure;

      std::vector<std::uint8_t> bytes;
      appendU8(bytes, opcode);
      appendU8(bytes, 0);
      return accept(std::move(bytes));
    }

    RuntimeEncodedCommand encodeSingleOpcodeCommand(const RuntimeCommandRequest& request, std::uint8_t opcode)
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 0, 0, failure))
        return failure;

      std::vector<std::uint8_t> bytes;
      appendU8(bytes, opcode);
      return accept(std::move(bytes));
    }

    RuntimeEncodedCommand encodeFixedQueuedOpcodeCommand(
      const RuntimeCommandRequest& request,
      std::uint8_t opcode,
      int queued)
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 0, 0, failure))
        return failure;

      std::vector<std::uint8_t> bytes;
      appendU8(bytes, opcode);
      appendU8(bytes, queued);
      return accept(std::move(bytes));
    }

    RuntimeEncodedCommand encodeTileAndUnitTypeCommand(
      const RuntimeCommandRequest& request,
      std::uint8_t order)
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 3, 3, failure))
        return failure;

      int tileX = 0;
      int tileY = 0;
      int unitType = 0;
      if (!readArg(request, 0, "tileX", std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max(), tileX, failure)
          || !readArg(request, 1, "tileY", std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max(), tileY, failure)
          || !readArg(request, 2, "unitType", 0, std::numeric_limits<std::uint16_t>::max(), unitType, failure))
        return failure;

      std::vector<std::uint8_t> bytes;
      appendU8(bytes, 0x0c);
      appendU8(bytes, order);
      appendS16LE(bytes, tileX);
      appendS16LE(bytes, tileY);
      appendU16LE(bytes, unitType);
      return accept(std::move(bytes));
    }

    RuntimeEncodedCommand encodeBuildCommand(const RuntimeCommandRequest& request)
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 4, 4, failure))
        return failure;

      int order = 0;
      if (!readArg(request, 3, "buildOrder", 0, std::numeric_limits<std::uint8_t>::max(), order, failure))
        return failure;

      RuntimeCommandRequest normalized = request;
      normalized.arguments.pop_back();
      RuntimeEncodedCommand encoded = encodeTileAndUnitTypeCommand(normalized, static_cast<std::uint8_t>(order));
      if (encoded.encoded)
      {
        encoded.warnings.push_back(
          "Build is state-dependent in BWAPI; buildOrder must already be DroneStartBuild, PlaceBuilding, or PlaceProtossBuilding.");
      }
      return encoded;
    }

    RuntimeEncodedCommand encodeType16Command(
      const RuntimeCommandRequest& request,
      std::uint8_t opcode,
      const char* label)
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 1, 1, failure))
        return failure;

      int value = 0;
      if (!readArg(request, 0, label, 0, std::numeric_limits<std::uint16_t>::max(), value, failure))
        return failure;

      std::vector<std::uint8_t> bytes;
      appendU8(bytes, opcode);
      appendU16LE(bytes, value);
      return accept(std::move(bytes));
    }

    RuntimeEncodedCommand encodeType8Command(
      const RuntimeCommandRequest& request,
      std::uint8_t opcode,
      const char* label)
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 1, 1, failure))
        return failure;

      int value = 0;
      if (!readArg(request, 0, label, 0, std::numeric_limits<std::uint8_t>::max(), value, failure))
        return failure;

      std::vector<std::uint8_t> bytes;
      appendU8(bytes, opcode);
      appendU8(bytes, value);
      return accept(std::move(bytes));
    }

    RuntimeEncodedCommand encodeCancelTrain(const RuntimeCommandRequest& request)
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 0, 1, failure))
        return failure;

      int slot = -2;
      if (!request.arguments.empty()
          && !readArg(request, 0, "slot", std::numeric_limits<std::int8_t>::min(), std::numeric_limits<std::int8_t>::max(), slot, failure))
        return failure;

      std::vector<std::uint8_t> bytes;
      appendU8(bytes, 0x20);
      appendS8(bytes, slot);
      appendU8(bytes, 0);
      return accept(std::move(bytes));
    }

    RuntimeEncodedCommand encodeUnitMorph(const RuntimeCommandRequest& request)
    {
      RuntimeEncodedCommand result = encodeType16Command(request, 0x23, "unitType");
      if (result.encoded)
      {
        result.warnings.push_back(
          "Morph can be a unit morph or building morph in BWAPI; this encodes the unit-morph command form.");
      }
      return result;
    }

    RuntimeEncodedCommand encodeTrain(const RuntimeCommandRequest& request)
    {
      RuntimeEncodedCommand result = encodeType16Command(request, 0x1f, "unitType");
      if (result.encoded)
      {
        result.warnings.push_back(
          "Train is state-dependent for larvae, morph-capable zerg units, carriers, and reavers; this encodes the TrainUnit command form.");
      }
      return result;
    }

    RuntimeEncodedCommand encodeUseTechPosition(const RuntimeCommandRequest& request)
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 3, 3, failure))
        return failure;

      int x = 0;
      int y = 0;
      int order = 0;
      if (!readArg(request, 0, "x", std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max(), x, failure)
          || !readArg(request, 1, "y", std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max(), y, failure)
          || !readArg(request, 2, "order", 0, std::numeric_limits<std::uint8_t>::max(), order, failure))
        return failure;

      return encodeAttackTarget(x, y, 0, order, 0);
    }

    RuntimeEncodedCommand encodeUseTechUnit(const RuntimeCommandRequest& request)
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 1, 1, failure))
        return failure;
      if (!inRange(request.targetUnitId, 1, std::numeric_limits<std::uint16_t>::max()))
        return reject(request.name + " requires --unit to be a raw 16-bit BW UnitTarget id");

      int order = 0;
      if (!readArg(request, 0, "order", 0, std::numeric_limits<std::uint8_t>::max(), order, failure))
        return failure;

      return encodeAttackTarget(0, 0, request.targetUnitId, order, 0);
    }

    RuntimeEncodedCommand encodeLoadCommand(const RuntimeCommandRequest& request)
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 1, 2, failure))
        return failure;
      if (!inRange(request.targetUnitId, 1, std::numeric_limits<std::uint16_t>::max()))
        return reject(request.name + " requires --unit to be a raw 16-bit BW UnitTarget id");

      int order = 0;
      if (!readArg(request, 0, "loadOrder", 0, std::numeric_limits<std::uint8_t>::max(), order, failure))
        return failure;

      int queued = 0;
      RuntimeEncodedCommand queuedResult = readQueued(request, 1, queued);
      if (!queuedResult.encoded)
        return queuedResult;

      RuntimeEncodedCommand encoded = encodeAttackTarget(0, 0, request.targetUnitId, order, queued);
      if (encoded.encoded)
      {
        encoded.warnings.push_back(
          "Load is state-dependent in BWAPI; loadOrder must already be PickupBunker, PickupTransport, or RightClick.");
      }
      return encoded;
    }

    RuntimeEncodedCommand encodeUnloadAllCommand(const RuntimeCommandRequest& request)
    {
      RuntimeEncodedCommand encoded = encodeUnusedByteCommand(request, 0x28);
      if (encoded.encoded)
      {
        encoded.warnings.push_back(
          "Unload_All is state-dependent in BWAPI; this encodes the bunker UnloadAll command form.");
      }
      return encoded;
    }

    RuntimeEncodedCommand encodeCancelMorphCommand(const RuntimeCommandRequest& request)
    {
      RuntimeEncodedCommand encoded = encodeSingleOpcodeCommand(request, 0x19);
      if (encoded.encoded)
      {
        encoded.warnings.push_back(
          "Cancel_Morph is state-dependent in BWAPI; this encodes the non-building CancelUnitMorph command form.");
      }
      return encoded;
    }

    RuntimeEncodedCommand encodeUseTechCommand(const RuntimeCommandRequest& request)
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 1, 1, failure))
        return failure;

      int tech = 0;
      if (!readArg(request, 0, "tech", 0, std::numeric_limits<std::uint8_t>::max(), tech, failure))
        return failure;

      if (tech == BWAPI::TechTypes::Enum::Stim_Packs)
      {
        std::vector<std::uint8_t> bytes;
        appendU8(bytes, 0x36);
        return accept(std::move(bytes));
      }

      return reject(
        "Use_Tech requires live unit state for this tech; only Stim_Packs has a state-independent turn-buffer form");
    }

    RuntimeEncodedCommand encodeGameAction(const RuntimeCommandRequest& request)
    {
      if (request.name == "pauseGame")
        return encodeSingleOpcodeCommand(request, 0x10);
      if (request.name == "resumeGame")
        return encodeSingleOpcodeCommand(request, 0x11);
      if (request.name == "restartGame")
        return encodeSingleOpcodeCommand(request, 0x08);
      if (request.name == "pingMinimap")
      {
        RuntimeEncodedCommand failure;
        if (!requireArgCount(request, 2, 2, failure))
          return failure;

        int x = 0;
        int y = 0;
        if (!readArg(request, 0, "x", std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max(), x, failure)
            || !readArg(request, 1, "y", std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max(), y, failure))
          return failure;

        std::vector<std::uint8_t> bytes;
        appendU8(bytes, 0x58);
        appendS16LE(bytes, x);
        appendS16LE(bytes, y);
        return accept(std::move(bytes));
      }
      if (request.name == "setAlliance")
      {
        RuntimeEncodedCommand failure;
        if (!requireArgCount(request, 1, 1, failure))
          return failure;

        int alliance = 0;
        if (!readArg(request, 0, "alliance", 0, std::numeric_limits<std::int32_t>::max(), alliance, failure))
          return failure;

        std::vector<std::uint8_t> bytes;
        appendU8(bytes, 0x0e);
        appendU32LE(bytes, static_cast<std::uint32_t>(alliance));
        return accept(std::move(bytes));
      }
      if (request.name == "setVision")
      {
        RuntimeEncodedCommand failure;
        if (!requireArgCount(request, 1, 1, failure))
          return failure;

        int vision = 0;
        if (!readArg(request, 0, "vision", 0, std::numeric_limits<std::uint16_t>::max(), vision, failure))
          return failure;

        std::vector<std::uint8_t> bytes;
        appendU8(bytes, 0x0d);
        appendU16LE(bytes, vision);
        return accept(std::move(bytes));
      }

      return reject("game action is not a BW turn-buffer command: " + request.name);
    }
  }

  RuntimeEncodedCommand encodeRuntimeCommandRequest(const RuntimeCommandRequest& request)
  {
    if (request.name.empty())
      return reject("runtime command name is empty");

    if (request.kind == RuntimeCommandKind::GameAction)
      return encodeGameAction(request);

    if (request.kind != RuntimeCommandKind::UnitCommand)
      return reject("unknown runtime command kind");

    if (request.name == "Move")
      return encodePositionAttackCommand(request, BWAPI::Orders::Enum::Move);
    if (request.name == "Attack_Move")
      return encodePositionAttackCommand(request, BWAPI::Orders::Enum::AttackMove);
    if (request.name == "Patrol")
      return encodePositionAttackCommand(request, BWAPI::Orders::Enum::Patrol);
    if (request.name == "Set_Rally_Position")
      return encodePositionAttackCommandUnqueued(request, BWAPI::Orders::Enum::RallyPointTile);
    if (request.name == "Unload_All_Position")
      return encodePositionAttackCommand(request, BWAPI::Orders::Enum::MoveUnload);
    if (request.name == "Attack_Unit")
      return encodeUnitAttackCommand(request, BWAPI::Orders::Enum::Attack1);
    if (request.name == "Follow")
      return encodeUnitAttackCommand(request, BWAPI::Orders::Enum::Follow);
    if (request.name == "Gather")
      return encodeUnitAttackCommand(request, BWAPI::Orders::Enum::Harvest1);
    if (request.name == "Repair")
      return encodeUnitAttackCommand(request, BWAPI::Orders::Enum::Repair);
    if (request.name == "Set_Rally_Unit")
      return encodeUnitAttackCommandUnqueued(request, BWAPI::Orders::Enum::RallyPointUnit);
    if (request.name == "Right_Click_Position")
      return encodeRightClickPositionCommand(request);
    if (request.name == "Right_Click_Unit")
      return encodeRightClickUnitCommand(request);
    if (request.name == "Stop")
      return encodeQueuedOpcodeCommand(request, 0x1a);
    if (request.name == "Halt_Construction")
      return encodeFixedQueuedOpcodeCommand(request, 0x1a, 0);
    if (request.name == "Hold_Position")
      return encodeQueuedOpcodeCommand(request, 0x2b);
    if (request.name == "Return_Cargo")
      return encodeQueuedOpcodeCommand(request, 0x1e);
    if (request.name == "Burrow")
      return encodeUnusedByteCommand(request, 0x2c);
    if (request.name == "Unburrow")
      return encodeUnusedByteCommand(request, 0x2d);
    if (request.name == "Cloak")
      return encodeUnusedByteCommand(request, 0x21);
    if (request.name == "Decloak")
      return encodeUnusedByteCommand(request, 0x22);
    if (request.name == "Siege")
      return encodeUnusedByteCommand(request, 0x26);
    if (request.name == "Unsiege")
      return encodeUnusedByteCommand(request, 0x25);
    if (request.name == "Lift")
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 0, 0, failure))
        return failure;
      std::vector<std::uint8_t> bytes;
      appendU8(bytes, 0x2f);
      appendU32LE(bytes, 0);
      return accept(std::move(bytes));
    }
    if (request.name == "Cancel_Construction")
      return encodeSingleOpcodeCommand(request, 0x18);
    if (request.name == "Cancel_Addon")
      return encodeSingleOpcodeCommand(request, 0x34);
    if (request.name == "Cancel_Research")
      return encodeSingleOpcodeCommand(request, 0x31);
    if (request.name == "Cancel_Upgrade")
      return encodeSingleOpcodeCommand(request, 0x33);
    if (request.name == "Cancel_Train" || request.name == "Cancel_Train_Slot")
      return encodeCancelTrain(request);
    if (request.name == "Train")
      return encodeTrain(request);
    if (request.name == "Morph")
      return encodeUnitMorph(request);
    if (request.name == "Research")
      return encodeType8Command(request, 0x30, "tech");
    if (request.name == "Upgrade")
      return encodeType8Command(request, 0x32, "upgrade");
    if (request.name == "Build")
      return encodeBuildCommand(request);
    if (request.name == "Build_Addon")
      return encodeTileAndUnitTypeCommand(request, BWAPI::Orders::Enum::PlaceAddon);
    if (request.name == "Land")
      return encodeTileAndUnitTypeCommand(request, BWAPI::Orders::Enum::BuildingLand);
    if (request.name == "Place_COP")
      return encodeTileAndUnitTypeCommand(request, BWAPI::Orders::Enum::CTFCOP2);
    if (request.name == "Load")
      return encodeLoadCommand(request);
    if (request.name == "Unload_All")
      return encodeUnloadAllCommand(request);
    if (request.name == "Cancel_Morph")
      return encodeCancelMorphCommand(request);
    if (request.name == "Use_Tech")
      return encodeUseTechCommand(request);
    if (request.name == "Use_Tech_Position")
      return encodeUseTechPosition(request);
    if (request.name == "Use_Tech_Unit")
      return encodeUseTechUnit(request);
    if (request.name == "Unload")
    {
      RuntimeEncodedCommand failure;
      if (!requireArgCount(request, 0, 0, failure))
        return failure;
      if (!inRange(request.targetUnitId, 1, std::numeric_limits<std::uint16_t>::max()))
        return reject(request.name + " requires --unit to be a raw 16-bit BW UnitTarget id");
      std::vector<std::uint8_t> bytes;
      appendU8(bytes, 0x29);
      appendU16LE(bytes, request.targetUnitId);
      return accept(std::move(bytes));
    }

    return reject("unit command requires live unit state or a dedicated adapter implementation: " + request.name);
  }

  RuntimeEncodedCommand encodeRuntimeSelectCommand(
    const std::vector<int>& unitTargetIds,
    bool addToSelection)
  {
    if (unitTargetIds.empty())
      return reject("selection command requires at least one unit target id");
    if (unitTargetIds.size() > MaxSelectionCount)
      return reject("selection command supports at most 12 unit target ids");

    std::vector<std::uint8_t> bytes;
    appendU8(bytes, addToSelection ? 0x0a : 0x09);
    appendU8(bytes, static_cast<int>(unitTargetIds.size()));
    for (int unitTargetId : unitTargetIds)
    {
      if (!inRange(unitTargetId, 1, std::numeric_limits<std::uint16_t>::max()))
        return reject("selection unit target id must be in [1, 65535]");
      appendU16LE(bytes, unitTargetId);
    }

    return accept(std::move(bytes));
  }

  std::string formatCommandBytesHex(const std::vector<std::uint8_t>& bytes)
  {
    std::ostringstream output;
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
      if (i > 0)
        output << ' ';
      output << std::hex << std::setfill('0') << std::setw(2)
             << static_cast<int>(bytes[i]);
    }
    return output.str();
  }
}
