#include <Poseidon/World/Scene/Scene.hpp>
#include "TextureBankWgpu.hpp"
#include "EngineWgpu.hpp" // AcquireProducerWindow / LastMemoryStats (REN-THR-013)

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <cctype>
#include <cjson/cJSON.h>
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Streams/PatnikArchiveReason.hpp>
#include <Poseidon/Asset/Formats/Enfusion/EnfusionMount.hpp>

#include <Poseidon/Graphics/Rendering/Shape/ObjectAdmitProfile.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>

namespace Poseidon
{

TextureBankWgpu::TextureBankWgpu(WgrRenderer* renderer) : _renderer(renderer) {}

TextureBankWgpu::~TextureBankWgpu()
{
    UnlockAllTextures();
    DeleteAllAnimated();
}

void TextureBankWgpu::BeginModelTextureCapture(std::vector<TextureWgpu*>& out)
{
    out.clear();
    _capture = &out;
}

void TextureBankWgpu::EndModelTextureCapture()
{
    _capture = nullptr;
}

void TextureBankWgpu::NoteTextureUse(TextureWgpu* texture)
{
    if (!texture)
        return;
    if (auto* observer = WgpuTextureBirthObserver::Active())
    {
        observer->Observe(texture, [texture]() {
            WgpuTextureBirthObserver::Facts facts;
            facts.texture = Ref<Texture>(texture);
            facts.dynamic = texture->IsDynamicTexture();
            facts.generatedDeferred = texture->HasDeferredGeneratedUpload();
            facts.handleBeforeUse = texture->GpuHandle();
            facts.slotLeaseBeforeUse = texture->SlotLease();
            const char* name = facts.dynamic ? texture->Name() : texture->SourceName();
            if (name)
            {
                size_t length = 0;
                while (length <= WgpuTextureBirthObserver::MaxName && name[length]) ++length;
                facts.sourceName = {name, length};
            }
            if (!facts.dynamic)
            {
                auto current = texture->CurrentArchiveSourceBinding(); // current mount, no pixel read
                BankReadMemberIdentity member;
                if (current && current->Request().CopyMemberIdentity(member))
                {
                    facts.member.volume = member.volume;
                    facts.member.archiveBytes = member.archiveBytes;
                    facts.member.offset = member.offset;
                    facts.member.bytes = member.bytes;
                    std::memcpy(facts.member.fileId.data(), member.fileId, sizeof(member.fileId));
                    facts.mountedCurrent = true;
                    facts.currentBirthLease = std::move(current);
                }
            }
            return facts;
        });
    }
    texture->_lastUsedFrame = _residencyFrame;
    if (_capture && std::find(_capture->begin(), _capture->end(), texture) == _capture->end())
        _capture->push_back(texture);
}

namespace
{
bool IsTerrainOwnedTexture(const TextureWgpu* texture)
{
    const char* name = texture ? texture->Name() : nullptr;
    if (!name)
        return false;
    auto contains = [name](const char* needle)
    {
        const size_t n = std::strlen(needle);
        for (const char* at = name; *at; ++at)
            if (strnicmp(at, needle, n) == 0)
                return true;
        return false;
    };
    return contains("\\layers\\") || contains("/layers/") || contains("_lca.") || contains("_lco.");
}
} // namespace

// The share of the dynamic budget object textures may hold before the LRU starts evicting.
//
// Half, because the other tracked pool is merged model geometry and it is NOT evictable from
// here: a split that leaves textures the whole budget is the unwinnable test this replaces, and
// one that leaves them too little evicts a working set that fits. Tunable because the right
// split is a per-corpus judgement -- Everon is geometry-heavy, an interior-heavy world is not.
static uint64_t TextureBudgetBytes(uint64_t budgetBytes)
{
    static const double share = []
    {
        if (const char* value = std::getenv("WGR_TEXTURE_BUDGET_SHARE"))
            return std::clamp(std::strtod(value, nullptr), 0.05, 1.0);
        return 0.5;
    }();
    return static_cast<uint64_t>(static_cast<double>(budgetBytes) * share);
}

// AST-012A -- say what the frame actually sampled, next to what the LRU believes.
//
// The point of this report is that the two are different quantities and nothing here has ever
// compared them. `_lastUsedFrame` says "a model referencing this texture is within the object
// draw distance"; the GPU's answer says "a fragment sampled it, and at THIS detail". A texture
// can be marked used every frame and never be sampled at all (behind a hill, behind a wall,
// back-facing, off-screen), and one that is sampled can be sampled only at mip 3 -- in which
// case the three finer levels are resident VRAM the frame demonstrably did not want.
//
// Three numbers come out of it, and each answers a different question:
//
//   * `unsampled` -- resident, slot-leased textures the GPU has never reported. This is the
//     LRU's false-keep rate: bytes held on the strength of a distance mark alone.
//   * the mip histogram -- of the textures the frame DID sample, how deep into the chain it
//     went. Everything to the right of mip 0 is chain uploaded above what was needed.
//   * `slack` -- those wasted levels as a fraction of resident top-level area. Area rather
//     than bytes because the bank does not carry a per-texture byte count and the formats
//     vary; within one texture the ratio between levels is exactly 4 per level regardless of
//     format, so the fraction is honest even though the absolute is not a byte count.
//
// It is a MEASUREMENT and nothing acts on it yet. Deliberately: a residency policy driven by
// this signal has to answer what happens to a texture the GPU stops reporting because the LRU
// already evicted it, and that question is not settled by the instrument that raises it.
void TextureBankWgpu::ReportMipFeedback()
{
    if (!_renderer)
        return;
    WgrMipFeedbackStats fb{};
    // One allocation, sized past any plausible bindless cap (8,192 today). The call writes
    // min(this, the renderer's slot count) and reports which, so oversizing costs 96 KB once
    // and cannot truncate.
    if (_mipDesired.size() < 65536)
    {
        _mipDesired.resize(65536);
        _mipAge.resize(65536);
    }
    EngineWgpu::AcquireProducerWindow("mip feedback");
    const uint32_t slots =
        wgr_get_mip_feedback(_renderer, &fb, _mipDesired.data(), _mipAge.data(),
                             static_cast<uint32_t>(_mipDesired.size()));
    if (slots == 0)
        return;
    if (fb.harvests == 0)
    {
        // NOT the same statement as "nothing was sampled", and reporting it as one is exactly
        // the failure mode a counter that cannot say "did not run" produces. Say it plainly.
        LOG_INFO(Graphics, "Wgpu mip feedback: no readback has completed yet (slots={} groups={})",
                 slots, fb.groups);
        return;
    }

    uint32_t resident = 0;      // resident, leased, evictable object textures
    uint32_t unsampled = 0;     // ... of which the GPU has never reported one fragment
    uint32_t stale = 0;         // ... reported, but not within one full rotation + margin
    uint32_t hist[5] = {0, 0, 0, 0, 0}; // desired mip 0,1,2,3,4+
    double areaResident = 0.0;  // sum of w*h over sampled textures
    double areaNeeded = 0.0;    // ... scaled by 4^-desired
    uint32_t markedButUnsampled = 0; // marked used THIS frame by the CPU, never sampled
    for (int i = 0; i < _texture.Size(); ++i)
    {
        TextureWgpu* texture = _texture[i];
        if (!texture || !texture->GpuHandle() || texture->IsDynamicTexture() ||
            IsTerrainOwnedTexture(texture))
            continue;
        const uint32_t slot = texture->SlotLease();
        // Slot 0 means the bindless array was at cap and this texture samples through
        // per-upload slots, where no stable identity exists to report against.
        if (slot == 0 || slot >= slots)
            continue;
        ++resident;
        const uint8_t desired = _mipDesired[slot];
        if (desired == 0xFF)
        {
            ++unsampled;
            // The direct comparison the whole report exists for: the CPU marked this used
            // recently enough that the LRU would refuse to evict it, and the GPU says no
            // fragment has ever sampled it.
            if (_residencyFrame < texture->LastUsedFrame() + 120)
                ++markedButUnsampled;
            continue;
        }
        // Two rotations plus a margin: one is the honest floor (a slot only gets its turn once
        // per rotation), so anything beyond two means it stopped being sampled rather than that
        // it is waiting its turn.
        if (fb.groups != 0 && _mipAge[slot] > fb.groups * 2 + 8)
            ++stale;
        hist[desired < 4 ? desired : 4] += 1;
        const double area = static_cast<double>(texture->AWidth(0)) * texture->AHeight(0);
        if (area <= 0.0)
            continue;
        areaResident += area;
        areaNeeded += area / static_cast<double>(1u << (2 * (desired < 15 ? desired : 15)));
    }
    const double slackPct =
        areaResident > 0.0 ? 100.0 * (1.0 - areaNeeded / areaResident) : 0.0;
    LOG_INFO(Graphics,
             "Wgpu mip feedback: resident={} sampled={} unsampled={} (LRU-fresh-but-unsampled={}) "
             "stale={} mip0={} mip1={} mip2={} mip3={} mip4+={} slack={:.1f}% "
             "slots={} observed={} groups={} harvests={}",
             resident, resident - unsampled, unsampled, markedButUnsampled, stale, hist[0], hist[1],
             hist[2], hist[3], hist[4], slackPct, slots, fb.observed, fb.groups, fb.harvests);
}

void TextureBankWgpu::UpdateAdaptiveTextureDetail()
{
    static const bool enabled = [] {
        const char* value = std::getenv("WGR_ADAPTIVE_TEXTURE_DETAIL");
        return value && value[0] == '1';
    }();
    if (!enabled || !_renderer || (_residencyFrame % 8) != 0 || !GScene || !GScene->GetCamera()) return;
    const auto position = GScene->GetCamera()->Position();
    const auto direction = GScene->GetCamera()->Direction();
    const float dx = position.X() - _adaptiveViewX, dy = position.Y() - _adaptiveViewY, dz = position.Z() - _adaptiveViewZ;
    const float alignment = direction.X() * _adaptiveDirX + direction.Y() * _adaptiveDirY + direction.Z() * _adaptiveDirZ;
    const float projectionX = GScene->GetCamera()->InvLeft() * (GEngine ? GEngine->Width() : 0);
    const float projectionY = GScene->GetCamera()->InvTop() * (GEngine ? GEngine->Height() : 0);
    if (!_adaptiveViewValid || dx * dx + dy * dy + dz * dz > 4.0f || alignment < 0.999f ||
        projectionX != _adaptiveProjectionX || projectionY != _adaptiveProjectionY) {
        ++_adaptiveViewEpoch;
        _adaptiveViewX = position.X(); _adaptiveViewY = position.Y(); _adaptiveViewZ = position.Z();
        _adaptiveDirX = direction.X(); _adaptiveDirY = direction.Y(); _adaptiveDirZ = direction.Z();
        _adaptiveViewValid = true;
        _adaptiveProjectionX = projectionX; _adaptiveProjectionY = projectionY;
    }
    if (_mipDesired.size() < 65536) { _mipDesired.resize(65536); _mipAge.resize(65536); }
    if (!EngineWgpu::TryAcquireProducerWindow()) return; // optional work must not serialize render overlap
    WgrMipFeedbackStats feedback{};
    const uint32_t slots = wgr_get_mip_feedback(_renderer, &feedback, _mipDesired.data(), _mipAge.data(), 65536);
    if (!slots) return;
    const size_t count = static_cast<size_t>(_texture.Size());
    // Round-robin prevents one frequently changing texture from starving the others.
    for (size_t scanned = 0; scanned < count; ++scanned) {
        _adaptiveCursor %= count;
        TextureWgpu* texture = _texture[static_cast<int>(_adaptiveCursor++)];
        if (!texture || IsTerrainOwnedTexture(texture) || !texture->AdaptiveMipCandidate()) continue;
        const uint32_t slot = texture->_slotLease;
        if (slot >= slots) continue;
        const bool moved = texture->_adaptiveViewEpoch != _adaptiveViewEpoch;
        int maxBias = std::min(3, texture->_nMipmaps - 1);
        while (maxBias > 0 && (texture->_mipmaps[maxBias]._w < 128 || texture->_mipmaps[maxBias]._h < 128)) --maxBias;
        const int target = texture->_mipDemand.Choose(texture->_uploadedFirstLevel, _mipDesired[slot],
            _mipAge[slot], feedback.groups, _residencyFrame, moved, maxBias);
        if (target == texture->_uploadedFirstLevel) {
            if (target == 0) texture->_adaptiveViewEpoch = _adaptiveViewEpoch;
            continue;
        }
        if (texture->ReplaceMipTail(target, _residencyFrame)) {
            texture->_adaptiveViewEpoch = _adaptiveViewEpoch;
            break; // at most one image / 4 MiB per eight frames
        }
    }
}

void TextureBankWgpu::TrimResidency()
{
    ++_residencyFrame;
    if (!_renderer)
        return;
    // Every 600 residency frames -- roughly ten seconds of play, and long enough that the
    // rotation (32 frames by default) has swept the table a dozen times over.
    if (_residencyFrame >= _mipReportDue)
    {
        _mipReportDue = _residencyFrame + 600;
        ReportMipFeedback();
    }
    // REN-THR-013: the on-budget frame (every frame on a stock world) reads the worker's
    // snapshot and never touches the renderer; only an over-budget frame opens the window,
    // re-reads the live figure and evicts as before.
    WgrMemoryStats stats{};
    if (!EngineWgpu::LastMemoryStats(stats) || stats.budget_bytes == 0)
        return;
    // TEXTURES against a TEXTURE budget, not textures+geometry against the whole one.
    //
    // `tracked_bytes` is textures plus geometry and only textures are evictable here, so on a
    // world whose GEOMETRY alone passes the budget the old test was unwinnable: it fired every
    // frame, evicted every candidate it had, and the number it chased went UP regardless.
    // Measured on Reforger Everon at WGR_DYNAMIC_VRAM_MB=1200 -- three consecutive trims
    // evicted 14/14, 4/4 and 1/1 candidates while tracked climbed 1558 -> 1596 -> 1682 MB.
    // Everon is ~84% geometry, so no amount of texture eviction could ever reach the target.
    //
    // That is not merely a wasted pass, it is a DESTRUCTIVE one: `wgr_model_register` bakes
    // each material's bindless slot permanently, so evicting a texture leaves every model
    // using it on the white fallback (see the pin note below, and InvalidateStaleGpuModels,
    // which now repairs exactly this). The owner's report -- a building's texture appearing,
    // then reverting to white, and never arriving at all closer in -- is this loop.
    const uint64_t textureBudget = TextureBudgetBytes(stats.budget_bytes);
    if (stats.object_texture_bytes <= textureBudget)
        return;
    EngineWgpu::AcquireProducerWindow("texture residency trim");
    if (!wgr_get_memory_stats(_renderer, &stats) || stats.object_texture_bytes <= textureBudget)
        return;

    // NOTE on the ResidentPins() check below. A pin means "some REGISTERED model references
    // this", and while pins were unconditionally exempt this LRU starved to 1-31 candidates on
    // DayZ Chernarus while tracked VRAM climbed past twice the budget. Making pinned textures
    // evictable needed two things, and both now exist:
    //
    //   * Slot identity -- FIXED (REN-RES-001). wgr_model_register still bakes each material's
    //     bindless slot permanently, but a slot is now LEASED by the TextureWgpu rather than by
    //     one upload, so an eviction leaves it reserved-and-white and the re-upload reclaims the
    //     same index. Evicting a pinned texture no longer corrupts anything.
    //   * Liveness -- now supplied by EngineWgpu::MarkRetainedModelLiveness, which sweeps the
    //     retained instance table against the camera and calls NoteTextureUse for the textures
    //     of every registered model that has an instance within the object draw distance. That
    //     is what makes _lastUsedFrame mean something on the retained path, where nothing calls
    //     EnsureUploaded() per frame. The refill half lives in InvalidateStaleGpuModels, which
    //     re-uploads an evicted texture whose model has come back into range (the lease keeps
    //     its slot, so no re-registration is needed) and only destroys the registration when
    //     that re-upload fails.
    //
    // The exemption therefore survives only as a FAILSAFE, gated on _retainedLivenessActive:
    // until the sweep has completed one full pass, a retained texture's _lastUsedFrame is still
    // the frame it was registered on, and evicting on that would be evicting on noise -- right
    // slots, white buildings, which is exactly the A/B measured at WGR_DYNAMIC_VRAM_MB=900 on
    // 2026-08-31. The flag is also cleared whenever the sweep cannot run at all (no camera, no
    // draw distance) or WGR_RETAINED_LIVENESS=0 pins it off for an A/B from one binary.
    //
    // Granularity worth knowing when reading an eviction log: the signal is PER MODEL and
    // distance-only (no frustum, no occlusion), and it lags by up to one sweep pass. It is
    // deliberately conservative in the keep direction -- it marks more than actually drew.
    // Full argument at TextureMipBias() in TextureWgpu.cpp.
    //
    // The grace window. IT IS NOT A SAFETY MARGIN, and reading it as one is the mistake this
    // comment exists to prevent (roadmap 4.3, which forbids "arbitrary frame-count delays" as
    // a lifetime mechanism -- correctly, but this is not one).
    //
    // What actually makes eviction safe is ownership, not elapsed frames. Audited 2026-08-31:
    // the Rust renderer contains NO call to wgpu's explicit `Texture::destroy()` /
    // `Buffer::destroy()` anywhere -- `Textures::destroy` removes the handle from the registry
    // and DROPS it. wgpu's resource tracker then keeps the underlying GPU allocation alive
    // until every submission still referencing it has completed. Separately, the texture's
    // BINDLESS SLOT INDEX is parked and only recycled in `frame_submitted()`, after the
    // frame's queue submit, because encoded draws and material buffers carry that index for
    // the rest of the frame. Those two together are the completed-work tracking 4.3 asks for.
    //
    // So this number is a THRASH heuristic: how long a texture must go unused before we are
    // willing to pay to load it again. Lowering it cannot cause a use-after-free; it can cause
    // a texture to be evicted and immediately re-uploaded. Raising it cannot make eviction
    // safer; it can only hold more bytes.
    uint64_t grace = 120;
    if (const char* value = std::getenv("WGR_TEXTURE_EVICT_GRACE_FRAMES"))
        grace = static_cast<uint64_t>(std::clamp<long long>(std::strtoll(value, nullptr, 10), 2, 36000));
    std::vector<TextureWgpu*> candidates;
    candidates.reserve(_texture.Size());
    for (int i = 0; i < _texture.Size(); ++i)
    {
        TextureWgpu* texture = _texture[i];
        if (!texture || !texture->GpuHandle() || texture->IsDynamicTexture() || IsTerrainOwnedTexture(texture))
            continue;
        if (texture->AdaptiveMipCandidate()) continue; // prototype retains a valid image; reports soft overage instead of white fallback
        const bool pinned = texture->ResidentPins() != 0;
        if (pinned && !_retainedLivenessActive)
            continue;
        // A pinned texture's mark comes from an incremental sweep, so it is allowed to be a
        // sweep-period old before it means anything. Unpinned (direct-draw) textures are marked
        // every frame they are used and keep the plain grace.
        const uint64_t effectiveGrace = pinned ? std::max(grace, _retainedLivenessMaxAgeFrames) : grace;
        if (_residencyFrame < texture->LastUsedFrame() + effectiveGrace)
            continue;
        candidates.push_back(texture);
    }
    std::sort(candidates.begin(), candidates.end(), [](const TextureWgpu* a, const TextureWgpu* b)
              { return a->LastUsedFrame() < b->LastUsedFrame(); });

    const uint64_t target = textureBudget * 9 / 10;
    uint32_t evicted = 0;
    // Counted separately because it is the number that says whether the liveness signal is
    // doing anything: before it existed this was structurally 0, and a run where evicted>0 but
    // pinned==0 means the LRU is still only reaching direct-draw textures.
    uint32_t evictedPinned = 0;
    for (TextureWgpu* texture : candidates)
    {
        if (texture->ResidentPins() != 0)
            ++evictedPinned;
        texture->EvictGpu();
        ++evicted;
        // Amortise the FFI query while still stopping close to the target.
        // Measured on the same quantity the gate used, so the loop can actually reach it.
        if ((evicted & 15u) == 0 && wgr_get_memory_stats(_renderer, &stats) &&
            stats.object_texture_bytes <= target)
            break;
    }
    if (evicted && wgr_get_memory_stats(_renderer, &stats))
        LOG_INFO(Graphics,
                 "Wgpu texture LRU: evicted={} (pinned={}) textureBytes={} textureBudget={} tracked={} budget={} "
                 "candidates={} liveness={}",
                 evicted, evictedPinned, stats.object_texture_bytes, textureBudget, stats.tracked_bytes,
                 stats.budget_bytes, candidates.size(), _retainedLivenessActive ? 1 : 0);
}

namespace
{
std::optional<std::string> NativeTextureIdentity(RStringB name)
{
    static const bool enabled = [] {
        const char* value = std::getenv("WGR_NATIVE_TEXTURE_KEYS");
        return !value || value[0] != '0';
    }();
    if (!enabled) return std::nullopt;
    return Asset::Formats::Enfusion::EnfusionMount::Instance().CanonicalPath(static_cast<const char*>(name));
}
}

int TextureBankWgpu::Find(RStringB name) const
{
    // Was a linear scan of _texture comparing `texture->GetName() == name`.
    // _textureByName uses that same operator== as its equality predicate, so the
    // result is identical - see the NameHash/NameEqual comment in the header.
    const auto it = _textureByName.find(name);
    if (it != _textureByName.end()) return it->second;
    if (const auto canonical = NativeTextureIdentity(name))
    {
        const auto native = _textureByName.find(RStringB(canonical->c_str()));
        if (native != _textureByName.end()) return native->second;
    }
    return -1;
}

namespace
{
std::string MaterialPathKey(const char* name)
{
    std::string result = name ? name : "";
    for (char& c : result)
        c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

bool SafeMaterialPath(const std::string& path)
{
    return !path.empty() && path.front() != '\\' && path.find(':') == std::string::npos &&
           path.find("..") == std::string::npos;
}
}

const TextureBankWgpu::LegacyMaterial* TextureBankWgpu::ResolveLegacyMaterial(const char* name)
{
    constexpr const char* manifest = "op_ground_materials\\materials.json";
    if (!_legacyMaterialsLoaded)
    {
        // The first UI textures may load before addon mounting is complete.
        if (!QIFStreamB::AutoBank(manifest))
            return nullptr;
        _legacyMaterialsLoaded = true;
        QIFStreamB file;
        file.AutoOpen(manifest);
        const int length = file.rest();
        if (length <= 0 || length > 65536)
            return nullptr;
        std::string text(static_cast<size_t>(length), '\0');
        file.read(text.data(), length);
        cJSON* root = cJSON_Parse(text.c_str());
        if (!root)
            return nullptr;
        const cJSON* api = cJSON_GetObjectItemCaseSensitive(root, "api");
        const cJSON* entries = cJSON_GetObjectItemCaseSensitive(root, "materials");
        if (cJSON_IsNumber(api) && api->valuedouble == 1 && cJSON_IsArray(entries))
        {
            const cJSON* entry;
            cJSON_ArrayForEach(entry, entries)
            {
                const auto field = [&](const char* key)
                {
                    const cJSON* value = cJSON_GetObjectItemCaseSensitive(entry, key);
                    return MaterialPathKey(cJSON_IsString(value) ? value->valuestring : "");
                };
                const std::string source = field("source");
                LegacyMaterial material{field("albedo"), field("normal"), field("sourceArchive")};
                const cJSON* detailMetres = cJSON_GetObjectItemCaseSensitive(entry, "detailNormalMetres");
                material.detailNormal = field("detailNormal");
                if (!material.detailNormal.empty())
                {
                    if (!SafeMaterialPath(material.detailNormal) || !material.detailNormal.ends_with("_nohq.paa") ||
                        !cJSON_IsNumber(detailMetres) || !(detailMetres->valuedouble >= 0.1 && detailMetres->valuedouble <= 100.0))
                    {
                        LOG_WARN(Graphics, "Default material: invalid terrain detail ignored for {}", source);
                        material.detailNormal.clear();
                    }
                    else
                        material.detailNormalMetres = static_cast<float>(detailMetres->valuedouble);
                }
                if (!SafeMaterialPath(source) || !SafeMaterialPath(material.albedo) ||
                    !SafeMaterialPath(material.normal) || !SafeMaterialPath(material.sourceArchive) ||
                    !(source.ends_with(".paa") || source.ends_with(".pac")) ||
                    !material.albedo.ends_with(".paa") || !material.normal.ends_with("_nohq.paa") ||
                    !material.sourceArchive.ends_with(".pbo"))
                {
                    LOG_WARN(Graphics, "Default material: rejected invalid entry {}", source);
                    continue;
                }
                if (!_legacyMaterials.emplace(source, std::move(material)).second)
                    LOG_WARN(Graphics, "Default material: duplicate source {} ignored", source);
            }
        }
        else
            LOG_WARN(Graphics, "Default material: incompatible catalog; original materials retained");
        cJSON_Delete(root);
    }
    const auto found = _legacyMaterials.find(MaterialPathKey(name));
    if (found == _legacyMaterials.end())
        return nullptr;
    const auto& material = found->second;
    // A texture supplied by a user mod must win even if it retains the stock name.
    // Bind only when the existing loader selected the exact original archive.
    const QFBank* original = QIFStreamB::AutoBank(name);
    if (!original)
        return nullptr;
    std::error_code ec;
    const auto actual = std::filesystem::weakly_canonical((const char*)original->GetOpenName(), ec);
    if (ec)
        return nullptr;
    std::string expectedPath = material.sourceArchive;
    std::replace(expectedPath.begin(), expectedPath.end(), '\\', '/');
    const auto expected = std::filesystem::weakly_canonical(expectedPath, ec);
    if (ec || MaterialPathKey(actual.generic_string().c_str()) != MaterialPathKey(expected.generic_string().c_str()))
        return nullptr;
    return &material;
}

std::string TextureBankWgpu::LegacyNormalPath(const char* name)
{
    const int loaded = Find(name);
    if (loaded >= 0 && !_texture[loaded]->_sourceName.GetLength())
        return {}; // Failed albedo replacement must not leave half a material active.
    if (const auto* material = ResolveLegacyMaterial(name))
        return material->normal;
    return {};
}

Ref<Texture> TextureBankWgpu::LoadLegacyEnhancementNormal(const char* name)
{
    const std::string key(name);
    const size_t dot = key.find_last_of('.');
    if (dot == std::string::npos || dot == 0)
        return {};
    const std::string mapped = LegacyNormalPath(name);
    const char* normalMode = std::getenv("POSEIDON_VISUAL_UPGRADE_NORMALS");
    if (!mapped.empty() && normalMode && std::strcmp(normalMode, "0") == 0)
        return {}; // Albedo-only arm, without disabling unrelated authored normals.
    const std::string candidate = mapped.empty() ? key.substr(0, dot) + "_nohq.paa" : mapped;
    QFBank* bank = QIFStreamB::AutoBank(candidate.c_str());
    const bool inBank = bank && bank->FileExists(candidate.c_str() + bank->GetPrefix().GetLength());
    if (!inBank && !QIFStream::FileExists(candidate.c_str()))
        return {}; // Never bind a missing-file placeholder as a surface normal.
    return Load(candidate.c_str());
}

Ref<Texture> TextureBankWgpu::LoadLegacyTerrainDetailNormal(const char* name, float& repeatsPerMetre)
{
    repeatsPerMetre = 0.0f;
    if (LegacyNormalPath(name).empty())
        return {};
    const char* normalMode = std::getenv("POSEIDON_VISUAL_UPGRADE_NORMALS");
    const char* detailMode = std::getenv("POSEIDON_VISUAL_UPGRADE_DETAIL_NORMALS");
    if ((normalMode && std::strcmp(normalMode, "0") == 0) ||
        (detailMode && std::strcmp(detailMode, "0") == 0))
        return {};
    const auto* material = ResolveLegacyMaterial(name);
    if (!material || material->detailNormal.empty() || material->detailNormalMetres <= 0.0f)
        return {};
    const auto& path = material->detailNormal;
    QFBank* bank = QIFStreamB::AutoBank(path.c_str());
    if (!(bank && bank->FileExists(path.c_str() + bank->GetPrefix().GetLength())) &&
        !QIFStream::FileExists(path.c_str()))
        return {};
    repeatsPerMetre = 1.0f / material->detailNormalMetres;
    return Load(path.c_str());
}

Ref<Texture> TextureBankWgpu::Load(RStringB name)
{
    int index = Find(name);
    if (index >= 0)
    {
        if (PatnikArchiveReason::Enabled() && PatnikArchiveReason::Matches((const char*)name) &&
            PatnikArchiveReason::ReserveRow())
        {
            auto* existing = static_cast<TextureWgpu*>((Texture*)_texture[index]);
            LOG_INFO(Graphics, "patnik_archive_reason stage=textureBank reason=existing mips={} initBound={} gpu={}",
                existing->HasInitializedWarmSource(), existing->HasInitializedArchiveBinding(),
                existing->GpuHandle() != 0);
        }
        return (Texture*)_texture[index];
    }

    int iFree = _texture.Add();
    TextureWgpu* texture = new TextureWgpu(this);
    texture->SetName(name);
    if (const auto* material = ResolveLegacyMaterial(name))
    {
        texture->_sourceName = material->albedo.c_str();
        LOG_INFO(Graphics, "Default material: {} -> {} (original texture identity preserved)",
                 (const char*)name, material->albedo);
    }
    _texture[iFree] = texture;
    // Key off the stored name rather than the argument: SetName copies the RStringB
    // verbatim, so the two share one interned buffer, and this cannot drift if that
    // ever stops being true.
    _textureByName.emplace(texture->GetName(), iFree);
    // Same bank/lease/source, not a duplicate decoded source or GPU upload.
    // Keep the original texture name for material attribution and diagnostics.
    if (const auto canonical = NativeTextureIdentity(name))
        _textureByName.emplace(RStringB(canonical->c_str()), iFree);
    // Admission attribution: Init() opens the file through GFileServer to parse the PAA
    // header. Timed only while an admit scope is open (one bool load otherwise), because
    // whether a cold model's `adapt` milliseconds are these opens or geometry work is the
    // question the split exists to answer.
    render::ObjectAdmitProfile& admit = render::GObjectAdmitProfile;
    if (admit.active)
    {
        const auto began = std::chrono::steady_clock::now();
        texture->Init();
        admit.texHeaderLoadMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        ++admit.texHeaderLoads;
    }
    else
    {
        texture->Init();
    }
    if (PatnikArchiveReason::Enabled() && PatnikArchiveReason::Matches((const char*)name) &&
        PatnikArchiveReason::ReserveRow())
        LOG_INFO(Graphics, "patnik_archive_reason stage=textureBank reason=new mips={} initBound={} gpu={}",
            texture->HasInitializedWarmSource(), texture->HasInitializedArchiveBinding(),
            texture->GpuHandle() != 0);
    return texture;
}

MipInfo TextureBankWgpu::UseMipmap(Texture* tex, int /*level*/, int /*top*/)
{
    if (auto* t = static_cast<TextureWgpu*>(tex))
    {
        t->EnsureUploaded();
    }
    return MipInfo(tex, 0);
}

Texture* TextureBankWgpu::CreateDynamic(int w, int h, const void* rgba, uint32_t size, bool mipmap,
                                       int alphaClass)
{
    auto* texture = new TextureWgpu(this);
    texture->InitDynamic(w, h, rgba, size, mipmap);
    // A dynamic texture has no ITextureSource, so GetAlphaClass has nothing to scan
    // and keeps the base Opaque. That is right for a font atlas and wrong for a leaf
    // card whose coverage the caller just folded in by hand, which draws as a solid
    // quad with no warning anywhere.
    if (alphaClass >= 0)
        texture->SetAlphaClassHint(static_cast<AlphaStats::Kind>(alphaClass));
    return texture;
}

void TextureBankWgpu::UpdateDynamic(Texture* tex, const void* rgba, uint32_t size)
{
    if (auto* t = static_cast<TextureWgpu*>(tex))
    {
        t->UpdateDynamic(rgba, size);
    }
}

} // namespace Poseidon
