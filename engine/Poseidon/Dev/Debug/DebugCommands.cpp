
#include <Poseidon/Dev/Debug/DebugCommands.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>

#include <algorithm>
#include <stddef.h>
#include <Poseidon/Foundation/Framework/Log.hpp>

namespace Poseidon::Dev
{
namespace DebugCommands
{

namespace
{
std::vector<Command>& Registry()
{
    static std::vector<Command> s;
    return s;
}
} // namespace

void Register(const Command& cmd)
{
    auto& r = Registry();
    for (const auto& existing : r)
    {
        if (std::string_view(existing.name) == std::string_view(cmd.name))
        {
            LOG_WARN(Core, "DebugCommands: duplicate registration of '{}'", cmd.name);
            return;
        }
    }
    r.push_back(cmd);
}

const Command* Find(std::string_view name)
{
    for (const auto& c : Registry())
        if (std::string_view(c.name) == name)
            return &c;
    return nullptr;
}

const std::vector<Command>& All()
{
    return Registry();
}

bool Run(std::string_view line, std::string& out)
{
    if (GWorld && GWorld->GetMode() == GModeNetware)
    {
        out = "developer console disabled in multiplayer";
        return false;
    }
    // Trim leading whitespace
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        line.remove_prefix(1);
    if (line.empty())
        return false;

    // Split on first whitespace.
    size_t sp = 0;
    while (sp < line.size() && line[sp] != ' ' && line[sp] != '\t')
        sp++;
    std::string_view name = line.substr(0, sp);
    std::string_view args;
    if (sp < line.size())
    {
        args = line.substr(sp + 1);
        while (!args.empty() && (args.front() == ' ' || args.front() == '\t'))
            args.remove_prefix(1);
    }

    const Command* cmd = Find(name);
    if (!cmd)
    {
        out = "unknown command: " + std::string(name);
        return false;
    }
    if (cmd->available && !cmd->available())
    {
        out = "command unavailable: " + std::string(name);
        return true;
    }
    cmd->invoke(args, out);
    return true;
}

} // namespace DebugCommands
} // namespace Poseidon::Dev
