// Adapted from the author-supplied CWR-Physical Inventory source donation (2026).
// Distributed under this project's GPL-3.0-or-later licence and Section 7 terms; see LICENSE.
#include "DisplayInventory.hpp"
#include <Poseidon/Core/Config/CombatAssistSettings.hpp>
#include "../../World/Entities/Weapons/ItemMass.hpp"

#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/World/Entities/Weapons/Weapons.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Rendering/Draw/Font.hpp>
#include <Poseidon/Graphics/Textures/TexturePreload.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/Core/resincl.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>
#include <Poseidon/Input/UserAction.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/UI/Controls/UIControlsExtShared.hpp>

#include <cstdio>   // snprintf
#include <cstring>  // strcmp
#include <string>
#include <algorithm>
#include "InventoryResources.hpp"
#include <Poseidon/IO/PackFiles.hpp>

namespace Poseidon
{

// Resolves a picture reference to a loadable path (adds the dtaExt\ prefix so
// pbo-packed equip icons resolve, searches mission/campaign dirs, etc.) - the
// same call CHTMLContainer::AddImage makes before GlobLoadTexture. Defined in
// OptionsUI.cpp.
RString FindPicture(RString name);

namespace
{

inline float Saturate(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

bool IconFileExists(const char *path);   // defined below; used to pick an icon path

// Original project-owned category icons are used only for known classic classes.
// Addon classes retain their own configured artwork; no broad name heuristics
// replace it with a generic rifle/magazine silhouette.
const char* OwnedIconCategory(const InvCell& cell)
{
    const RStringB className = cell.ClassName();
    const char* rawName = (const char*)className;
    std::string name = rawName ? rawName : "";
    for (char& c : name)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    struct Entry { const char* name; const char* weapon; const char* magazine; };
    static const Entry entries[] = {
        {"m16", "m16", "magazine"}, {"m16grenadelauncher", "rifle", "magazine"},
        {"xm177e2", "rifle", "magazine"}, {"m4", "rifle", "magazine"},
        {"g36a", "rifle", "magazine"}, {"steyr", "rifle", "magazine"}, {"fal", "rifle", "magazine"},
        {"ak47", "ak_rifle", "curved_magazine"}, {"ak47cz", "ak_rifle", "curved_magazine"},
        {"ak74", "ak_rifle", "curved_magazine"}, {"ak74su", "ak_rifle", "curved_magazine"},
        {"aks74u", "ak_rifle", "curved_magazine"},
        {"ak47grenadelauncher", "ak_rifle", "curved_magazine"},
        {"ak74grenadelauncher", "ak_rifle", "curved_magazine"},
        {"m21", "scoped_rifle", "magazine"}, {"svddragunov", "scoped_rifle", "curved_magazine"},
        {"huntingrifle", "scoped_rifle", "magazine"}, {"kozlice", "rifle", "magazine"},
        {"hk", "smg", "curved_magazine"}, {"mp5", "smg", "curved_magazine"},
        {"uzi", "smg", "magazine"}, {"skorpion", "smg", "magazine"}, {"bizon", "smg", "magazine"},
        {"m60", "machinegun", "magazine"}, {"pk", "machinegun", "magazine"},
        {"lawlauncher", "launcher", "rocket"}, {"rpglauncher", "launcher", "rocket"},
        {"carlgustavlauncher", "launcher", "rocket"}, {"aalauncher", "launcher", "rocket"},
        {"9k32launcher", "launcher", "rocket"}, {"strelalauncher", "launcher", "rocket"},
        {"law", "launcher", "rocket"}, {"rpg", "launcher", "rocket"},
        {"carlgustav", "launcher", "rocket"}, {"aalauncherrocket", nullptr, "rocket"},
        {"beretta", "pistol", "magazine"}, {"berettamag", nullptr, "magazine"},
        {"tokarev", "pistol", "magazine"}, {"tokarevmag", nullptr, "magazine"},
        {"cz75", "pistol", "magazine"}, {"cz75mag", nullptr, "magazine"},
        {"glock", "pistol", "magazine"}, {"glocks", "pistol", "magazine"},
        {"glockmag", nullptr, "magazine"}, {"bizonmag", nullptr, "magazine"},
        {"kozliceshell", nullptr, "magazine"}, {"kozliceball", nullptr, "magazine"},
        {"revolver", "pistol", "magazine"}, {"sam", "pistol", "magazine"},
        {"binocular", "binoculars", nullptr}, {"nvgoggles", "nvg", nullptr},
        {"handgrenade", nullptr, "grenade"}, {"grenadelauncher", nullptr, "grenade"},
        {"smokeshell", nullptr, "smoke"}, {"smokeshellred", nullptr, "smoke"},
        {"smokeshellgreen", nullptr, "smoke"}, {"pipebomb", nullptr, "satchel"},
        {"timebomb", nullptr, "satchel"}, {"mine", nullptr, "mine"}, {"minee", nullptr, "mine"},
    };
    for (const Entry& entry : entries)
        if (name == entry.name)
            return cell.kind == InvItemKind::Weapon ? entry.weapon : entry.magazine;
    return nullptr;
}

inline RString InvItemStockPicturePath(const InvCell& cell)
{
    RString name = cell.PictureName();
    if (name.GetLength() == 0)
        return RString();
    if (name[0] == '\\')
        return FindPicture(name);
    if (cell.kind == InvItemKind::Weapon)
        return FindPicture(RString("equip\\w\\w_") + name + RString(".paa"));
    if (cell.kind == InvItemKind::Magazine)
        return FindPicture(RString("equip\\m\\m_") + name + RString(".paa"));
    return FindPicture(name);
}

// Local per-class overrides remain optional; no donated/reworked game artwork
// is shipped in invicons. Installed addon textures continue to work without it.
inline RString OptionalIconPath(const InvCell& cell)
{
    const char* prefix = cell.kind == InvItemKind::Weapon ? "invicons\\w_" : "invicons\\m_";
    RString path = RString(prefix) + RString((const char*)cell.ClassName()) + RString(".paa");
    if (IconFileExists(path))
        return path;
    RString name = cell.PictureName();
    if (name.GetLength() == 0)
        return RString();
    if (name[0] == '\\')
    {
        int slash = name.GetLength() - 1;
        while (slash >= 0 && name[slash] != '\\' && name[slash] != '/')
            --slash;
        path = RString("invicons\\") + name.Substring(slash + 1, name.GetLength());
    }
    else
        path = RString(prefix) + name + RString(".paa");
    return IconFileExists(path) ? path : RString();
}

inline RString InvItemPicturePath(const InvCell& cell)
{
    const char* category = OwnedIconCategory(cell);
    if (category)
    {
        RString path = RString("assets\\inventory\\") + RString(category) + RString(".paa");
        if (IconFileExists(path))
            return path;
        // Existing local overrides can fill a missing original category icon.
        path = OptionalIconPath(cell);
        if (path.GetLength() > 0)
            return path;
    }
    // For unknown/addon classes the configured image is always preferred.
    RString picture = cell.PictureName();
    if (picture.GetLength() > 0)
    {
        if (IconFileExists(picture))
            return picture;
        RString resolved = FindPicture(picture);
        if (resolved.GetLength() > 0 && IconFileExists(resolved))
            return resolved;
    }
    RString stock = InvItemStockPicturePath(cell);
    if (stock.GetLength() > 0 && IconFileExists(stock))
        return stock;
    return OptionalIconPath(cell);
}

inline Ref<Texture> LoadInvItemIcon(const InvCell& cell)
{
    RString path = InvItemPicturePath(cell);
    return path.GetLength() > 0 ? GlobLoadTexture(path) : Ref<Texture>();
}

// Dark translucent panels, lighter grey
// item tiles with subtle borders, an orange selection accent, green ammo bars.
const PackedColor ColPaneBg      (Color(0.09f, 0.10f, 0.12f, 0.80f));
const PackedColor ColPaneHeader  (Color(0.14f, 0.15f, 0.17f, 0.90f));
const PackedColor ColPaneBorder  (Color(0.42f, 0.45f, 0.50f, 0.45f));
const PackedColor ColTileBg      (Color(0.22f, 0.24f, 0.27f, 0.55f));
const PackedColor ColTileBorder  (Color(0.62f, 0.65f, 0.70f, 0.75f));   // lighter grey outline
const PackedColor ColTileHover   (Color(0.32f, 0.35f, 0.40f, 0.75f));
const PackedColor ColAccent      (Color(0.80f, 0.83f, 0.87f, 1.00f));   // neutral highlight (no orange)
const PackedColor ColText        (Color(0.90f, 0.92f, 0.94f, 1.00f));
const PackedColor ColTextDim     (Color(0.62f, 0.66f, 0.72f, 1.00f));
const PackedColor ColAmmoBar     (Color(0.35f, 0.78f, 0.38f, 1.00f));   // green
const PackedColor ColAmmoBarBg   (Color(0.13f, 0.14f, 0.16f, 0.85f));
const PackedColor ColOkTint      (Color(0.20f, 0.50f, 0.26f, 0.60f));
const PackedColor ColBadTint     (Color(0.55f, 0.20f, 0.20f, 0.60f));

// Weight bar colour bands. Green up to the fighting-load figure, amber up to
// the approach-march cap, red past it (which shouldn't be reachable via
// CanTransfer, but the bar still needs a colour if somehow over via a
// forced/edited save).
const PackedColor ColWeightOk     (Color(0.25f, 0.65f, 0.30f, 1.00f));
const PackedColor ColWeightHeavy  (Color(0.80f, 0.60f, 0.15f, 1.00f));
const PackedColor ColWeightOver   (Color(0.75f, 0.20f, 0.20f, 1.00f));

// --- Accent theming --------------------------------------------------------
// The edit menu exposes an accent colour with two independent switches: tint
// borders/outlines, and tint panel backgrounds/headers. Highlights (selection,
// slider thumbs, hints) adopt the accent whenever either switch is on.
static bool AccentActive() { return InventoryAccentTheming() || InventoryAccentBorders(); }

static PackedColor AccentColor()
{
    if (!AccentActive()) return ColAccent;
    float r, g, b; InventoryAccentColor(r, g, b);
    return PackedColor(Color(r, g, b, 1.0f));
}

// Border/outline colour: accent-tinted when the borders switch is on, else base.
static PackedColor AccentBorderCol(PackedColor base, float a = 0.75f)
{
    if (!InventoryAccentBorders()) return base;
    float r, g, b; InventoryAccentColor(r, g, b);
    return PackedColor(Color(r, g, b, a));
}

// Panel/header fill: a dim accent wash when the theming switch is on, else base.
// `mul` darkens the accent so text stays readable; `a` is the fill opacity.
static PackedColor AccentPanelCol(PackedColor base, float mul, float a)
{
    if (!InventoryAccentTheming()) return base;
    float r, g, b; InventoryAccentColor(r, g, b);
    return PackedColor(Color(r * mul, g * mul, b * mul, a));
}

constexpr float TextSize = 0.022f;
constexpr float TilePad  = 0.0015f;   // small icon inset / grid gap so tiles sit snug

void FillRect(float x, float y, float w, float h, PackedColor col, float alpha)
{
    // A solid colour fill still needs a texture bound, or Draw2D rasterizes
    // nothing. The engine's own controls fill rects with the preloaded white
    // texture (see UIControls.cpp CStatic/progress-bar draws) - do the same.
    Texture *white = GLOB_SCENE->Preloaded(TextureWhite);
    Draw2DPars pars;
    pars.mip = GLOB_ENGINE->TextBank()->UseMipmap(white, 0, 0);
    pars.SetColor(ModAlpha(col, alpha));
    pars.spec = NoZBuf | IsAlpha;
    pars.SetU(0, 1);
    pars.SetV(0, 1);

    const float sw = GLOB_ENGINE->Width2D();
    const float sh = GLOB_ENGINE->Height2D();
    GLOB_ENGINE->Draw2D(pars, Rect2DPixel(x * sw, y * sh, w * sw, h * sh));
}

// Thin border made of four edge rects (Draw2D only fills rects).
void DrawBorder(float x, float y, float w, float h, float t, PackedColor col, float alpha)
{
    FillRect(x, y, w, t, col, alpha);              // top
    FillRect(x, y + h - t, w, t, col, alpha);      // bottom
    FillRect(x, y, t, h, col, alpha);              // left
    FillRect(x + w - t, y, t, h, col, alpha);      // right
}

// Icons use their complete source rectangle. This works with stock PBO icons
// without external crop metadata or expensive per-pixel readback.
void IconContentUV(Texture *tex, float &u0, float &u1, float &v0, float &v1, float *outAspect = nullptr)
{
    u0 = v0 = 0.0f; u1 = v1 = 1.0f;
    if (outAspect)
        *outAspect = tex && tex->AHeight() > 0 ? float(tex->AWidth()) / tex->AHeight() : 1.0f;
}

// Draw an icon fitted into the target rect preserving aspect ratio (letterboxed
// + centred) so wide icons do not stretch. The source alpha and full rectangle
// are retained for both original category artwork and addon-configured icons.
void DrawIcon(Texture *tex, float x, float y, float w, float h, float alpha,
              float sizeMul = 1.0f, float offX = 0.0f, float offY = 0.0f,
              PackedColor tint = PackedWhite)
{
    if (!tex)
    {
        return;
    }
    tex->SetMaxSize(1024);

    float u0, u1, v0, v1, aspect = 1.0f;
    IconContentUV(tex, u0, u1, v0, v1, &aspect);

    const float sw = GLOB_ENGINE->Width2D();
    const float sh = GLOB_ENGINE->Height2D();

    const float pw = w * sw;    // tile size in pixels
    const float ph = h * sh;

    // Size by the icon's ASPECT RATIO (cached, mip-stable), not its absolute texel
    // size. AWidth/AHeight report the currently-loaded mip, which changes as the
    // texture streams in and out - using them directly made icons visibly pulse in
    // size every couple of seconds. Fitting a stable aspect into the tile holds the
    // size steady; a rifle still reads larger than a mag because its tile is larger.
    // Fit the complete source into the tile with a small surrounding pad.
    const float CellFill = 0.92f;

    float dw = pw * CellFill;
    float dh = dw / (aspect > 0.0f ? aspect : 1.0f);
    if (dh > ph * CellFill)
    {
        dh = ph * CellFill;
        dw = dh * aspect;
    }

    // Per-icon tuning (debug tool): scale the fitted image and nudge it within the
    // tile. offX/offY are fractions of the tile (+x right, +y down).
    dw *= sizeMul;
    dh *= sizeMul;

    const float px = x * sw + (pw - dw) * 0.5f + offX * pw;
    const float py = y * sh + (ph - dh) * 0.5f + offY * ph;

    Draw2DPars pars;
    pars.mip = GLOB_ENGINE->TextBank()->UseMipmap(tex, 0, 0);
    pars.SetColor(ModAlpha(tint, alpha));   // tint multiplies the icon (black -> silhouette)
    pars.spec = NoZBuf | IsAlpha;
    pars.SetU(u0, u1);
    pars.SetV(v0, v1);
    GLOB_ENGINE->Draw2D(pars, Rect2DPixel(px, py, dw, dh));
}

// Image-size slider range (multiplier applied on top of the normal cell fill).
static constexpr float kSizeMin = 0.30f;
static constexpr float kSizeMax = 1.70f;

// Per-icon size/offset tuning for a cell (debug tool). Defaults if no override.
// The equipped slot, inventory grid and vicinity each carry independent size+nudge.
void CellTune(const InvCell &cell, TuneScope scope, float &sz, float &ox, float &oy)
{
    sz = 1.0f; ox = 0.0f; oy = 0.0f;
    if (cell.ClassName().GetLength() > 0)
        if (const IconTune *t = FindIconTune((const char *)IconTuneKey(cell)))
        {
            switch (scope)
            {
                case TuneScope::Loadout:  sz = t->lsize; ox = t->lox; oy = t->loy; break;
                case TuneScope::Vicinity: sz = t->vsize; ox = t->vox; oy = t->voy; break;
                default:                  sz = t->size;  ox = t->ox;  oy = t->oy;  break;
            }
        }

    // Fixed icon-size rules in the INVENTORY grid (not the loadout slots or vicinity):
    // binoculars draw at 90%, rifles/launchers (but not the laser designator) at 125%.
    if (scope != TuneScope::Loadout && scope != TuneScope::Vicinity &&
        cell.kind == InvItemKind::Weapon && cell.weapon)
    {
        const int wt = cell.weapon->_weaponType;
        const char *cn = (const char *)cell.ClassName();
        const bool laser = cn && (strcmp(cn, "LaserDesignator") == 0 ||
                                  strcmp(cn, "LaserDesignatorOH") == 0);
        if (wt & MaskSlotBinocular)
            sz = 0.90f;
        else if ((wt & (MaskSlotPrimary | MaskSlotSecondary)) && !laser)
            sz = 1.25f;
    }
}

void DrawLabel(const char *text, float x, float y, float size, PackedColor col, float alpha)
{
    if (!text || !*text)
    {
        return;
    }
    // DrawText needs a real Font (nullptr silently draws nothing). Load a
    // standard bitmap font once; the texture bank keeps it cached.
    static Ref<Font> font = GLOB_ENGINE->LoadFont(GetFontID("tahomaB24"));
    GLOB_ENGINE->DrawText(Point2DFloat(x, y), size, font, ModAlpha(col, alpha), text);
}

// Transparent category artwork can also serve as an empty-slot silhouette.
// Skip a fully opaque streaming placeholder instead of drawing a black block.
bool IconIsSolidBlock(Texture *tex)
{
    if (!tex)
    {
        return true;   // nothing to draw -> treat as "no real icon"
    }
    const float c[4][2] = { {0.03f, 0.03f}, {0.97f, 0.03f},
                            {0.03f, 0.97f}, {0.97f, 0.97f} };
    int opaque = 0;
    for (int i = 0; i < 4; i++)
    {
        if (tex->GetPixel(0, c[i][0], c[i][1]).A() > 0.5f)
        {
            opaque++;
        }
    }
    return opaque >= 4;   // every corner opaque -> not a transparent gear icon
}

// Cache existence across loose original assets and installed addon/PBO icons.
bool IconFileExists(const char *path)
{
    struct Entry { RString path; bool exists; };
    static AutoArray<Entry> cache;
    for (int i = 0; i < cache.Size(); i++)
    {
        if (strcmp((const char *)cache[i].path, path) == 0)
        {
            return cache[i].exists;
        }
    }
    Entry e;
    e.path = RString(path);
    e.exists = QIFStreamB::FileExist(path);
    cache.Add(e);
    return e.exists;
}

// Width of a string in screen-fraction units, for right-aligning labels.
float MeasureText(const char *text, float size)
{
    if (!text || !*text)
    {
        return 0.0f;
    }
    static Ref<Font> font = GLOB_ENGINE->LoadFont(GetFontID("tahomaB24"));
    return GLOB_ENGINE->GetTextWidth(size, font, text);
}

} // namespace

// Incrementally preload inventory gear icons, a small batch per call, from the
// in-game update loop (DisplayMission). Spreading it over frames of normal
// gameplay - rather than doing it all during mission load, which made the first
// editor Preview bounce - loads every icon into the texture bank (and pins it so
// it isn't flushed) before the player ever opens the inventory. Once they're
// resident, opening the arsenal and flipping pages hit the cache with no decode
// or upload, so there's no hitch. Returns true when everything is loaded.
bool PreloadInventoryIconsStep(int budget)
{
    static bool done = false;
    if (done)
    {
        return true;
    }
    // Heap-allocated and intentionally never freed: this pins texture references for
    // the whole session. If it were a normal static, its destructor would run during
    // process exit and release these Ref<Texture> AFTER the texture bank may already
    // be torn down - a static-destruction-order fault that shows up as a 0xC0000005
    // crash on quit (execute_onexit_table in the crash log). Leaking it on purpose
    // sidesteps that entirely; the OS reclaims the memory at exit regardless.
    static AutoArray<Ref<Texture>>& pinned = *(new AutoArray<Ref<Texture>>());
    static int wi = 0, mi = 0, gi = 0;

    auto warm = [&](Texture *tex) {
        if (!tex)
        {
            return;
        }
        tex->SetMaxSize(1024);
        tex->GetPixel(0, 0.5f, 0.5f);   // force the decode now, not on first draw
        // Resolve the stable source rectangle before the first draw.
        float u0, u1, v0, v1;
        IconContentUV(tex, u0, u1, v0, v1);
        pinned.Add(Ref<Texture>(tex));
    };

    int n = 0;
    // Empty slots use the same original category assets as occupied tiles.
    static const char* const kGhosts[] = {
        "assets\\inventory\\m16.paa",
        "assets\\inventory\\rifle.paa", "assets\\inventory\\ak_rifle.paa",
        "assets\\inventory\\scoped_rifle.paa", "assets\\inventory\\smg.paa",
        "assets\\inventory\\machinegun.paa", "assets\\inventory\\launcher.paa",
        "assets\\inventory\\pistol.paa", "assets\\inventory\\binoculars.paa",
        "assets\\inventory\\nvg.paa", "assets\\inventory\\magazine.paa",
        "assets\\inventory\\curved_magazine.paa", "assets\\inventory\\grenade.paa",
        "assets\\inventory\\smoke.paa", "assets\\inventory\\satchel.paa",
        "assets\\inventory\\mine.paa", "assets\\inventory\\rocket.paa",
    };
    for (; gi < (int)(sizeof(kGhosts) / sizeof(kGhosts[0])); )
    {
        const char *path = kGhosts[gi++];
        if (QIFStreamB::FileExist(path))
            warm(GlobLoadTexture(RString(path)));
        if (++n >= budget)
            return false;
    }
    for (; wi < WeaponTypes.Size(); )
    {
        const WeaponType *w = WeaponTypes.Get(wi++);
        if (!w)
            continue;
        InvCell c;
        c.kind = InvItemKind::Weapon;
        c.weapon = w;
        warm(LoadInvItemIcon(c));
        if (++n >= budget)
            return false;
    }
    for (; mi < MagazineTypes.Size(); )
    {
        const MagazineType *m = MagazineTypes.Get(mi++);
        if (!m)
            continue;
        InvCell c;
        c.kind = InvItemKind::Magazine;
        c.magType = m;
        warm(LoadInvItemIcon(c));
        if (++n >= budget)
            return false;
    }
    done = true;
    return true;
}

// ---------------------------------------------------------------------------
// CInventoryPreview
// ---------------------------------------------------------------------------

CInventoryPreview::CInventoryPreview(ControlsContainer *parent, int idc, const ParamEntry &cls)
    : base(parent, idc, cls)
{
}

void CInventoryPreview::SetSubject(Person *player)
{
    _subject = player;
    if (!player)
    {
        return;
    }
    SetShape(player->GetShape());
    // VERIFY: Object::SetShape/GetShape, same note as the previous pass.
}

void CInventoryPreview::OnDraw(float alpha)
{
    base::OnDraw(alpha);
}

// ---------------------------------------------------------------------------
// DisplayInventory
// ---------------------------------------------------------------------------

DisplayInventory::DisplayInventory(ControlsContainer *parent, Person *player)
    : base(parent)
{
    // Keep the world simulating (animations/AI keep running - no frozen
    // player) but disable the in-game UI. World::IsUIEnabled() gates
    // _ui->SimulateHUD, which is where player fire/movement/aim and the action
    // menu are processed - so clearing this blocks player interaction and hides
    // the HUD/action menu while the world keeps ticking behind the panes.
    _enableUI = false;

    _model = new InventoryModel(player);

    if (const ParamEntry *resource = InventoryDisplayResource())
        Load(*resource);
    Rebuild();   // also seeds the 1/2/3 slot hotkeys

    if (_preview)
    {
        _preview->SetSubject(player);
    }
}

DisplayInventory::~DisplayInventory()
{
    // Release the right-mouse-button suppression we hold while open, so RMB drives
    // gameplay binds normally again once the inventory is closed.
    InputSubsystem::Instance().SetSuppressRightButtonActions(false);
}

Control *DisplayInventory::OnCreateCtrl(int type, int idc, const ParamEntry &cls)
{
    return base::OnCreateCtrl(type, idc, cls);
}

ControlObject *DisplayInventory::OnCreateObject(int type, int idc, const ParamEntry &cls)
{
    if (idc == IDC_INVENTORY_PREVIEW)
    {
        _preview = new CInventoryPreview(this, idc, cls);
        return _preview;
    }
    return base::OnCreateObject(type, idc, cls);
}

// --- layout ------------------------------------------------------------

// Header height: measured ~0.128 x the grid width in the mockup (90 px over a
// 704 px grid). At GridW=0.30 and ~16:9 that is ~0.068 screen-fractions.
static constexpr float InvHeaderH = 0.068f;

// Measured loadout-slot proportions (fractions of the strip/grid width) and the
// rifle-slot pixel aspect, straight from the mockup: two rifle rectangles +
// a pistol slot, rifle rectangle ~2.125:1.
static constexpr float LoadRifleFrac  = 0.395f;   //!< each rifle/launcher slot
static constexpr float LoadPistolFrac = 0.210f;   //!< pistol slot
static constexpr float LoadRifleAspect = 2.125f;  //!< rifle slot width:height (px)

float DisplayInventory::LoadoutStripH() const
{
    // Height that makes a rifle slot the measured 2.125:1 rectangle. The slots
    // tile GridW flush with two inter-slot gaps, so the usable width excludes them.
    const float asp = GLOB_ENGINE->Width2D() / (float)GLOB_ENGINE->Height2D();
    const float availW = GridPaneW() - 2.0f * TilePad;
    const float rifleW = LoadRifleFrac * availW;         // x-fraction
    return rifleW * asp / LoadRifleAspect;               // y-fraction
}

float DisplayInventory::ContentGap() const
{
    // Uniform breathing space: header->content and loadout->grid use the same gap.
    const float cellH = CellFrac() * (GLOB_ENGINE->Width2D() / (float)GLOB_ENGINE->Height2D());
    return 0.175f * cellH;
}

float DisplayInventory::StorageTopExtra() const
{
    // header -> (gap) -> loadout strip -> (gap) -> storage grid.
    return ContentGap() + LoadoutStripH() + ContentGap();
}

// --- Effective (edit-menu-scaled) panel geometry ---------------------------
int   DisplayInventory::Cols() const       { return InventoryGridCols(); }
float DisplayInventory::VicPaneX() const    { return GroundX; }
float DisplayInventory::VicPaneW() const    { return GroundW * InventoryVicWinScaleW(); }
float DisplayInventory::VicPaneHt() const   { return VicH * InventoryVicWinScaleH(); }
float DisplayInventory::GridPaneW() const   { return GridW * InventoryInvWinScaleW(); }
// Inventory keeps its right edge fixed (GridX+GridW) and grows leftward, so
// enlarging it never pushes the panel off the right of the screen.
float DisplayInventory::GridPaneX() const   { return (GridX + GridW) - GridPaneW(); }
float DisplayInventory::GridPaneHt() const  { return PaneH * InventoryInvWinScaleH(); }
float DisplayInventory::CellFrac() const    { return GridPaneW() / (float)Cols(); }

// Default vicinity width for a cell (before any per-class tuning override).
static int DefaultVicSpan(const InvCell &cell)
{
    if (cell.kind == InvItemKind::Weapon)
    {
        const WeaponType *w = cell.weapon;
        return (w && (w->_weaponType & (MaskSlotHandGun | MaskSlotBinocular))) ? 1 : 2;
    }
    char buf[160];
    int j = 0;
    for (const char *p = (const char *)cell.ClassName(); p && *p && j < 78; ++p)
    { char c = *p; if (c >= 'A' && c <= 'Z') c += 32; buf[j++] = c; }
    buf[j++] = ' ';
    for (const char *p = (const char *)cell.PictureName(); p && *p && j < 158; ++p)
    { char c = *p; if (c >= 'A' && c <= 'Z') c += 32; buf[j++] = c; }
    buf[j] = 0;
    if (strstr(buf, "9k32") || strstr(buf, "strela")) return 3;
    if (strstr(buf, "carlgustav") || strstr(buf, "aalaunch") || strstr(buf, "law") ||
        strstr(buf, "rpg") || strstr(buf, "at4") || strstr(buf, "mortar")) return 2;
    return 1;
}

// Default grid footprint for a cell (before any per-class tuning override).
static GridSize DefaultGridFootprint(const InvCell &cell)
{
    if (cell.kind == InvItemKind::Weapon)
        return ItemFootprint::Of(cell.weapon);
    const MagazineType *mt = cell.magType ? cell.magType
                                          : (cell.magazine ? cell.magazine->_type : nullptr);
    return mt ? ItemFootprint::Of(mt) : GridSize{1, 1};
}

void DisplayInventory::LayoutGroundPane()
{
    _groundTiles.Clear();
    _vicPageLabels.Clear();
    _vicPageSource.Clear();

    const float asp = GLOB_ENGINE->Width2D() / (float)GLOB_ENGINE->Height2D();
    const float gap = 0.0015f;   // tight gap so occupied tiles nearly touch
    const float top = PaneY + InvHeaderH + ContentGap();
    const float viewH = VicPaneHt() - InvHeaderH - ContentGap();
    _vicViewH = viewH;

    // Unified 6-column flow: items are placed in MODEL order (each weapon then the
    // magazine(s) that go in it - see InventoryModel::AddArsenal), a weapon spanning
    // two columns and a magazine one. This keeps a gun and its mag side by side,
    // instead of the old "all guns, then all mags" split which separated them.
    const int   cols  = 6;
    const float cellW = (VicPaneW() - (cols - 1) * gap) / cols;
    const float cellH = cellW * asp;              // visually square cell
    const float rowH  = cellH + gap;

    const AutoArray<InvCell> &cells = _model->Vicinity();
    const AutoArray<InventoryModel::VicSource> &sources = _model->VicinitySources();

    struct PT { int cellIdx; int page; float x; float y; float w; float h; };
    AutoArray<PT> placed;

    // One page per vicinity SOURCE (container/body/arsenal/ground) - including
    // EMPTY ones, so a bare car shows its name and a Ground page is always offered.
    int page = -1;
    for (int s = 0; s < sources.Size(); s++)
    {
        const InventoryModel::VicSource &src = sources[s];
        AutoArray<int> idx;
        // Jerry cans are grouped to one end of each page (model order preserved within
        // each group): FIRST on a vehicle/container page - a freshly spawned vehicle
        // surfaces its fuel cans up top - but LAST on the Arsenal page, where the
        // infinite gear list reads better with the cans at the bottom.
        bool srcIsArsenal = false;
        AutoArray<int> jerry, rest;
        for (int i = 0; i < cells.Size(); i++)
        {
            if (!_model->CellOnSource(cells[i], s)) continue;
            if (cells[i].fromArsenal) srcIsArsenal = true;
            const char *cn = (const char *)cells[i].ClassName();
            if (cn && strcmp(cn, "JerryCan") == 0) jerry.Add(i);
            else                                   rest.Add(i);
        }
        if (srcIsArsenal)
        {
            for (int a = 0; a < rest.Size(); a++)  idx.Add(rest[a]);
            for (int a = 0; a < jerry.Size(); a++) idx.Add(jerry[a]);
        }
        else
        {
            for (int a = 0; a < jerry.Size(); a++) idx.Add(jerry[a]);
            for (int a = 0; a < rest.Size(); a++)  idx.Add(rest[a]);
        }
        // This source starts a fresh page (empty source -> label-only page).
        page++; _vicPageLabels.Add(src.label); _vicPageSource.Add(s);
        float yAcc = 0.0f;
        int col = 0;
        int bandRows = 1;   // row-height (in cells) of the band currently being filled
        for (int a = 0; a < idx.Size(); a++)
        {
            const int ci = idx[a];
            // Per-item vicinity width (default), with any per-class tuning override.
            int span = DefaultVicSpan(cells[ci]);
            if (const IconTune *t = FindIconTune((const char *)IconTuneKey(cells[ci])))
                if (t->vw > 0) span = t->vw;
            // Jerry can: matches its inventory footprint - 2 wide x 3 tall.
            const char *cn = (const char *)cells[ci].ClassName();
            int rows = 1;
            if (cn && strcmp(cn, "JerryCan") == 0) { span = 2; rows = 3; }

            // Wrap to a new band when the item won't fit in the remaining columns, or
            // when its height differs from the band we're filling. Multi-row items of
            // the same height (e.g. two jerry cans) therefore pack side by side within
            // one band instead of each taking its own row.
            if (col != 0 && (col + span > cols || rows != bandRows))
            {
                col = 0; yAcc += bandRows * rowH;
            }
            if (col == 0) bandRows = rows;

            const float tileH = rows * cellH + (rows - 1) * gap;

            // Page break if this band would run past the bottom of the view.
            if (yAcc > 0.0f && yAcc + tileH > viewH)
            {
                page++; _vicPageLabels.Add(src.label); _vicPageSource.Add(s);
                yAcc = 0.0f; col = 0; bandRows = rows;
            }

            PT p;
            p.cellIdx = ci;
            p.page = page;
            p.x = VicPaneX() + col * (cellW + gap);
            p.y = top + yAcc;
            p.w = span * cellW + (span - 1) * gap;
            p.h = tileH;
            placed.Add(p);
            col += span;   // next slot in the same band; top-of-loop wraps when full
        }
    }

    _vicPageCount = _vicPageLabels.Size() > 0 ? _vicPageLabels.Size() : 1;
    if (_vicPage < 0) _vicPage = 0;
    if (_vicPage >= _vicPageCount) _vicPage = _vicPageCount - 1;

    for (int i = 0; i < placed.Size(); i++)
    {
        if (placed[i].page != _vicPage) continue;
        InvTile tile;
        tile.cell = cells[placed[i].cellIdx];
        tile.x = placed[i].x;
        tile.y = placed[i].y;
        tile.w = placed[i].w;
        tile.h = placed[i].h;
        tile.icon = LoadInvItemIcon(tile.cell);   // resolve once, not per frame
        {   // Validate the resolved loose/PBO icon once at layout.
            RString ip = InvItemPicturePath(tile.cell);
            tile.iconBlock = !tile.icon;
        }
        _groundTiles.Add(tile);
    }
}

void DisplayInventory::LayoutGridPane()
{
    _gridTiles.Clear();

    // FLUSH grid: the outer tile edges sit exactly on GridX and GridX+GridW (same
    // as the header/weight bar), with the gap only BETWEEN tiles - no outer inset.
    const float asp = GLOB_ENGINE->Width2D() / (float)GLOB_ENGINE->Height2D();
    const int   cols = Cols();
    const float gap = TilePad;
    const float tileW = (GridPaneW() - (cols - 1) * gap) / cols;
    const float tileH = tileW * asp;
    const float pitchX = tileW + gap;
    const float pitchY = tileH + gap;

    const float storageTop = PaneY + InvHeaderH + StorageTopExtra();

    // Paginate by rows: how many tile rows fit between storageTop and the bottom of the
    // pane, leaving a strip for the pager arrows.
    const float pagerH = 0.030f;
    const float usableH = (PaneY + GridPaneHt()) - storageTop - pagerH;
    _gridRowsPerPage = (pitchY > 0.0f) ? (int)(usableH / pitchY) : 1;
    if (_gridRowsPerPage < 1) _gridRowsPerPage = 1;

    const AutoArray<InvCell> &cells = _model->Grid();

    // Total rows the items occupy (bottom edge of the lowest item).
    int totalRows = 0;
    for (int i = 0; i < cells.Size(); i++)
    {
        const int bottom = cells[i].gy + cells[i].footprint.h;
        if (bottom > totalRows) totalRows = bottom;
    }
    _gridPageCount = (totalRows + _gridRowsPerPage - 1) / _gridRowsPerPage;
    if (_gridPageCount < 1) _gridPageCount = 1;
    if (_gridPage < 0) _gridPage = 0;
    if (_gridPage >= _gridPageCount) _gridPage = _gridPageCount - 1;

    const int pageStartRow = _gridPage * _gridRowsPerPage;

    for (int i = 0; i < cells.Size(); i++)
    {
        const InvCell &c = cells[i];
        // An item belongs to the page that contains its top-left row.
        if (c.gy < pageStartRow || c.gy >= pageStartRow + _gridRowsPerPage)
            continue;

        InvTile tile;
        tile.cell = c;
        tile.x = GridPaneX() + c.gx * pitchX;
        tile.y = storageTop + (c.gy - pageStartRow) * pitchY;
        tile.w = c.footprint.w * tileW + (c.footprint.w - 1) * gap;
        tile.h = c.footprint.h * tileH + (c.footprint.h - 1) * gap;
        tile.icon = LoadInvItemIcon(tile.cell);   // resolve once, not per frame
        {   // Validate the resolved loose/PBO icon once at layout.
            RString ip = InvItemPicturePath(tile.cell);
            tile.iconBlock = !tile.icon;
        }
        _gridTiles.Add(tile);
    }
}

void DisplayInventory::LayoutLoadoutStrip()
{
    _loadoutTiles.Clear();

    const float slotH = LoadoutStripH();
    const float top = PaneY + InvHeaderH + ContentGap();

    // Slot widths straight from the mockup ratios: two rifle/launcher rectangles
    // then a narrower pistol slot, together spanning the grid width. Order matches
    // ScanLoadout: primary, secondary, handgun.
    const float slotFrac[3] = { LoadRifleFrac, LoadRifleFrac, LoadPistolFrac };
    const float gap = TilePad;
    const float availW = GridPaneW() - 2.0f * gap;   // three slots, two gaps

    const AutoArray<LoadoutSlot> &slots = _model->Loadout();
    float x = GridPaneX();   // flush with the header/grid left edge
    for (int i = 0; i < slots.Size() && i < 3; i++)
    {
        const LoadoutSlot &s = slots[i];
        const float slotW = slotFrac[i] * availW;

        InvTile tile;
        tile.cell = s.cell;
        tile.x = x;
        tile.y = top;
        tile.w = slotW;
        tile.h = slotH;
        tile.loadoutSlot = true;
        tile.loadoutEmpty = !s.hasItem;
        tile.loadoutPistol = (i == 2);   // third slot is the pistol slot
        tile.loadoutKind = i;            // 0=rifle 1=launcher 2=pistol -> per-side ghost
        tile.loadoutCount = s.count;
        tile.loadoutSpare = s.spare;
        _loadoutTiles.Add(tile);

        x += slotW + gap;
    }
}

void DisplayInventory::Rebuild()
{
    LayoutLoadoutStrip();
    LayoutGroundPane();
    LayoutGridPane();
    _hovered = nullptr;

    // Keep the 1/2/3 slot bindings in sync with the current loadout so a weapon just
    // moved into a slot gets its number badge on this very rebuild (not next open).
    SeedDefaultHotkeys();


}

const InvTile *DisplayInventory::TileAt(float x, float y) const
{
    // Loadout slots first: they sit above the storage grid and their tiles are
    // the only interactive things in that band.
    for (int i = 0; i < _loadoutTiles.Size(); i++)
    {
        if (!_loadoutTiles[i].loadoutEmpty && _loadoutTiles[i].Contains(x, y))
            return &_loadoutTiles[i];
    }
    for (int i = 0; i < _groundTiles.Size(); i++)
    {
        if (_groundTiles[i].Contains(x, y)) return &_groundTiles[i];
    }
    for (int i = 0; i < _gridTiles.Size(); i++)
    {
        if (_gridTiles[i].Contains(x, y)) return &_gridTiles[i];
    }
    return nullptr;
}

bool DisplayInventory::PaneAt(float x, float y, InvPane &out) const
{
    // Vicinity (left) is shorter than the Inventory (right).
    if (x >= VicPaneX() && x <= VicPaneX() + VicPaneW() && y >= PaneY && y <= PaneY + VicPaneHt())
    {
        out = InvPane::Ground;
        return true;
    }
    if (x >= GridPaneX() && x <= GridPaneX() + GridPaneW() && y >= PaneY && y <= PaneY + GridPaneHt())
    {
        out = InvPane::Grid;
        return true;
    }
    return false;
}

// --- input ---------------------------------------------------------------

void DisplayInventory::OnLButtonDown(float x, float y)
{
    _cursorX = x;
    _cursorY = y;

    // Storage grid pager arrows.
    for (int k = 0; k < 2; k++)
    {
        float ax, ay, aw, ah;
        if (GridPageArrowRect(k, ax, ay, aw, ah) &&
            x >= ax && x <= ax + aw && y >= ay && y <= ay + ah)
        {
            if (k == 0 && _gridPage > 0)                    { _gridPage--; Rebuild(); }
            else if (k == 1 && _gridPage < _gridPageCount - 1) { _gridPage++; Rebuild(); }
            return;
        }
    }

    // Interacting with the edit-menu panel? Grab a slider, or click a toggle/button.
    if (InventoryEditMenuVisible())
    {
        for (int s = 0; s < EditSliderCount(); s++)
        {
            float sx, sy, sw, sh;
            EditSliderRect(s, sx, sy, sw, sh);
            if (x >= sx && x <= sx + sw && y >= sy - 0.006f && y <= sy + sh + 0.006f)
            {
                _activeSlider = s;
                float f = (sw > 0.0f) ? (x - sx) / sw : 0.0f;
                f = (f < 0.0f) ? 0.0f : (f > 1.0f ? 1.0f : f);
                EditSliderSet(s, f);
                Rebuild();
                return;
            }
        }
        if (EditMenuClick(x, y))
            return;
    }

    const InvTile *tile = TileAt(x, y);
    if (!tile)
    {
        return;
    }

    _dragArmed = true;
    _dragging = false;
    _dragCell = tile->cell;
    _dragStartX = x;
    _dragStartY = y;
}

void DisplayInventory::OnMouseMove(float x, float y)
{
    _cursorX = x;
    _cursorY = y;

    if (_dragArmed && !_dragging)
    {
        const float dx = x - _dragStartX;
        const float dy = y - _dragStartY;
        if (dx * dx + dy * dy > DragThreshold * DragThreshold)
        {
            _dragging = true;
        }
    }

    if (!_dragging)
    {
        const InvTile *tile = TileAt(x, y);
        if (tile != _hovered)
        {
            _hovered = tile;
            _hoverTime = 0.0f;
        }
    }
}

void DisplayInventory::OnLButtonUp(float x, float y)
{
    if (!_dragArmed)
    {
        return;
    }

    const bool wasDragging = _dragging;
    _dragArmed = false;
    _dragging = false;

    // Index of the vicinity source the player is currently looking at, so a
    // grid->vicinity drop goes into THAT page (the car, or Ground) - not just any
    // nearby container.
    const int curSrc = (_vicPage >= 0 && _vicPage < _vicPageSource.Size())
                           ? _vicPageSource[_vicPage]
                           : -1;

    if (!wasDragging)
    {
        // Click: grid item -> drop onto the current vicinity page; vicinity item
        // -> pick up into the grid.
        if (_dragCell.pane == InvPane::Grid)
        {
            _model->DropToSource(_dragCell, curSrc);
        }
        else
        {
            _model->Transfer(_dragCell, InvPane::Grid);
        }
        Rebuild();
        return;
    }

    // Drop a carried WEAPON onto a loadout slot -> equip it (make it the active weapon).
    // This is the "change weapon by drag-and-drop" that was missing; previously equipping
    // required dropping the gun and picking it back up.
    if (_dragCell.pane == InvPane::Grid && _dragCell.kind == InvItemKind::Weapon && _dragCell.weapon)
    {
        const InvTile *drop = TileAt(x, y);
        if (drop && drop->loadoutSlot)
        {
            _model->EquipWeapon(_dragCell);
            Rebuild();
            return;
        }
    }

    // Within-grid rearrangement is intentionally not persisted in v1: ScanGrid
    // re-derives tile positions from scratch on every Refresh, so a manual
    // arrangement wouldn't survive anyway (see SANITY_CHECK.md items 7-8).
    // So every drag is either a cross-pane transfer or a drop-to-ground.
    InvPane targetPane;
    if (PaneAt(x, y, targetPane) && targetPane != _dragCell.pane)
    {
        if (_dragCell.pane == InvPane::Grid && targetPane == InvPane::Ground)
        {
            // Grid -> vicinity: honour the current page (car vs Ground).
            _model->DropToSource(_dragCell, curSrc);
        }
        else
        {
            _model->Transfer(_dragCell, targetPane);
        }
    }
    else if (!PaneAt(x, y, targetPane))
    {
        // Released outside both panes -> drop onto the current vicinity page.
        if (_dragCell.pane == InvPane::Grid)
        {
            _model->DropToSource(_dragCell, curSrc);
        }
    }
    // Released inside its own pane -> no-op (or a within-grid move we don't
    // persist in v1).

    Rebuild();
}

bool DisplayInventory::OnKeyDown(unsigned nChar, unsigned nRepCnt, unsigned nFlags)
{
    if (nChar == SDL_SCANCODE_ESCAPE)
    {
        Exit(IDC_CANCEL);
        return true;
    }
    return base::OnKeyDown(nChar, nRepCnt, nFlags);
}

void DisplayInventory::OnSimulate(EntityAI *vehicle)
{
    base::OnSimulate(vehicle);
    const auto now = Glob.uiTime;
    const float dt = _uiClockStarted ? std::max(0.0f, now - _lastUiTime) : 0.0f;
    _lastUiTime = now;
    _uiClockStarted = true;

    // The panes aren't engine controls, so the container's control-routing
    // never delivers pointer events to them. Drive our own handlers from the
    // raw cursor + left button, using the same [-1,1]->[0,1] mapping
    // ControlsContainer::OnSimulate uses, so hit-testing lines up with drawing.
    auto &input = InputSubsystem::Instance();

    // While the inventory is open, the right mouse button is ours: keep it from
    // also triggering any gameplay keybind (cleared when the display closes).
    input.SetSuppressRightButtonActions(true);

    // Close on TAB (the same key that opened it in DisplayMission). GetKeyToDo is
    // edge-triggered and consumed, and only the topmost display polls it per
    // frame, so one press can't both close here and re-open next frame.
    if (input.GetActionToDo(UAInventory))   // the same bound key that opened it
    {
        Exit(IDC_CANCEL);
        return;
    }

    // Toggle the edit menu while the inventory is open (the mission loop's poll is
    // gated on the inventory being closed, so it wouldn't fire in here).
    if (input.GetActionToDo(UAInventoryEditToggle))
    {
        SetInventoryEditMenuVisible(!InventoryEditMenuVisible());
    }

    const float mx = 0.5f + input.GetCursorX() * 0.5f;
    const float my = 0.5f + input.GetCursorY() * 0.5f;

    // Right-click a weapon tile to arm hotkey binding, then press a number key
    // (1-9,0) to bind that weapon to the slot. Pressing that number in-game later
    // switches the player to the weapon (see DisplayMission update).
    const bool rDown = input.IsMouseRightDown();
    if (rDown && !_lastRDown)
    {
        const InvTile *t = TileAt(mx, my);
        if (InventoryEditMenuVisible())
        {
            // Edit mode: right-click selects this item's class for icon tuning, and
            // seeds its footprint fields from the current defaults so adjustments
            // start from the real values.
            _editIcon = (t && t->cell.ClassName().GetLength() > 0)
                            ? IconTuneKey(t->cell)
                            : RString();
            if (t)   // edit the size/nudge of whichever context was clicked
                _editScope = t->loadoutSlot ? TuneScope::Loadout
                             : (t->cell.pane == InvPane::Ground ? TuneScope::Vicinity
                                                                : TuneScope::Inventory);
            if (_editIcon.GetLength() > 0)
            {
                IconTune &tn = EditIconTune((const char *)_editIcon);
                if (tn.gw == 0) { GridSize g = DefaultGridFootprint(t->cell); tn.gw = g.w; tn.gh = g.h; }
                if (tn.vw == 0) tn.vw = DefaultVicSpan(t->cell);
                SaveIconTuning();
            }
        }
        else if (t && t->loadoutSlot && t->cell.kind == InvItemKind::Weapon)
        {
            // A rifle/launcher/pistol loadout slot: bind the KEY TO THE SLOT, not the
            // specific weapon, so it follows the slot after a weapon swap.
            char sk[12];
            snprintf(sk, sizeof(sk), "slot:%d", t->loadoutKind);
            _bindPending = RString(sk);
            _bindPendingLabel = t->cell.DisplayName();
            _bindPendingInst = nullptr;
            _bindArmedLoadout = true;
            _bindArmedKind = t->loadoutKind;
            _bindArmedPane = t->cell.pane;
        }
        else if (t && ((t->cell.kind == InvItemKind::Weapon && t->cell.weapon) ||
                       _model->IsThrowableItem(t->cell)))
        {
            // Bind carried weapons and throwables by their kind-qualified key.
            _bindPending = IconTuneKey(t->cell);
            _bindPendingLabel = t->cell.DisplayName();
            _bindPendingInst = (t->cell.kind == InvItemKind::Weapon)
                                   ? (const void *)t->cell.weapon
                                   : (const void *)t->cell.magazine.GetRef();
            // Guarantee the confirm toast differentiates the Put items - the AP mine
            // (MineE) and AT mine (Mine) both carry the config display name "Mine".
            {
                const char *cn = (const char *)t->cell.ClassName();
                if (strcmp(cn, "MineE") == 0)         _bindPendingLabel = RString("AP Mine");
                else if (strcmp(cn, "Mine") == 0)     _bindPendingLabel = RString("AT Mine");
                else if (strcmp(cn, "PipeBomb") == 0) _bindPendingLabel = RString("Satchel Charge");
                else if (strcmp(cn, "TimeBomb") == 0) _bindPendingLabel = RString("Timed Satchel");
            }
            _bindArmedLoadout = false;
            _bindArmedKind = -1;
            _bindArmedPane = t->cell.pane;
        }
        else
        {
            _bindPending = RString();
            _bindPendingLabel = RString();
            _bindPendingInst = nullptr;
            _bindArmedKind = -1;
        }
    }
    _lastRDown = rDown;

    if (_bindPending.GetLength() > 0)
    {
        static const SDL_Scancode kNumKeys[10] = {
            SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5,
            SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0 };
        for (int s = 0; s < 10; s++)
        {
            if (input.GetKeyToDo(kNumKeys[s]))
            {
                SetWeaponHotkey(s, _bindPending, _bindPendingInst);
                // Confirm in the top-left corner: "1  M16A2" (key 0 shown for slot 10).
                const int num = (s + 1) % 10;
                char toast[96];
                snprintf(toast, sizeof(toast), "%d   %s", num,
                         _bindPendingLabel.GetLength() > 0 ? (const char *)_bindPendingLabel
                                                           : (const char *)_bindPending);
                _hotkeyToast = RString(toast);
                _hotkeyToastTime = 2.5f;
                _bindPending = RString();
                _bindPendingLabel = RString();
                _bindPendingInst = nullptr;
                _bindArmedKind = -1;
                break;
            }
        }
    }

    // Vicinity paging: Q = previous page, E = next (edge-triggered/consumed).
    if (input.GetKeyToDo(SDL_SCANCODE_Q) && _vicPage > 0)
    {
        _vicPage--;
        Rebuild();
    }
    if (input.GetKeyToDo(SDL_SCANCODE_E) && _vicPage < _vicPageCount - 1)
    {
        _vicPage++;
        Rebuild();
    }

    // Storage grid paging via the scroll wheel: up = previous page, down = next.
    const float wheel = input.GetMouseWheel();
    if (wheel > 0.0f && _gridPage > 0)
    {
        _gridPage--;
        Rebuild();
    }
    else if (wheel < 0.0f && _gridPage < _gridPageCount - 1)
    {
        _gridPage++;
        Rebuild();
    }

    // Icon tuning keys (edit menu + a right-clicked item). Arrow keys nudge the
    // image within its tile; - / + resize it; [ ] set grid width, , . grid height,
    // ; ' the vicinity width. Footprint changes re-layout; all changes are saved.
    if (InventoryEditMenuVisible() && _editIcon.GetLength() > 0)
    {
        IconTune &tn = EditIconTune((const char *)_editIcon);
        // Size/nudge act on whichever context was clicked (equipped / inventory /
        // vicinity); footprint keys stay per-pane (grid gw/gh vs vicinity vw).
        float &sz = (_editScope == TuneScope::Loadout)  ? tn.lsize
                  : (_editScope == TuneScope::Vicinity) ? tn.vsize : tn.size;
        float &ox = (_editScope == TuneScope::Loadout)  ? tn.lox
                  : (_editScope == TuneScope::Vicinity) ? tn.vox : tn.ox;
        float &oy = (_editScope == TuneScope::Loadout)  ? tn.loy
                  : (_editScope == TuneScope::Vicinity) ? tn.voy : tn.oy;
        bool changed = false, relayout = false;
        const float step = 0.01f;
        if (input.GetKeyToDo(SDL_SCANCODE_LEFT))  { ox -= step; changed = true; }
        if (input.GetKeyToDo(SDL_SCANCODE_RIGHT)) { ox += step; changed = true; }
        if (input.GetKeyToDo(SDL_SCANCODE_UP))    { oy -= step; changed = true; }
        if (input.GetKeyToDo(SDL_SCANCODE_DOWN))  { oy += step; changed = true; }
        if (input.GetKeyToDo(SDL_SCANCODE_MINUS))
        { sz -= 0.02f; if (sz < 0.2f) sz = 0.2f; changed = true; }
        if (input.GetKeyToDo(SDL_SCANCODE_EQUALS))
        { sz += 0.02f; if (sz > 2.0f) sz = 2.0f; changed = true; }
        if (input.GetKeyToDo(SDL_SCANCODE_LEFTBRACKET))
        { if (tn.gw > 1)  tn.gw--; changed = relayout = true; }
        if (input.GetKeyToDo(SDL_SCANCODE_RIGHTBRACKET))
        { if (tn.gw < 12) tn.gw++; changed = relayout = true; }
        if (input.GetKeyToDo(SDL_SCANCODE_COMMA))
        { if (tn.gh > 1) tn.gh--; changed = relayout = true; }
        if (input.GetKeyToDo(SDL_SCANCODE_PERIOD))
        { if (tn.gh < 6) tn.gh++; changed = relayout = true; }
        if (input.GetKeyToDo(SDL_SCANCODE_SEMICOLON))
        { if (tn.vw > 1)  tn.vw--; changed = relayout = true; }
        if (input.GetKeyToDo(SDL_SCANCODE_APOSTROPHE))
        { if (tn.vw < 12) tn.vw++; changed = relayout = true; }
        if (changed)  SaveIconTuning();
        if (relayout) Rebuild();
    }

    // Drag an edit-menu slider (any of them go through EditSliderSet).
    if (_activeSlider >= 0)
    {
        if (!input.IsMouseLeftDown())
        {
            _activeSlider = -1;
            if (_uiSettingsDirty) { SaveInventorySettings(); _uiSettingsDirty = false; }
        }
        else
        {
            float sx, sy, sw, sh;
            EditSliderRect(_activeSlider, sx, sy, sw, sh);
            if (sw > 0.0f)
            {
                float f = (mx - sx) / sw;
                f = (f < 0.0f) ? 0.0f : (f > 1.0f ? 1.0f : f);
                EditSliderSet(_activeSlider, f);
                Rebuild();
            }
        }
    }

    if (mx != _cursorX || my != _cursorY)
    {
        OnMouseMove(mx, my);
    }

    const bool lDown = input.IsMouseLeftDown();
    if (lDown && !_lastLDown)
    {
        OnLButtonDown(mx, my);
    }
    else if (!lDown && _lastLDown)
    {
        OnLButtonUp(mx, my);
    }
    _lastLDown = lDown;

    if (_hovered && !_dragging)
    {
        _hoverTime += dt;
    }
    if (_hotkeyToastTime > 0.0f)
    {
        _hotkeyToastTime = std::max(0.0f, _hotkeyToastTime - dt);
    }

    // Don't resync mid-gesture - Refresh()+Rebuild() rebuilds the tile arrays,
    // which invalidates _hovered and would yank a tile out from under an
    // active drag.
    if (!_dragArmed && !_dragging)
    {
        _resyncTime += dt;
        if (_resyncTime > 1.5f)
        {
            _resyncTime = 0.0f;
            _model->Refresh();
            Rebuild();
        }
    }

    // Robust hover: recompute from the live cursor every frame (after any
    // Rebuild above, which nulls _hovered). Compare by ITEM identity, not tile
    // pointer - the resync reallocates the tile arrays, so the pointer changes
    // every 0.5s even when the same item is still under the cursor. Resetting
    // _hoverTime on that is what made the tooltip flicker on and off.
    if (!_dragging)
    {
        const InvTile *h = TileAt(mx, my);
        const void *item = nullptr;
        if (h)
        {
            item = (h->cell.kind == InvItemKind::Weapon) ? (const void *)h->cell.weapon
                                                         : (const void *)h->cell.magazine.GetRef();
        }
        _hovered = h;
        if (item != _hoverItem)
        {
            _hoverItem = item;
            _hoverTime = 0.0f;
        }
    }
    else
    {
        _hovered = nullptr;
        _hoverItem = nullptr;
    }
}

// --- drawing ---------------------------------------------------------------

bool DisplayInventory::IsBindArmed(const InvCell &cell) const
{
    if (_bindPending.GetLength() == 0)
        return false;
    return strcmp((const char *)IconTuneKey(cell), (const char *)_bindPending) == 0;
}

bool DisplayInventory::BindFlash(const InvCell &cell, bool isLoadoutTile, int loadoutKind) const
{
    if (_bindPending.GetLength() == 0)
        return false;
    // Slot binding (a rifle/launcher/pistol loadout slot): flash the loadout tile of
    // the armed kind, regardless of which weapon currently sits in it.
    if (strncmp((const char *)_bindPending, "slot:", 5) == 0)
        return isLoadoutTile && loadoutKind == _bindArmedKind;

    if (!IsBindArmed(cell) || isLoadoutTile != _bindArmedLoadout)
        return false;
    // Non-loadout tiles must also be in the armed pane, so an equipped weapon's
    // vicinity copy doesn't flash alongside the clicked tile.
    if (!isLoadoutTile && cell.pane != _bindArmedPane)
        return false;
    // Throwables share a class, so only the exact clicked magazine flashes.
    if (cell.kind == InvItemKind::Magazine)
        return (const void *)cell.magazine.GetRef() == _bindPendingInst;
    return true;
}

void DisplayInventory::DrawHotkeyBadge(const InvTile &tile, float alpha) const
{
    int s;
    if (tile.loadoutSlot)
    {
        // Loadout slots badge their SLOT binding ("slot:0/1/2").
        char sk[12];
        snprintf(sk, sizeof(sk), "slot:%d", tile.loadoutKind);
        s = WeaponHotkeyBadgeSlot(sk, nullptr);
    }
    else
    {
        const void *inst = (tile.cell.kind == InvItemKind::Weapon)
                               ? (const void *)tile.cell.weapon
                               : (const void *)tile.cell.magazine.GetRef();
        s = WeaponHotkeyBadgeSlot((const char *)IconTuneKey(tile.cell), inst);
    }
    if (s < 0)
        return;
    char num[4];
    snprintf(num, sizeof(num), "%d", (s + 1) % 10);   // slot 10 shows as 0

    // Plain small number in the tile's top-right corner - no chip or border.
    const float sz = TextSize * 0.7f;
    const float tw = MeasureText(num, sz);
    const float pad = TilePad * 2.0f;
    DrawLabel(num, tile.x + tile.w - tw - pad, tile.y + pad, sz, ColText, alpha);
}

void DisplayInventory::SeedDefaultHotkeys()
{
    // Runs every Rebuild (idempotent): bind keys 1/2/3 to the rifle / launcher /
    // pistol SLOTS (not the specific weapons) so the key follows the slot after a
    // weapon swap, AND so newly-filled slots get their number badge instantly rather
    // than only after the inventory is reopened. Only fills a key the player hasn't
    // already bound, and only for a slot they actually have a weapon in.
    const AutoArray<LoadoutSlot> &slots = _model->Loadout();
    for (int i = 0; i < slots.Size(); i++)
    {
        const LoadoutSlot &sl = slots[i];
        if (!sl.hasItem || sl.cell.kind != InvItemKind::Weapon || !sl.cell.weapon)
            continue;
        int key = -1;
        if (sl.kind == LoadoutSlot::Primary)        key = 0;   // rifle    -> 1
        else if (sl.kind == LoadoutSlot::Secondary) key = 1;   // launcher -> 2
        else if (sl.kind == LoadoutSlot::Handgun)   key = 2;   // pistol   -> 3
        if (key < 0 || GetWeaponHotkey(key).GetLength() > 0)
            continue;
        char sk[12];
        snprintf(sk, sizeof(sk), "slot:%d", key);
        SetWeaponHotkey(key, RString(sk), nullptr);
    }
}

void DisplayInventory::DrawTile(const InvTile &tile, float alpha, bool highlighted)
{
    // Tile darkness from the live debug opacity: vicinity tiles use it directly,
    // filled storage tiles twice as opaque (clamped). Scroll-edge fade multiplies
    // the whole tile (background, icon and text) so items dissolve at the edges.
    const float a = alpha * tile.fade;
    // An occupied tile (item present) uses the occupied-opacity slider value.
    PackedColor bg = PackedColor(Color(0.0f, 0.0f, 0.0f, _occupiedOpacity));
    PackedColor border = AccentBorderCol(ColTileBorder);

    // While dragging across panes, tint each destination tile by legality.
    if (_dragging && tile.cell.pane != _dragCell.pane)
    {
        const InvTransferResult res = _model->CanTransfer(_dragCell, tile.cell.pane);
        bg = (res == InvTransferResult::Ok) ? ColOkTint : ColBadTint;
    }
    else if (highlighted)
    {
        bg = ColTileHover;
        border = AccentColor();
    }

    FillRect(tile.x, tile.y, tile.w, tile.h, bg, a);

    // A readable label replaces missing artwork. Availability is checked once
    // at layout, without any per-frame texture scan.
    if (tile.iconBlock)
    {
        DrawLabel(tile.cell.DisplayName(), tile.x + TilePad, tile.y + TilePad,
                  TextSize * 0.65f, ColText, a);
    }
    else
    {
        float tsz, tox, toy;
        CellTune(tile.cell, tile.cell.pane == InvPane::Ground ? TuneScope::Vicinity
                                                              : TuneScope::Inventory,
                 tsz, tox, toy);
        DrawIcon(tile.icon, tile.x + TilePad, tile.y + TilePad,
                 tile.w - 2 * TilePad, tile.h - 2 * TilePad, a,
                 tsz * InventoryGlobalIconScale(), tox, toy);
    }

    // Round count for real magazines only (capacity > 1). First-aid kits (uses) and the
    // jerry can (litres, shown as a bar below) carry a capacity too, but aren't badged.
    int ammo = 0, capacity = 0;
    const char *tileCn = (const char *)tile.cell.ClassName();
    const bool isJerryCan = tileCn && strcmp(tileCn, "JerryCan") == 0;
    if (tile.cell.GetAmmo(ammo, capacity) && capacity > 1 &&
        !isJerryCan)
    {
        char cnt[16];
        snprintf(cnt, sizeof(cnt), "%d", ammo);
        DrawLabel(cnt, tile.x + TilePad * 1.5f, tile.y + TilePad, TextSize * 0.7f, ColText, a);
    }

    // Jerry can: a green fuel-level bar across the bottom of the tile (litres / 20).
    if (isJerryCan)
    {
        int fa = 0, fcap = 0;
        if (tile.cell.GetAmmo(fa, fcap) && fcap > 0)
        {
            const float frac = (float)fa / (float)fcap;
            // Match the mock-up: a thin bar, inset from both sides, sitting just above
            // the bottom edge of the tile.
            const float bh   = 0.007f;               // slightly thinner still
            const float side = tile.w * 0.12f;       // margin on each side
            const float bx   = tile.x + side;
            const float bw   = tile.w - 2.0f * side;
            const float by   = tile.y + tile.h - TilePad - bh - 0.010f;   // nudged down
            FillRect(bx, by, bw, bh, ColAmmoBarBg, a);
            FillRect(bx, by, bw * frac, bh, ColAmmoBar, a);
        }
    }

    // Armed for hotkey binding: lighten only THIS tile's interior (no text prompt).
    if (BindFlash(tile.cell, /*isLoadoutTile*/ false, /*loadoutKind*/ -1))
    {
        FillRect(tile.x + TilePad, tile.y + TilePad, tile.w - 2 * TilePad, tile.h - 2 * TilePad,
                 PackedColor(Color(1.0f, 1.0f, 1.0f, 0.22f)), a);
    }

    DrawBorder(tile.x, tile.y, tile.w, tile.h, 0.0016f, border, a * _outlineOpacity);

    // Persistent hotkey number - only on the player's own items, never vicinity.
    if (tile.cell.pane != InvPane::Ground)
        DrawHotkeyBadge(tile, a);
}

void DisplayInventory::DrawPaneTitle(const char *title, float px, float py, float pw, float alpha)
{
    // Grey header bar behind the section title (no full panel - tiles still float).
    FillRect(px, py, pw, InvHeaderH, AccentPanelCol(ColPaneHeader, 0.30f, 0.90f), alpha);
    DrawBorder(px, py, pw, InvHeaderH, 0.0014f, AccentBorderCol(ColPaneBorder), alpha * 0.6f);
    DrawLabel(title, px + 0.010f, py + InvHeaderH * 0.28f, TextSize * 1.05f, ColText, alpha);
}

void DisplayInventory::DrawGridBackground(float px, float py, float pw, float ph, float topExtra, float alpha)
{
    // Same FLUSH geometry as LayoutGridPane so empty cells line up under the items
    // and with the header (outer edges on px and px+pw).
    const float asp = GLOB_ENGINE->Width2D() / (float)GLOB_ENGINE->Height2D();
    const int   cols = Cols();
    const float gap = TilePad;
    const float tileW = (pw - (cols - 1) * gap) / cols;
    const float tileH = tileW * asp;
    const float pitchX = tileW + gap;
    const float pitchY = tileH + gap;
    const float top = py + InvHeaderH + topExtra;
    int rows = (pitchY > 0.0f) ? (int)((py + ph - top) / pitchY) : 0;
    if (rows > _gridRowsPerPage) rows = _gridRowsPerPage;   // match the paginated grid page
    if (rows <= 0) return;

    const float gridW = pw;                       // flush to the pane edges
    const float gridH = rows * pitchY - gap;

    // PERFORMANCE: one fill + continuous grid lines instead of a fill + 4-rect
    // border per cell (49 cells x 5 rects -> ~245 Draw2D calls each frame). This
    // is ~17 calls and looks the same (an outlined grid).
    const PackedColor empty = AccentPanelCol(PackedColor(Color(0.0f, 0.0f, 0.0f, _debugOpacity)),
                                             0.22f, _debugOpacity);
    FillRect(px, top, gridW, gridH, empty, alpha);

    const float t = 0.0013f;
    const float la = alpha * _outlineOpacity;
    const PackedColor lineCol = AccentBorderCol(ColTileBorder);
    for (int c = 0; c <= cols; c++)
    {
        float lx = (c == cols) ? (px + gridW - t) : (px + c * pitchX);
        FillRect(lx, top, t, gridH, lineCol, la);
    }
    for (int r = 0; r <= rows; r++)
    {
        float ly = (r == rows) ? (top + gridH - t) : (top + r * pitchY);
        FillRect(px, ly, gridW, t, lineCol, la);
    }
}

void DisplayInventory::DrawLoadoutTile(const InvTile &tile, float alpha, bool highlighted)
{
    // Empty slot at the empty-opacity slider, filled at the occupied-opacity slider.
    const float fop = tile.loadoutEmpty ? _debugOpacity : _occupiedOpacity;
    PackedColor bg = PackedColor(Color(0.0f, 0.0f, 0.0f, fop));
    PackedColor border = AccentBorderCol(ColTileBorder);
    if (!tile.loadoutEmpty && highlighted)
    {
        bg = ColTileHover;
        border = AccentColor();
    }

    FillRect(tile.x, tile.y, tile.w, tile.h, bg, alpha);

    if (tile.loadoutEmpty)
    {
        DrawGhost(LoadoutGhost(tile.loadoutKind), tile.x + TilePad, tile.y + TilePad,
                  tile.w - 2 * TilePad, tile.h - 2 * TilePad, alpha);
        static const char *names[] = {"Primary", "Launcher", "Handgun"};
        DrawLabel(names[tile.loadoutKind >= 0 && tile.loadoutKind < 3 ? tile.loadoutKind : 0],
                  tile.x + TilePad, tile.y + TilePad, TextSize * 0.65f, ColTextDim, alpha);
    }
    else
    {
        Ref<Texture> iconRef = LoadInvItemIcon(tile.cell);
        float tsz, tox, toy;
        CellTune(tile.cell, TuneScope::Loadout, tsz, tox, toy);   // equipped slot

        if (!iconRef)
        {
            DrawLabel(tile.cell.DisplayName(), tile.x + TilePad, tile.y + TilePad,
                      TextSize * 0.65f, ColText, alpha);
        }
        else if (tile.loadoutPistol)
        {
            DrawIcon(iconRef, tile.x + TilePad, tile.y + TilePad,
                     tile.w - 2 * TilePad, tile.h - 2 * TilePad, alpha,
                     tsz * InventoryGlobalIconScale(), tox, toy);
        }
        else
        {
            // Aspect-fit (not cover) so a near-square item like the Laser Designator
            // shows at its natural size instead of being zoomed/cropped to fill the
            // wide rifle slot. A real rifle's aspect nearly matches the slot anyway.
            DrawIcon(iconRef, tile.x + TilePad, tile.y + TilePad,
                     tile.w - 2 * TilePad, tile.h - 2 * TilePad, alpha,
                     tsz * InventoryGlobalIconScale(), tox, toy);
        }

        // Bottom-left: rounds in the loaded mag (weapon) or grenade count.
        if (tile.loadoutCount > 0)
        {
            char b[16];
            snprintf(b, sizeof(b), "%d", tile.loadoutCount);
            DrawLabel(b, tile.x + TilePad * 1.5f, tile.y + tile.h - 0.032f,
                      TextSize * 0.85f, ColText, alpha);
        }
    }

    // Armed for hotkey binding: lighten only THIS slot's interior (no text prompt).
    if (!tile.loadoutEmpty && BindFlash(tile.cell, /*isLoadoutTile*/ true, tile.loadoutKind))
    {
        FillRect(tile.x + TilePad, tile.y + TilePad, tile.w - 2 * TilePad, tile.h - 2 * TilePad,
                 PackedColor(Color(1.0f, 1.0f, 1.0f, 0.22f)), alpha);
    }

    DrawBorder(tile.x, tile.y, tile.w, tile.h, 0.0016f, border, alpha * _outlineOpacity);

    // Persistent hotkey number in the top-right corner.
    if (!tile.loadoutEmpty)
        DrawHotkeyBadge(tile, alpha);
}

const char *DisplayInventory::LoadoutGhost(int slotKind) const
{
    if (slotKind == 1)
        return "assets\\inventory\\launcher.paa";
    if (slotKind == 2)
        return "assets\\inventory\\pistol.paa";
    const int side = _model ? _model->PlayerSideIndex() : 0;
    return side == 1 || side == 2 ? "assets\\inventory\\ak_rifle.paa" : "assets\\inventory\\rifle.paa";
}

void DisplayInventory::DrawGhost(const char *path, float x, float y, float w, float h, float alpha)
{
    // A ghost is just the normal (transparent-background) icon drawn faint, so an
    // empty slot shows a placeholder of what goes there.
    if (!path || !*path || !IconFileExists(path))
    {
        return;   // Missing optional assets leave the text label visible.
    }
    Ref<Texture> tex = GlobLoadTexture(RString(path));
    if (!tex || IconIsSolidBlock(tex))
    {
        return;   // still streaming in -> opaque placeholder; skip so it isn't a black block
    }
    // Draw original category artwork as a faint black silhouette,
    // so it reads as a translucent grey shape of what belongs in the slot. Permanent
    // look - not tied to the debug opacity sliders.
    const PackedColor black(Color(0.0f, 0.0f, 0.0f, 1.0f));
    DrawIcon(tex, x, y, w, h, alpha * 0.40f, 1.0f, 0.0f, 0.0f, black);
}

void DisplayInventory::DrawPageIndicator(float alpha)
{
    // "Q   i/N   E" centred under the vicinity (the source name is shown up top in
    // the header). Q/E cycle pages, each page a nearby inventory.
    char buf[48];
    snprintf(buf, sizeof(buf), "Q   %d/%d   E", _vicPage + 1, _vicPageCount);
    const float size = TextSize * 1.05f;
    const float w = MeasureText(buf, size);
    const float cx = VicPaneX() + VicPaneW() * 0.5f;
    const float y = PaneY + VicPaneHt() + 0.006f;
    DrawLabel(buf, cx - w * 0.5f, y, size, ColText, alpha);
}

bool DisplayInventory::GridPageArrowRect(int which, float &x, float &y, float &w, float &h) const
{
    if (_gridPageCount <= 1)
        return false;
    w = 0.024f; h = 0.020f;
    y = PaneY + GridPaneHt() - h - 0.002f;
    x = (which == 0) ? (GridPaneX() + 0.004f)
                     : (GridPaneX() + GridPaneW() - w - 0.004f);
    return true;
}

void DisplayInventory::DrawGridPager(float alpha)
{
    if (_gridPageCount <= 1)
        return;
    for (int k = 0; k < 2; k++)
    {
        float x, y, w, h; GridPageArrowRect(k, x, y, w, h);
        const bool en = (k == 0) ? (_gridPage > 0) : (_gridPage < _gridPageCount - 1);
        FillRect(x, y, w, h, ColTileHover, alpha * (en ? 0.9f : 0.35f));
        DrawBorder(x, y, w, h, 0.0012f, AccentBorderCol(ColPaneBorder), alpha);
        const char *ar = (k == 0) ? "<" : ">";
        const float tw = MeasureText(ar, TextSize);
        DrawLabel(ar, x + (w - tw) * 0.5f, y + h * 0.10f, TextSize, ColText, alpha * (en ? 1.0f : 0.5f));
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%d/%d", _gridPage + 1, _gridPageCount);
    const float sz = TextSize * 0.9f;
    const float tw = MeasureText(buf, sz);
    float ax, ay, aw, ah; GridPageArrowRect(0, ax, ay, aw, ah);
    DrawLabel(buf, GridPaneX() + (GridPaneW() - tw) * 0.5f, ay + ah * 0.12f, sz, ColText, alpha);
}

void DisplayInventory::DrawTooltip(const InvTile &tile, float alpha)
{
    const RString name = tile.cell.DisplayName();

    char detail[128];
    detail[0] = 0;

    int ammo = 0, capacity = 0;
    bool hasAmmo = tile.cell.GetAmmo(ammo, capacity);
    if (!hasAmmo && tile.cell.kind == InvItemKind::Weapon && tile.cell.weapon)
    {
        hasAmmo = _model->WeaponLoadedAmmo(tile.cell.weapon, ammo, capacity);   // rifle's loaded mag
    }
    if (hasAmmo && capacity > 1)
    {
        const char *cn = (const char *)tile.cell.ClassName();
        const char *unit = tile.cell.isBox ? "shells"

                         : (cn && strcmp(cn, "JerryCan") == 0) ? "Liters"
                         : "rounds";
        snprintf(detail, sizeof(detail), "%d / %d %s  -  %.2f kg",
                 ammo, capacity, unit, tile.cell.Mass());
    }
    else
    {
        snprintf(detail, sizeof(detail), "%.2f kg", tile.cell.Mass());
    }

    const float tw = 0.20f;
    const float th = 0.07f;
    float tx = _cursorX + 0.012f;
    float ty = _cursorY + 0.012f;
    if (tx + tw > 1.0f) tx = _cursorX - tw - 0.012f;
    if (ty + th > 1.0f) ty = _cursorY - th - 0.012f;

    FillRect(tx, ty, tw, th, ColPaneHeader, alpha);
    DrawLabel(name, tx + 0.008f, ty + 0.008f, TextSize, ColText, alpha);
    DrawLabel(detail, tx + 0.008f, ty + 0.036f, TextSize * 0.85f, ColTextDim, alpha);
}

void DisplayInventory::DrawDragGhost(float alpha)
{
    if (!_dragging)
    {
        return;
    }

    const GridSize fp = (_dragCell.pane == InvPane::Grid) ? _dragCell.footprint : GridSize{1, 1};
    const float cellW = CellFrac();
    const float cellH = cellW * (GLOB_ENGINE->Width2D() / (float)GLOB_ENGINE->Height2D());
    const float gw = fp.w * cellW;
    const float gh = fp.h * cellH;

    Ref<Texture> iconRef = LoadInvItemIcon(_dragCell);
    DrawIcon(iconRef, _cursorX - gw * 0.5f, _cursorY - gh * 0.5f, gw, gh, alpha * 0.75f);
}

void DisplayInventory::DrawWeightBar(float alpha)
{
    const float carried = _model->CarriedMass();
    const float cap = _model->MaxCarryWeight();
    const float frac = Saturate(carried / cap);

    // Colour by ABSOLUTE encumbrance bands (kg), matching the movement penalties:
    //   0-20 green (full sprint), 20-35 amber (sensitivity/sprint drop),
    //   35-50 darker amber (+ max-sprint & walk drop), 50+ red (walk only).
    const PackedColor ColWeightDarkAmber(Color(0.93f, 0.55f, 0.15f, 1.00f));   // warm orange (yellower)
    PackedColor col = ColWeightOk;
    if (carried >= 50.0f)      col = ColWeightOver;        // red
    else if (carried >= 35.0f) col = ColWeightDarkAmber;   // darker amber
    else if (carried >= 20.0f) col = ColWeightHeavy;       // amber

    const float barH = 0.009f;                 // half the previous thickness
    const float barW = GridPaneW();
    const float barX = GridPaneX();
    const float barY = PaneY - 0.030f;

    FillRect(barX, barY, barW, barH, ColAmmoBarBg, alpha);
    FillRect(barX, barY, barW * frac, barH, col, alpha);
    DrawBorder(barX, barY, barW, barH, 0.0012f, ColPaneBorder, alpha);

    // Numbers at the right end of the bar.
    char buf[64];
    snprintf(buf, sizeof(buf), "%.1f / %.1f kg", carried, cap);
    const float txtW = MeasureText(buf, TextSize);
    DrawLabel(buf, barX + barW - txtW, barY - 0.026f, TextSize, ColText, alpha);
}

// The edit menu's sliders, laid out in two columns of 7. Order matters (columns
// fill top-to-bottom): global look first, then window sizes, accent RGB, and the
// finer tile-opacity / per-item tuning knobs.
enum EditSliderId
{
    ES_IconScale = 0, ES_GridCols, ES_VolCap, ES_VicW, ES_VicH, ES_InvW, ES_InvH,
    ES_AccentR, ES_AccentG, ES_AccentB, ES_Empty, ES_Occupied, ES_Outline, ES_ItemSize,
    ES_COUNT
};

// --- Edit-menu pages ------------------------------------------------------------------
enum EditPageId { EP_Inventory = 0, EP_COUNT };

int DisplayInventory::EditPageCount() const { return EP_COUNT; }

const char* DisplayInventory::EditPageTitle(int page) const
{
    return "Inventory Settings";
}

bool DisplayInventory::EditPageTabRect(int page, float& x, float& y, float& w, float& h) const
{
    if (page < 0 || page >= EP_COUNT)
        return false;
    float px, py, pw, ph; EditPanelRect(px, py, pw, ph);
    w = 0.072f; h = 0.020f;
    // Right-aligned on the title row, so the tabs never collide with the heading text.
    x = px + pw - 0.014f - (EP_COUNT - page) * (w + 0.006f);
    y = py + 0.010f;
    return true;
}

int DisplayInventory::EditSliderCount() const
{
    return ES_COUNT;
}

// Table-driven boolean toggles in the edit menu (checkbox + label). Adding a row here
// is all that's needed - layout, drawing and click handling iterate this table.
namespace
{
struct EditToggleDef { const char *label; bool (*get)(); void (*set)(bool); };
static const EditToggleDef kEditToggles[] = {
    { "Accent: theming",      InventoryAccentTheming,     SetInventoryAccentTheming     },
    { "Accent: borders",      InventoryAccentBorders,     SetInventoryAccentBorders     },
    { "Rifles in backpack",   InventoryWeaponsInBackpack, SetInventoryWeaponsInBackpack },
    { "Infinite weight",      InventoryInfiniteWeight,    SetInventoryInfiniteWeight    },
    { "Infinite volume",      InventoryInfiniteVolume,    SetInventoryInfiniteVolume    },
    { "Bearing callouts",     BearingCalloutsEnabled,     SetBearingCalloutsEnabled     },
    { "Bearing readout",      BearingReadoutEnabled,      SetBearingReadoutEnabled      },
    { "Grenade range (SP)",   GrenadeRangeEnabled,        SetGrenadeRangeEnabled        },
};
static const int kEditToggleCount = (int)(sizeof(kEditToggles) / sizeof(kEditToggles[0]));
}

void DisplayInventory::EditPanelRect(float &x, float &y, float &w, float &h) const
{
    w = 0.340f; h = 0.690f;
    // Centre in the free space BETWEEN the vicinity and inventory panels so the edit
    // menu never overlaps them (rather than screen-centre, which clipped the grid).
    const float gapL = VicPaneX() + VicPaneW();
    const float gapR = GridPaneX();
    x = (gapL + gapR) * 0.5f - w * 0.5f;
    y = 0.140f;
}

void DisplayInventory::EditSliderRect(int i, float &x, float &y, float &w, float &h) const
{
    float px, py, pw, ph; EditPanelRect(px, py, pw, ph);
    // height: 10 rows per column instead of 9.
    const int perCol = 9;
    const int col = i / perCol, row = i % perCol;
    const float colW = pw * 0.5f;
    x = px + 0.022f + col * colW;
    y = py + 0.064f + row * 0.0400f;
    w = colW - 0.040f;
    h = 0.013f;
}

bool DisplayInventory::EditToggleRect(int which, float &x, float &y, float &w, float &h) const
{
    if (_editPage != EP_Inventory)
        return false;
    float px, py, pw, ph; EditPanelRect(px, py, pw, ph);
    x = px + 0.022f;
    y = py + 0.408f + which * 0.0265f;   // single column of checkboxes below the sliders
    w = 0.017f; h = 0.017f;
    return true;
}

bool DisplayInventory::EditSwatchRect(float &x, float &y, float &w, float &h) const
{
    if (_editPage != EP_Inventory)
        return false;
    float px, py, pw, ph; EditPanelRect(px, py, pw, ph);
    x = px + pw - 0.085f; y = py + 0.408f; w = 0.065f; h = 0.040f;   // by the accent toggles
    return true;
}

bool DisplayInventory::EditResetRect(float &x, float &y, float &w, float &h) const
{
    float px, py, pw, ph; EditPanelRect(px, py, pw, ph);
    w = 0.150f; h = 0.026f;
    x = px + pw * 0.5f - w * 0.5f;
    y = py + ph - 0.062f;                // above the per-item tuning status line
    return true;
}

float DisplayInventory::EditSliderFrac(int i) const
{
    auto nm = [](float v, float a, float b){ float f = (v - a) / (b - a); return f < 0 ? 0.0f : (f > 1 ? 1.0f : f); };

    switch (i)
    {
        case ES_IconScale: return nm(InventoryGlobalIconScale(), 0.40f, 2.00f);
        case ES_GridCols:  return nm((float)InventoryGridCols(), 1.0f, 12.0f);
        case ES_VolCap:    return nm(InventoryVolumeCapOverride(), 0.0f, 150.0f);
        case ES_VicW:      return nm(InventoryVicWinScaleW(), 0.50f, 1.60f);
        case ES_VicH:      return nm(InventoryVicWinScaleH(), 0.50f, 1.60f);
        case ES_InvW:      return nm(InventoryInvWinScaleW(), 0.50f, 1.60f);
        case ES_InvH:      return nm(InventoryInvWinScaleH(), 0.50f, 1.60f);
        case ES_AccentR: { float r, g, b; InventoryAccentColor(r, g, b); return r; }
        case ES_AccentG: { float r, g, b; InventoryAccentColor(r, g, b); return g; }
        case ES_AccentB: { float r, g, b; InventoryAccentColor(r, g, b); return b; }
        case ES_Empty:     return _debugOpacity;
        case ES_Occupied:  return _occupiedOpacity;
        case ES_Outline:   return _outlineOpacity;
        case ES_ItemSize:
        {
            if (_editIcon.GetLength() == 0) return 0.0f;
            const IconTune *t = FindIconTune((const char *)_editIcon);
            if (!t) return 0.0f;
            const float s = (_editScope == TuneScope::Loadout)  ? t->lsize
                          : (_editScope == TuneScope::Vicinity) ? t->vsize : t->size;
            return nm(s, kSizeMin, kSizeMax);
        }
    }
    return 0.0f;
}

void DisplayInventory::EditSliderSet(int i, float f)
{
    auto lp = [](float a, float b, float t){ return a + t * (b - a); };
    switch (i)
    {
        case ES_IconScale: SetInventoryGlobalIconScale(lp(0.40f, 2.00f, f)); break;
        case ES_GridCols:  SetInventoryGridCols((int)(lp(1.0f, 12.0f, f) + 0.5f)); break;
        case ES_VolCap:    SetInventoryVolumeCapOverride(lp(0.0f, 150.0f, f)); break;
        case ES_VicW:      SetInventoryVicWinScaleW(lp(0.50f, 1.60f, f)); break;
        case ES_VicH:      SetInventoryVicWinScaleH(lp(0.50f, 1.60f, f)); break;
        case ES_InvW:      SetInventoryInvWinScaleW(lp(0.50f, 1.60f, f)); break;
        case ES_InvH:      SetInventoryInvWinScaleH(lp(0.50f, 1.60f, f)); break;
        case ES_AccentR: { float r, g, b; InventoryAccentColor(r, g, b); SetInventoryAccentColor(f, g, b); } break;
        case ES_AccentG: { float r, g, b; InventoryAccentColor(r, g, b); SetInventoryAccentColor(r, f, b); } break;
        case ES_AccentB: { float r, g, b; InventoryAccentColor(r, g, b); SetInventoryAccentColor(r, g, f); } break;
        case ES_Empty:     _debugOpacity = f; break;
        case ES_Occupied:  _occupiedOpacity = f; break;
        case ES_Outline:   _outlineOpacity = f; break;
        case ES_ItemSize:
            if (_editIcon.GetLength() > 0)
            {
                IconTune &tn = EditIconTune((const char *)_editIcon);
                (_editScope == TuneScope::Loadout ? tn.lsize
                 : _editScope == TuneScope::Vicinity ? tn.vsize : tn.size) =
                    kSizeMin + f * (kSizeMax - kSizeMin);
                SaveIconTuning();
            }
            break;
    }
    // Global UI settings persist to the cfg file; opacity/item-size don't. Mark dirty
    // so a dragged global setting is saved exactly once when the drag ends.
    if (i != ES_Empty && i != ES_Occupied && i != ES_Outline && i != ES_ItemSize)
        _uiSettingsDirty = true;
}

void DisplayInventory::EditSliderLabel(int i, char *buf, int n) const
{

    float r, g, b; InventoryAccentColor(r, g, b);
    switch (i)
    {
        case ES_IconScale: snprintf(buf, n, "Global change icon size  %.2fx", InventoryGlobalIconScale()); break;
        case ES_GridCols:  snprintf(buf, n, "Columns  %d", InventoryGridCols()); break;
        case ES_VolCap:
            if (InventoryVolumeCapOverride() <= 0.0f) snprintf(buf, n, "Capacity  side default");
            else snprintf(buf, n, "Capacity  %.0f L", InventoryVolumeCapOverride());
            break;
        case ES_VicW: snprintf(buf, n, "Vicinity W  %.2f", InventoryVicWinScaleW()); break;
        case ES_VicH: snprintf(buf, n, "Vicinity H  %.2f", InventoryVicWinScaleH()); break;
        case ES_InvW: snprintf(buf, n, "Inventory W  %.2f", InventoryInvWinScaleW()); break;
        case ES_InvH: snprintf(buf, n, "Inventory H  %.2f", InventoryInvWinScaleH()); break;
        case ES_AccentR: snprintf(buf, n, "Accent R  %.2f", r); break;
        case ES_AccentG: snprintf(buf, n, "Accent G  %.2f", g); break;
        case ES_AccentB: snprintf(buf, n, "Accent B  %.2f", b); break;
        case ES_Empty:    snprintf(buf, n, "Empty cell  %d%%", (int)(_debugOpacity * 100 + 0.5f)); break;
        case ES_Occupied: snprintf(buf, n, "Filled tile  %d%%", (int)(_occupiedOpacity * 100 + 0.5f)); break;
        case ES_Outline:  snprintf(buf, n, "Outline  %d%%", (int)(_outlineOpacity * 100 + 0.5f)); break;
        case ES_ItemSize:
            if (_editIcon.GetLength() == 0) snprintf(buf, n, "This icon size  (right-click an item)");
            else snprintf(buf, n, "This icon size  %.2f",
                          kSizeMin + EditSliderFrac(ES_ItemSize) * (kSizeMax - kSizeMin));
            break;
        default: if (n > 0) buf[0] = 0; break;
    }
}

bool DisplayInventory::EditMenuClick(float x, float y)
{
    float tx, ty, tw, th;
    // Page tabs first: they sit on the title row, clear of everything else.
    for (int pg = 0; pg < EditPageCount(); pg++)
    {
        if (!EditPageTabRect(pg, tx, ty, tw, th))
            continue;
        if (x >= tx && x <= tx + tw && y >= ty && y <= ty + th)
        {
            _editPage = pg;
            _activeSlider = -1;               // indices mean different things per page
            Rebuild();
            return true;
        }
    }
    for (int k = 0; k < kEditToggleCount; k++)
    {
        if (!EditToggleRect(k, tx, ty, tw, th))
            break;                            // page has no toggles
        // Allow clicking the label too (a bit to the right of the box), not just the box.
        if (x >= tx && x <= tx + 0.16f && y >= ty - 0.002f && y <= ty + th + 0.002f)
        {
            kEditToggles[k].set(!kEditToggles[k].get());
            SaveInventorySettings();
            Rebuild();
            return true;
        }
    }
    EditResetRect(tx, ty, tw, th);
    if (x >= tx && x <= tx + tw && y >= ty && y <= ty + th)
    {
        ResetInventorySettingsToDefault();
        ResetCombatAssistSettings();
        SaveInventorySettings();
        Rebuild();
        return true;
    }
    return false;
}

void DisplayInventory::DrawEditMenu(float alpha)
{
    float px, py, pw, ph; EditPanelRect(px, py, pw, ph);

    // Background panel behind every option, at the same opacity as the empty grid
    // tiles so it reads as a faint tint rather than a solid slab.
    FillRect(px, py, pw, ph, PackedColor(Color(0.0f, 0.0f, 0.0f, _debugOpacity)), alpha);
    DrawBorder(px, py, pw, ph, 0.0018f, AccentBorderCol(ColPaneBorder, 0.85f), alpha);
    DrawLabel(EditPageTitle(_editPage), px + 0.014f, py + 0.012f, TextSize * 1.05f, ColText, alpha);

    // Page tabs, top-right. The active one is filled with the accent.
    for (int pg = 0; pg < EditPageCount(); pg++)
    {
        float tx, ty, tw, th;
        if (!EditPageTabRect(pg, tx, ty, tw, th))
            continue;
        FillRect(tx, ty, tw, th, (pg == _editPage) ? AccentColor() : ColAmmoBarBg, alpha);
        DrawBorder(tx, ty, tw, th, 0.0012f, ColPaneBorder, alpha);
        const char *nm = "Inventory";
        const float nw = MeasureText(nm, TextSize * 0.64f);
        DrawLabel(nm, tx + (tw - nw) * 0.5f, ty + th * 0.22f, TextSize * 0.64f, ColText, alpha);
    }

    for (int i = 0; i < EditSliderCount(); i++)
    {
        float x, y, w, h; EditSliderRect(i, x, y, w, h);
        char lbl[64]; EditSliderLabel(i, lbl, sizeof(lbl));
        DrawLabel(lbl, x, y - 0.017f, TextSize * 0.70f, ColTextDim, alpha);
        FillRect(x, y, w, h, ColAmmoBarBg, alpha);
        const float f = EditSliderFrac(i);
        const float thumbW = 0.010f;
        FillRect(x + f * (w - thumbW), y - 0.002f, thumbW, h + 0.004f,
                 (_activeSlider == i) ? AccentColor() : ColTileHover, alpha);
        DrawBorder(x, y, w, h, 0.0010f, ColPaneBorder, alpha * 0.7f);
    }

    // Boolean toggles (accent, HUD bars, gameplay overrides) - table-driven.
    for (int k = 0; k < kEditToggleCount; k++)
    {
        float x, y, w, h;
        if (!EditToggleRect(k, x, y, w, h))
            break;                            // page has no toggles
        const bool on = kEditToggles[k].get();
        FillRect(x, y, w, h, on ? AccentColor() : ColAmmoBarBg, alpha);
        DrawBorder(x, y, w, h, 0.0012f, ColPaneBorder, alpha);
        DrawLabel(kEditToggles[k].label, x + w + 0.008f, y - 0.001f, TextSize * 0.72f, ColText, alpha);
    }
    // Accent colour swatch (live RGB preview) - inventory page only.
    {
        float x, y, w, h;
        if (EditSwatchRect(x, y, w, h))
        {
            float r, g, b; InventoryAccentColor(r, g, b);
            DrawLabel("accent", x, y - 0.016f, TextSize * 0.68f, ColTextDim, alpha);
            FillRect(x, y, w, h, PackedColor(Color(r, g, b, 1.0f)), alpha);
            DrawBorder(x, y, w, h, 0.0014f, ColPaneBorder, alpha);
        }
    }
    // Reset-to-default button.
    {
        float x, y, w, h; EditResetRect(x, y, w, h);
        FillRect(x, y, w, h, ColTileHover, alpha);
        DrawBorder(x, y, w, h, 0.0014f, AccentBorderCol(ColPaneBorder, 0.85f), alpha);
        const char *t = "Reset to default";
        const float tw = MeasureText(t, TextSize * 0.8f);
        DrawLabel(t, x + (w - tw) * 0.5f, y + h * 0.26f, TextSize * 0.8f, ColText, alpha);
    }
    // Per-item tuning status line at the bottom.
    if (_editIcon.GetLength() > 0)
    {
        const char *scope = (_editScope == TuneScope::Loadout)  ? "equipped"
                          : (_editScope == TuneScope::Vicinity) ? "vicinity" : "inventory";
        char r[200];
        snprintf(r, sizeof(r), "Tuning %s [%s] - arrows nudge, [ ] , . ; ' footprint",
                 (const char *)_editIcon, scope);
        DrawLabel(r, px + 0.014f, py + ph - 0.020f, TextSize * 0.66f, AccentColor(), alpha);
    }
    else
    {
        DrawLabel("Right-click an item to tune its size/position",
                  px + 0.014f, py + ph - 0.020f, TextSize * 0.66f, ColTextDim, alpha);
    }
}

void DisplayInventory::OnDraw(EntityAI *vehicle, float alpha)
{
    base::OnDraw(vehicle, alpha);

    // Storage grid backdrop first, then all tiles.
    DrawGridBackground(GridPaneX(), PaneY, GridPaneW(), GridPaneHt(), StorageTopExtra(), alpha);

    for (int i = 0; i < _loadoutTiles.Size(); i++)
    {
        DrawLoadoutTile(_loadoutTiles[i], alpha, &_loadoutTiles[i] == _hovered);
    }
    for (int i = 0; i < _groundTiles.Size(); i++)
    {
        DrawTile(_groundTiles[i], alpha, &_groundTiles[i] == _hovered);
    }
    for (int i = 0; i < _gridTiles.Size(); i++)
    {
        DrawTile(_gridTiles[i], alpha, &_gridTiles[i] == _hovered);
    }

    // Headers drawn last (flush with the tiles). Page indicator below the vicinity.
    DrawPaneTitle("Vicinity", VicPaneX(), PaneY, VicPaneW(), alpha);
    DrawPaneTitle("Inventory", GridPaneX(), PaneY, GridPaneW(), alpha);
    DrawPageIndicator(alpha);
    DrawGridPager(alpha);

    // Source of the CURRENT page (changes as you cycle with Q/E), shown up top in
    // the Vicinity header so you know whose gear you're looking at.
    {
        RString src = (_vicPage >= 0 && _vicPage < _vicPageLabels.Size())
                          ? _vicPageLabels[_vicPage]
                          : RString("Nothing nearby");
        const float sz = TextSize * 0.8f;
        const float w = MeasureText((const char *)src, sz);
        DrawLabel((const char *)src, VicPaneX() + VicPaneW() - w - 0.006f, PaneY + InvHeaderH * 0.34f,
                  sz, ColTextDim, alpha);
    }

    DrawWeightBar(alpha);
    if (InventoryEditMenuVisible())
    {
        DrawEditMenu(alpha);
    }

    // Hotkey confirmation toast: after pressing a number, "N   <weapon>" fades in the
    // top-left corner. No text prompt while arming - the armed tile just lightens.
    if (_hotkeyToastTime > 0.0f && _hotkeyToast.GetLength() > 0)
    {
        const float fade = _hotkeyToastTime < 0.6f ? (_hotkeyToastTime / 0.6f) : 1.0f;
        const float sz = TextSize * 1.15f;
        const float tx = 0.020f, ty = 0.028f;
        const float tw = MeasureText((const char *)_hotkeyToast, sz);
        // Small backing chip so it reads over any scene.
        FillRect(tx - 0.006f, ty - 0.006f, tw + 0.012f, sz + 0.012f,
                 PackedColor(Color(0.06f, 0.07f, 0.09f, 0.85f)), alpha * fade);
        DrawBorder(tx - 0.006f, ty - 0.006f, tw + 0.012f, sz + 0.012f, 0.0012f,
                   AccentColor(), alpha * fade);
        DrawLabel((const char *)_hotkeyToast, tx, ty, sz, ColText, alpha * fade);
    }

    if (_hovered && !_dragging && _hoverTime >= TooltipDelay)
    {
        DrawTooltip(*_hovered, alpha);
    }

    // Volume feedback: if the item being dragged into the inventory won't fit the
    // side's (invisible) volume budget, flood the storage grid red and label it.
    if (_dragging && _dragCell.pane != InvPane::Grid &&
        _model->CanTransfer(_dragCell, InvPane::Grid) == InvTransferResult::OverVolume)
    {
        const PackedColor floodRed(Color(0.70f, 0.14f, 0.14f, 0.62f));
        FillRect(GridPaneX(), PaneY, GridPaneW(), GridPaneHt(), floodRed, alpha);
        const char *msg = "Not Enough Space";
        const float sz = TextSize * 2.4f;   // large
        const float w = MeasureText(msg, sz);
        DrawLabel(msg, GridPaneX() + (GridPaneW() - w) * 0.5f, PaneY + GridPaneHt() * 0.44f, sz, ColText, alpha);
    }

    DrawDragGhost(alpha);
}

} // namespace Poseidon
