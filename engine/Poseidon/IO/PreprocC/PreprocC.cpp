
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

  protected:
    QIStream* OnEnterInclude(const char* filename) override
    {
        std::string resolved = filename;
        if (!QIFStreamB::FileExist(resolved.c_str()) && !_includeStack.empty())
        {
            std::string dir = DirectoryOf(_includeStack.back().c_str());
            if (!dir.empty())
            {
                resolved = dir + "/" + filename;
            }
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
