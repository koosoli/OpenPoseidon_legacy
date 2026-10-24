
#include <Poseidon/IO/PreprocC/PreprocC.hpp>
#include <Poseidon/IO/PreprocC/Preproc.h>

#include <Poseidon/IO/Streams/QBStream.hpp>
#include <string>
#include <vector>
#include <string.h>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>

namespace Poseidon
{
class Preprocessor : public Preproc
{
    std::vector<std::string> _includeStack;

    static std::string DirectoryOf(const char* path)
    {
        // the LAST separator of either kind: an include written with a backslash
        // ("UIScripts\\vendorUI.hpp") under a forward-slash path keeps its folder,
        // so that file's own includes resolve beside it (Arma 1 Merc shop missions)
        const char* fwd = strrchr(path, '/');
        const char* back = strrchr(path, '\\');
        const char* slash = !fwd ? back : !back ? fwd : (fwd > back ? fwd : back);
        if (!slash)
            return {};
        return std::string(path, slash - path);
    }

    // "a\b\..\c" -> "a\c" (either separator in, backslashes out)
    static std::string CollapseDots(const std::string& path)
    {
        std::vector<std::string> parts;
        std::string part;
        const auto flush = [&]
        {
            if (part == "..")
            {
                if (!parts.empty() && parts.back() != "..")
                    parts.pop_back();
                else
                    parts.push_back(part);
            }
            else if (!part.empty() && part != ".")
                parts.push_back(part);
            part.clear();
        };
        for (char c : path)
        {
            if (c == '\\' || c == '/')
                flush();
            else
                part += c;
        }
        flush();
        std::string out;
        for (size_t i = 0; i < parts.size(); i++)
            out += (i ? "\\" : "") + parts[i];
        return out;
    }

  protected:
    QIStream* OnEnterInclude(const char* filename) override
    {
        std::string resolved = filename;
        // An absolute include ("\CA\Missions_e\campaign\loadingscreen.ext", every Arrowhead
        // campaign description.ext) names an archive path from the root; the banks know it
        // without the leading separator. Unresolved, the whole description.ext failed.
        if (!QIFStreamB::FileExist(resolved.c_str()) && (resolved[0] == '\\' || resolved[0] == '/'))
        {
            const size_t start = resolved.find_first_not_of("\\/");
            const std::string rooted = start == std::string::npos ? std::string() : resolved.substr(start);
            if (!rooted.empty() && QIFStreamB::FileExist(rooted.c_str()))
                resolved = rooted;
        }
        // a relative include resolves beside the file that includes it, innermost first; ".." steps
        // out of that folder (Old Man's RscDatetimeSlider.inc includes "..\defines.inc"), which
        // the banks do not know written out
        for (auto it = _includeStack.rbegin(); !QIFStreamB::FileExist(resolved.c_str()) && it != _includeStack.rend();
             ++it)
        {
            const std::string dir = DirectoryOf(it->c_str());
            if (dir.empty())
                continue;
            const std::string beside = CollapseDots(dir + "/" + filename);
            if (QIFStreamB::FileExist(beside.c_str()))
                resolved = beside;
        }

        if (!QIFStreamB::FileExist(resolved.c_str()))
        {
            return nullptr;
        }

        QIFStreamB* stream = new QIFStreamB();
        stream->AutoOpen(resolved.c_str());
        _includeStack.push_back(resolved);
        return stream;
    }
    void OnExitInclude(QIStream* stream) override
    {
        if (!_includeStack.empty())
        {
            _includeStack.pop_back();
        }
        if (stream)
        {
            delete static_cast<QIFStreamB*>(stream);
        }
    }
};

bool CPreprocessorFunctions::Preprocess(QOStream& out, const char* name)
{
    Preprocessor preprocessor;
    if (!preprocessor.Process(&out, name))
    {
        Poseidon::Foundation::ErrorMessage("Preprocessor failed on file %s - error %d.", name, preprocessor.error);
        return false;
    }
    return true;
}

} // namespace Poseidon
