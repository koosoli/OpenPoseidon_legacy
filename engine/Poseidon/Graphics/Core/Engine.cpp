#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Rendering/Draw/Font.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>

#include <stdarg.h>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <Poseidon/Graphics/Rendering/GeometryPagePrototype.hpp>
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>

namespace Poseidon
{
RString GetUserParams();
}

namespace Poseidon
{

Engine* GEngine;

// Perf counter: written by RHI renderer, read by World::Simulate for diagnostics.
// Defined in Core (Engine.cpp) so headless Server builds link without Client.
int gPerfDrawCalls = 0;
bool gPerfDumpShapesOnce = false;
bool gPerfDumpShadowsOnce = false;
int gSmDepthCachedCasters = 0;
int gShadowFrozenCasters = 0;
int gShadowFrozenRouted = 0;

Engine::Engine()
    : _showTextFont(nullptr), _showTextColor(Color(HBlack)), _showTextSize(0), _showFps(0), _messageHandle(-1),
      _multitexturing(true), _nightVision(false), _accomodateEye(HWhite), _shadowFactor(0),

      _usrBrightness(1),

      _frameTime(0), _frameTime0(0),

      _startGame(Poseidon::Foundation::GlobalTickCount()), _textHandle(0)
{
    ResetFrameDuration();
}

void Engine::ResetFrameDuration()
{
    for (int i = 0; i < NFrameDurations; i++)
    {
        _frameDurations[i] = 70;
    }
    _lastFrameDuration = 70;
}

WindowMode IGraphicsEngine::GetCurrentWindowMode() const
{
    // Default placeholder for backends that haven't migrated to the
    // tri-state SetWindowMode API.  Returns Windowed; the live backends
    // (D3D11 / GL33 / D3D9) override with the real value.
    return WindowMode::Windowed;
}

void IGraphicsEngine::ListMonitors(FindArray<MonitorInfo>& ret)
{
    // Default placeholder: report a single "primary" monitor with
    // unknown geometry.  Real backends override with the live SDL
    // display list (SDL_GetDisplays).
    MonitorInfo info;
    info.index = 0;
    info.name = "Primary";
    info.w = 0;
    info.h = 0;
    info.refresh = 0;
    ret.Add(info);
}

DWORD Engine::GetAvgFrameDuration(int nFrames) const
{
    DWORD sum = 0;
    saturateMax(nFrames, NFrameDurations);
    for (int i = NFrameDurations - nFrames; i < NFrameDurations; i++)
    {
        sum += _frameDurations[i];
    }
    return sum / nFrames;
}

void Engine::SetMultitexturing(bool set)
{
    _multitexturing = set;
}

void Engine::FontDestroyed(Font* font)
{
#ifndef ACCESS_ONLY
    _fonts.RemoveFont(font);
#endif
}

Engine::~Engine()
{
#ifndef ACCESS_ONLY
    _fonts.Clear();
#endif
}
void Draw2DPars::Init()
{
    spec = NoZBuf | IsAlpha | ClampU | ClampV | IsAlphaFog;
    SetU(0, 1);
    SetV(0, 1);
}

int Engine::Width2D() const
{
    return toInt((_aspectSettings.uiBottomRightX - _aspectSettings.uiTopLeftX) * Width());
}

int Engine::Height2D() const
{
    return toInt((_aspectSettings.uiBottomRightY - _aspectSettings.uiTopLeftY) * Height());
}
int Engine::Left2D() const
{
    return toInt(_aspectSettings.uiTopLeftX * Width());
}
int Engine::Top2D() const
{
    return toInt(_aspectSettings.uiTopLeftY * Height());
}

Rect2DPixel Rect2DClipPixel(-1e6, -1e6, 2e6, 2e6);
Rect2DAbs Rect2DClipAbs(0, 0, 1e6, 1e6);

void Engine::Convert(Point2DAbs& to, const Point2DPixel& from)
{
    to.x = from.x + Left2D();
    to.y = from.y + Top2D();
}
void Engine::Convert(Point2DAbs& to, const Point2DFloat& from)
{
    to.x = from.x * Width2D() + Left2D();
    to.y = from.y * Height2D() + Top2D();
}

void Engine::Convert(Point2DPixel& to, const Point2DAbs& from)
{
    to.x = from.x - Left2D();
    to.y = from.y - Top2D();
}
void Engine::Convert(Point2DFloat& to, const Point2DAbs& from)
{
    to.x = (from.x - Left2D()) / Width2D();
    to.y = (from.y - Top2D()) / Height2D();
}

void Engine::Convert(Rect2DAbs& to, const Rect2DPixel& from)
{
    to.x = from.x + Left2D();
    to.y = from.y + Top2D();
    to.w = from.w;
    to.h = from.h;
}
void Engine::Convert(Rect2DAbs& to, const Rect2DFloat& from)
{
    float w2d = Width2D();
    float h2d = Height2D();
    to.x = from.x * w2d + Left2D();
    to.y = from.y * h2d + Top2D();
    to.w = from.w * w2d;
    to.h = from.h * h2d;
}
void Engine::Convert(Rect2DPixel& to, const Rect2DAbs& from)
{
    to.x = from.x - Left2D();
    to.y = from.y - Top2D();
    to.w = from.w;
    to.h = from.h;
}
void Engine::Convert(Rect2DFloat& to, const Rect2DAbs& from)
{
    float w2d = Width2D();
    float h2d = Height2D();
    to.x = (from.x - Left2D()) / w2d;
    to.y = (from.y - Top2D()) / h2d;
    to.w = from.w / w2d;
    to.h = from.h / h2d;
}

void Engine::Convert(Line2DAbs& to, const Line2DPixel& from)
{
    float l2d = Left2D();
    float t2d = Top2D();
    to.beg.x = from.beg.x + l2d;
    to.beg.y = from.beg.y + t2d;
    to.end.x = from.end.x + l2d;
    to.end.y = from.end.y + t2d;
}
void Engine::Convert(Line2DAbs& to, const Line2DFloat& from)
{
    float w2d = Width2D();
    float h2d = Height2D();
    float l2d = Left2D();
    float t2d = Top2D();
    to.beg.x = from.beg.x * w2d + l2d;
    to.beg.y = from.beg.y * h2d + t2d;
    to.end.x = from.end.x * w2d + l2d;
    to.end.y = from.end.y * h2d + t2d;
}
void Engine::Convert(Line2DPixel& to, const Line2DAbs& from)
{
    float l2d = Left2D();
    float t2d = Top2D();
    to.beg.x = from.beg.x - l2d;
    to.beg.y = from.beg.y - t2d;
    to.end.x = from.end.x - l2d;
    to.end.y = from.end.y - t2d;
}
void Engine::Convert(Line2DFloat& to, const Line2DAbs& from)
{
    float w2d = Width2D();
    float h2d = Height2D();
    float l2d = Left2D();
    float t2d = Top2D();
    to.beg.x = (from.beg.x - l2d) / w2d;
    to.beg.y = (from.beg.y - t2d) / h2d;
    to.end.x = (from.end.x - l2d) / w2d;
    to.end.y = (from.end.y - t2d) / h2d;
}

void Engine::PixelAlignXY(Point2DAbs& pos)
{
    pos.x = toInt(pos.x) + 0.5f;
    pos.y = toInt(pos.y) + 0.5f;
}
void Engine::PixelAlignX(Point2DAbs& pos)
{
    pos.x = toInt(pos.x) + 0.5f;
}
void Engine::PixelAlignY(Point2DAbs& pos)
{
    pos.y = toInt(pos.y) + 0.5f;
}
void Engine::PixelAlignXY(Point2DPixel& pos)
{
    pos.x = toInt(pos.x) + 0.5f;
    pos.y = toInt(pos.y) + 0.5f;
}
void Engine::PixelAlignX(Point2DPixel& pos)
{
    pos.x = toInt(pos.x) + 0.5f;
}
void Engine::PixelAlignY(Point2DPixel& pos)
{
    pos.y = toInt(pos.y) + 0.5f;
}

float Engine::PixelAlignedX(float x)
{
    return toInt(x) + 0.5f;
}
float Engine::PixelAlignedY(float y)
{
    return toInt(y) + 0.5f;
}

void Engine::SaveConfig()
{
    RString name = Poseidon::GetUserParams();

    ParamFile cfg;
    cfg.Parse(name);
    cfg.Add("brightness", _usrBrightness);
    cfg.Add("multitexturing", _multitexturing);
    cfg.Add("useWBuffer", IsWBuffer());
    cfg.Add("uiTopLeftX", _aspectSettings.uiTopLeftX);
    cfg.Add("uiTopLeftY", _aspectSettings.uiTopLeftY);
    cfg.Add("uiBottomRightX", _aspectSettings.uiBottomRightX);
    cfg.Add("uiBottomRightY", _aspectSettings.uiBottomRightY);
    cfg.Save(name);
}
void Engine::LoadConfig()
{
    RString name = Poseidon::GetUserParams();

    ParamFile cfg;
    cfg.Parse(name);

    SetBrightness(cfg.ReadValue("brightness", 1.6f)); // 1.6 = original CWA default (matches GraphicsConfig)
    if (cfg.FindEntry("multitexturing"))
    {
        SetMultitexturing(cfg >> "multitexturing");
    }
    _aspectSettings.uiTopLeftX = cfg.ReadValue("uiTopLeftX", 0.0f);
    _aspectSettings.uiTopLeftY = cfg.ReadValue("uiTopLeftY", 0.0f);
    _aspectSettings.uiBottomRightX = cfg.ReadValue("uiBottomRightX", 1.0f);
    _aspectSettings.uiBottomRightY = cfg.ReadValue("uiBottomRightY", 1.0f);
}

void Engine::SetFogColor(ColorVal fogColor)
{
    _fogColor = fogColor;
    FogColorChanged(_fogColor);
}

void Engine::ReinitCounters()
{
    _startGame = Poseidon::Foundation::GlobalTickCount();
}

void Engine::InitDraw(bool clear, PackedColor color)
{
    if (_nightVision)
    {
        _accomodateEye = Color(0, 8.0, 0);
    }
    else
    {
        _accomodateEye = HWhite;
    }
    _accomodateEye = _accomodateEye * _usrBrightness;
    _accomodateEye.SetA(1);
}

void Engine::FinishDraw()
{
    _frameCounter++;
    _frameTime = Poseidon::Foundation::GlobalTickCount();

    if (_frameTime0 > 0)
    {
        _lastFrameDuration = _frameTime - _frameTime0;
        for (int i = 0; i < NFrameDurations - 1; i++)
        {
            _frameDurations[i] = _frameDurations[i + 1];
        }
        _frameDurations[NFrameDurations - 1] = _lastFrameDuration;
    }
    _frameTime0 = _frameTime;
}

void Engine::NextFrame() {}

// Startup-only owned byte observation. No VFS/cache/header-derived lease is used.
bool Engine::ParseGeometryPageOriginalStartupInput(const std::string& path,const std::string& hash,
    uint64_t bytes,const std::string& key,GeometryPageOriginalStartupInput& destination)
{
    if(path.empty()||path.size()>=1024||hash.size()!=64||key.empty()||key.size()>160||!bytes||bytes>128u*1024u)return false;
    GeometryPageOriginalStartupInput result;result.expectedRawBytes=bytes;
    for(size_t i=0;i<path.size();++i) { const auto c=uint8_t(path[i]);if(c<32||c>126)return false;result.originalPath[i]=path[i]; }
    bool nonzero=false;
    const auto digit=[](char c)->int {return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;};
    for(size_t i=0;i<32;++i) {const int a=digit(hash[i*2]),b=digit(hash[i*2+1]);if(a<0||b<0)return false;
        result.expectedOriginal.sha256[i]=uint8_t(a*16+b);nonzero|=(a|b)!=0;}
    if(!nonzero)return false;
    std::array<uint64_t,7> values{};const char* at=key.data();const char* end=at+key.size();
    for(size_t i=0;i<values.size();++i) {const char* stop=at;while(stop!=end&&*stop!=':')++stop;
        if(stop==at)return false;const auto parsed=std::from_chars(at,stop,values[i]);
        if(parsed.ec!=std::errc{}||parsed.ptr!=stop)return false;
        if(i+1<values.size()) {if(stop==end)return false;at=stop+1;} else if(stop!=end)return false;}
    for(size_t i=2;i<values.size();++i)if(values[i]>UINT32_MAX)return false;
    if(!values[0]||!values[2]||values[3]==values[4]||!values[5]||!values[6])return false;
    auto& identity=result.expectedOriginal;identity.geometryOptions=values[0];identity.materialOptions=values[1];
    identity.producerVersion=uint32_t(values[2]);identity.coarseRepresentation=uint32_t(values[3]);
    identity.fineRepresentation=uint32_t(values[4]);identity.vertexLayout=uint32_t(values[5]);identity.materialMapping=uint32_t(values[6]);
    destination=result;return true;
}
bool Engine::GeometryPageOriginalIdentityMatches(const GeometryPageSourceIdentity& expected,const GeometryPages::SourceIdentity& actual)
{
    return expected.sha256==actual.sourceSha256&&expected.geometryOptions==actual.geometryOptions&&
        expected.materialOptions==actual.materialOptions&&expected.producerVersion==actual.producerVersion&&
        expected.coarseRepresentation==actual.coarseRepresentation&&expected.fineRepresentation==actual.fineRepresentation&&
        expected.vertexLayout==actual.vertexLayout&&expected.materialMapping==actual.materialMapping;
}
Engine::GeometryPageOriginalStatus Engine::ReadGeometryPageOriginalSnapshot(const GeometryPageOriginalStartupInput& input,
    std::vector<uint8_t>& destination)
{
    using Status=GeometryPageOriginalStatus;
    if(!Foundation::IsMainThread())return Status::WrongOwner;
    if(!input.expectedRawBytes||input.expectedRawBytes>128u*1024u)return Status::Capacity;
    size_t n=0;while(n<input.originalPath.size()&&input.originalPath[n]) {const auto c=uint8_t(input.originalPath[n]);if(c<32||c>126)return Status::Invalid;++n;}
    bool hash=false;for(auto b:input.expectedOriginal.sha256)hash|=b!=0;
    if(!n||n==input.originalPath.size()||!hash)return Status::Invalid;
    try {
        const auto path=std::filesystem::path(std::string(input.originalPath.data(),n));if(!path.is_absolute())return Status::Invalid;
        std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)return Status::IoFailure;
        const auto end=file.tellg();if(end<0)return Status::IoFailure;
        const auto size=uint64_t(end);if(size>128u*1024u)return Status::Capacity;
        if(size!=input.expectedRawBytes)return Status::IdentityMismatch;
        std::vector<uint8_t> owned(size);if(owned.capacity()>128u*1024u)return Status::Capacity;
        file.seekg(0);if(!file)return Status::IoFailure;
        file.read(reinterpret_cast<char*>(owned.data()),std::streamsize(size));
        if(file.gcount()!=std::streamsize(size)||!file)return Status::IoFailure;
        if(file.peek()!=std::char_traits<char>::eof())return Status::IdentityMismatch;
        if(!file.eof()||file.bad())return Status::IoFailure;
        Foundation::Sha256 sha;sha.Update(owned.data(),owned.size());const auto hex=sha.Hex();
        const auto digit=[](char c){return uint8_t(c<='9'?c-'0':c-'a'+10);};
        for(size_t i=0;i<32;++i)if(uint8_t(digit(hex[i*2])*16+digit(hex[i*2+1]))!=input.expectedOriginal.sha256[i])return Status::IdentityMismatch;
        destination=std::move(owned);return Status::Admitted;
    } catch(const std::bad_alloc&) {return Status::Capacity;}
      catch(const std::exception&) {return Status::IoFailure;}
}

} // namespace Poseidon
