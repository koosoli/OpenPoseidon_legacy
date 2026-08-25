#pragma once

#include <CLI/CLI.hpp>
#include <string>

namespace PoseidonTools
{

class PboCommand
{
  public:
    static void Setup(CLI::App& app);

  private:
    static int List(const std::string& pboPath, bool verbose);
    static int Show(const std::string& pboPath, const std::string& filePath);
    static int Extract(const std::string& pboPath, const std::string& outputDir, const std::string& fileFilter);
    static int Pack(const std::string& srcDir, const std::string& outputPbo, bool compress,
                    const std::string& prefix = {});

    static std::string StripPboExtension(const std::string& path);
    static std::string GetDefaultOutputDir(const std::string& pboPath);
    static bool CreateDirectories(const std::string& path);
    static std::string NormalizePath(const std::string& path);
    static std::string NormalizeBankPath(const std::string& path);
};

} // namespace PoseidonTools
