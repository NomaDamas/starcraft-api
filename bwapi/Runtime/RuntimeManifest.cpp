#include <BWAPI/Runtime/RuntimeManifest.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace BWAPI::Runtime
{
  namespace
  {
    struct BindingDirective
    {
      std::string name;
      BindingKind kind = BindingKind::DataAddress;
      BindingRequirement requirement = BindingRequirement::Required;
      std::string evidence;
      int line = 0;
    };

    struct StructureDirective
    {
      std::string name;
      std::size_t size = 0;
      BindingRequirement requirement = BindingRequirement::Required;
      int line = 0;
    };

    struct FieldDirective
    {
      std::string structureName;
      std::string fieldName;
      std::size_t offset = 0;
      std::size_t size = 0;
      int line = 0;
    };

    struct ManifestAccumulator
    {
      Product product = Product::Unknown;
      std::string version;
      int implementedApiSurfaceMethods = 0;
      int implementedCommandSurfaceEntries = 0;
      std::vector<Capability> capabilities;
      std::vector<BindingDirective> bindings;
      std::vector<StructureDirective> structures;
      std::vector<FieldDirective> fields;
    };

    std::string trim(std::string value)
    {
      const auto first = std::find_if_not(
        value.begin(),
        value.end(),
        [](unsigned char ch)
        {
          return std::isspace(ch) != 0;
        });
      const auto last = std::find_if_not(
        value.rbegin(),
        value.rend(),
        [](unsigned char ch)
        {
          return std::isspace(ch) != 0;
        }).base();

      if (first >= last)
        return {};
      return std::string(first, last);
    }

    std::vector<std::string> splitWhitespace(const std::string& line)
    {
      std::istringstream stream(line);
      std::vector<std::string> tokens;
      std::string token;
      while (stream >> token)
        tokens.push_back(token);
      return tokens;
    }

    void addError(RuntimeManifestLoadResult& result, const std::string& sourceName, int line, const std::string& message)
    {
      std::ostringstream output;
      output << sourceName;
      if (line > 0)
        output << ':' << line;
      output << ": " << message;
      result.errors.push_back(output.str());
    }

    void addWarning(RuntimeManifestLoadResult& result, const std::string& sourceName, int line, const std::string& message)
    {
      std::ostringstream output;
      output << sourceName;
      if (line > 0)
        output << ':' << line;
      output << ": " << message;
      result.warnings.push_back(output.str());
    }

    bool parseSize(const std::string& value, std::size_t& parsed)
    {
      try
      {
        std::size_t consumed = 0;
        const unsigned long long number = std::stoull(value, &consumed, 0);
        if (consumed != value.size())
          return false;
        if (number > std::numeric_limits<std::size_t>::max())
          return false;
        parsed = static_cast<std::size_t>(number);
        return true;
      }
      catch (const std::exception&)
      {
        return false;
      }
    }

    bool parseInt(const std::string& value, int& parsed)
    {
      std::size_t sizeValue = 0;
      if (!parseSize(value, sizeValue) || sizeValue > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return false;
      parsed = static_cast<int>(sizeValue);
      return true;
    }

    void applyBindings(
      RuntimeManifestLoadResult& result,
      const std::string& sourceName,
      RuntimeContract& contract,
      const std::vector<BindingDirective>& directives)
    {
      for (const BindingDirective& directive : directives)
      {
        auto it = std::find_if(
          contract.bindings.begin(),
          contract.bindings.end(),
          [&](const RuntimeBinding& binding)
          {
            return binding.name == directive.name && binding.kind == directive.kind;
          });

        if (it == contract.bindings.end())
        {
          addWarning(result, sourceName, directive.line, "manifest binding is not part of the BWAPI parity contract: " + directive.name);
          continue;
        }

        it->requirement = directive.requirement;
        it->resolved = true;
        it->evidence = directive.evidence;
      }
    }

    void applyStructures(
      RuntimeManifestLoadResult& result,
      const std::string& sourceName,
      RuntimeContract& contract,
      const std::vector<StructureDirective>& directives)
    {
      for (const StructureDirective& directive : directives)
      {
        auto it = std::find_if(
          contract.structures.begin(),
          contract.structures.end(),
          [&](const StructureLayout& structure)
          {
            return structure.name == directive.name;
          });

        if (it == contract.structures.end())
        {
          addWarning(result, sourceName, directive.line, "manifest structure is not part of the BWAPI parity contract: " + directive.name);
          continue;
        }

        it->size = directive.size;
        it->requirement = directive.requirement;
      }
    }

    void applyFields(
      RuntimeManifestLoadResult& result,
      const std::string& sourceName,
      RuntimeContract& contract,
      const std::vector<FieldDirective>& directives)
    {
      for (const FieldDirective& directive : directives)
      {
        auto structureIt = std::find_if(
          contract.structures.begin(),
          contract.structures.end(),
          [&](const StructureLayout& structure)
          {
            return structure.name == directive.structureName;
          });

        if (structureIt == contract.structures.end())
        {
          addWarning(result, sourceName, directive.line, "manifest field references an unknown structure: " + directive.structureName);
          continue;
        }

        auto fieldIt = std::find_if(
          structureIt->fields.begin(),
          structureIt->fields.end(),
          [&](const StructureField& field)
          {
            return field.name == directive.fieldName;
          });

        if (fieldIt == structureIt->fields.end())
        {
          addWarning(
            result,
            sourceName,
            directive.line,
            "manifest field is not part of the BWAPI parity contract: " + directive.structureName + "." + directive.fieldName);
          continue;
        }

        fieldIt->resolved = true;
        fieldIt->offset = directive.offset;
        fieldIt->size = directive.size;
      }
    }
  }

  RuntimeManifestLoadResult loadRuntimeManifest(std::istream& input, const std::string& sourceName)
  {
    RuntimeManifestLoadResult result;
    ManifestAccumulator accumulator;

    std::string rawLine;
    int lineNumber = 0;
    while (std::getline(input, rawLine))
    {
      ++lineNumber;

      const std::size_t commentOffset = rawLine.find('#');
      const std::string line = trim(rawLine.substr(0, commentOffset));
      if (line.empty())
        continue;

      const std::vector<std::string> tokens = splitWhitespace(line);
      if (tokens.empty())
        continue;

      const std::string& directive = tokens[0];
      if (directive == "product")
      {
        if (tokens.size() != 2)
        {
          addError(result, sourceName, lineNumber, "product directive expects exactly one value");
          continue;
        }

        accumulator.product = parseProduct(tokens[1]);
        if (accumulator.product == Product::Unknown)
          addError(result, sourceName, lineNumber, "unknown product: " + tokens[1]);
      }
      else if (directive == "version")
      {
        if (tokens.size() != 2)
        {
          addError(result, sourceName, lineNumber, "version directive expects exactly one value");
          continue;
        }
        accumulator.version = tokens[1];
      }
      else if (directive == "api-surface-methods")
      {
        if (tokens.size() != 2 || !parseInt(tokens[1], accumulator.implementedApiSurfaceMethods))
        {
          addError(result, sourceName, lineNumber, "api-surface-methods directive expects one non-negative integer");
          continue;
        }
      }
      else if (directive == "command-surface-entries")
      {
        if (tokens.size() != 2 || !parseInt(tokens[1], accumulator.implementedCommandSurfaceEntries))
        {
          addError(result, sourceName, lineNumber, "command-surface-entries directive expects one non-negative integer");
          continue;
        }
      }
      else if (directive == "capability")
      {
        if (tokens.size() != 2)
        {
          addError(result, sourceName, lineNumber, "capability directive expects exactly one value");
          continue;
        }

        Capability capability = Capability::ReadGameState;
        if (!parseCapability(tokens[1], capability))
        {
          addError(result, sourceName, lineNumber, "unknown capability: " + tokens[1]);
          continue;
        }
        accumulator.capabilities.push_back(capability);
      }
      else if (directive == "binding")
      {
        if (tokens.size() != 5)
        {
          addError(result, sourceName, lineNumber, "binding directive expects: binding <name> <kind> <requirement> <evidence>");
          continue;
        }

        BindingDirective binding;
        binding.name = tokens[1];
        binding.line = lineNumber;
        binding.evidence = tokens[4];

        if (!parseBindingKind(tokens[2], binding.kind))
        {
          addError(result, sourceName, lineNumber, "unknown binding kind: " + tokens[2]);
          continue;
        }
        if (!parseBindingRequirement(tokens[3], binding.requirement))
        {
          addError(result, sourceName, lineNumber, "unknown binding requirement: " + tokens[3]);
          continue;
        }
        if (binding.evidence.empty())
        {
          addError(result, sourceName, lineNumber, "binding evidence cannot be empty");
          continue;
        }

        accumulator.bindings.push_back(binding);
      }
      else if (directive == "structure")
      {
        if (tokens.size() != 4)
        {
          addError(result, sourceName, lineNumber, "structure directive expects: structure <name> <size> <requirement>");
          continue;
        }

        StructureDirective structure;
        structure.name = tokens[1];
        structure.line = lineNumber;
        if (!parseSize(tokens[2], structure.size) || structure.size == 0)
        {
          addError(result, sourceName, lineNumber, "structure size must be a positive integer");
          continue;
        }
        if (!parseBindingRequirement(tokens[3], structure.requirement))
        {
          addError(result, sourceName, lineNumber, "unknown structure requirement: " + tokens[3]);
          continue;
        }

        accumulator.structures.push_back(structure);
      }
      else if (directive == "field")
      {
        if (tokens.size() != 4)
        {
          addError(result, sourceName, lineNumber, "field directive expects: field <structure>.<field> <offset> <size>");
          continue;
        }

        const std::size_t separator = tokens[1].rfind('.');
        if (separator == std::string::npos || separator == 0 || separator + 1 >= tokens[1].size())
        {
          addError(result, sourceName, lineNumber, "field name must use <structure>.<field>");
          continue;
        }

        FieldDirective field;
        field.structureName = tokens[1].substr(0, separator);
        field.fieldName = tokens[1].substr(separator + 1);
        field.line = lineNumber;
        if (!parseSize(tokens[2], field.offset))
        {
          addError(result, sourceName, lineNumber, "field offset must be a non-negative integer");
          continue;
        }
        if (!parseSize(tokens[3], field.size) || field.size == 0)
        {
          addError(result, sourceName, lineNumber, "field size must be a positive integer");
          continue;
        }

        accumulator.fields.push_back(field);
      }
      else
      {
        addError(result, sourceName, lineNumber, "unknown manifest directive: " + directive);
      }
    }

    if (accumulator.product == Product::Unknown)
      addError(result, sourceName, 0, "manifest product is missing");
    if (accumulator.version.empty())
      addError(result, sourceName, 0, "manifest version is missing");
    if (accumulator.implementedApiSurfaceMethods <= 0)
      addError(result, sourceName, 0, "manifest API surface method count is missing");
    if (accumulator.implementedCommandSurfaceEntries <= 0)
      addError(result, sourceName, 0, "manifest command surface entry count is missing");

    if (!result.errors.empty())
      return result;

    RuntimeContract contract = accumulator.product == Product::StarCraftBroodWar1161
      ? makeBroodWar1161ParityContract()
      : makeRemasteredParityContract(accumulator.version);
    contract.version = accumulator.version;

    applyBindings(result, sourceName, contract, accumulator.bindings);
    applyStructures(result, sourceName, contract, accumulator.structures);
    applyFields(result, sourceName, contract, accumulator.fields);

    result.manifest.contract = std::move(contract);
    result.manifest.capabilities = std::move(accumulator.capabilities);
    result.manifest.implementedApiSurfaceMethods = accumulator.implementedApiSurfaceMethods;
    result.manifest.implementedCommandSurfaceEntries = accumulator.implementedCommandSurfaceEntries;
    result.loaded = result.errors.empty();
    return result;
  }

  RuntimeManifestLoadResult loadRuntimeManifestFile(const std::string& path)
  {
    std::ifstream input(path);
    if (!input)
    {
      RuntimeManifestLoadResult result;
      result.errors.push_back(path + ": unable to open runtime manifest");
      return result;
    }

    return loadRuntimeManifest(input, path);
  }
}
