#include <BWAPI/Runtime/RuntimeCommandSurface.h>

namespace BWAPI::Runtime
{
  int RuntimeCommandSurface::totalEntries() const
  {
    return static_cast<int>(unitCommands.size() + gameActions.size());
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

    return surface;
  }
}
