// Adapted from the author-supplied CWR-Physical Inventory source donation (2026).
// Distributed under this project's GPL-3.0-or-later licence and Section 7 terms; see LICENSE.
#pragma once

// Poseidon/UI/Inventory/DisplayInventory.hpp
//
// Paginated vicinity and storage panes with equipped slots. The built-in UI
// omits the optional 3D character preview. Pickups use model ownership, slot,
// weight and volume checks; tile placement is cosmetic.

#include "InventoryModel.hpp"

#include <Poseidon/UI/Controls/UIControls.hpp>
#include <Poseidon/UI/Controls/UIControlsBase.hpp>

namespace Poseidon
{

class Texture;

//! Preload inventory gear icons a batch at a time from the in-game update loop, so
//! they're resident before the inventory is opened (no open/page-flip hitch).
//! Returns true once everything is loaded.
bool PreloadInventoryIconsStep(int budget);

class CInventoryPreview : public ControlObject
{
    typedef ControlObject base;

public:
    CInventoryPreview(ControlsContainer *parent, int idc, const ParamEntry &cls);
    void SetSubject(Person *player);
    void OnDraw(float alpha) override;

private:
    LLink<Person> _subject;
};

/// One rendered tile. Size in pixels is derived from the cell's grid
/// footprint x the display's cell size - see DisplayInventory::CellPixels.
struct InvTile
{
    InvCell cell;
    float x = 0, y = 0, w = 0, h = 0;

    // Loadout-strip annotations (ignored for ordinary grid/ground tiles).
    bool  loadoutSlot  = false;   //!< this is one of the top equipped-gear slots
    bool  loadoutEmpty = false;   //!< slot has no equipped item -> draw faint
    bool  loadoutPistol = false;  //!< pistol slot (vs rifle/launcher) -> ghost choice
    int   loadoutKind  = 0;       //!< 0=primary(rifle) 1=secondary(launcher) 2=handgun(pistol)
    int   loadoutCount = 0;       //!< rounds loaded (weapon) / grenade count
    int   loadoutSpare = 0;       //!< spare compatible magazines (weapon)
    float fade = 1.0f;            //!< per-tile alpha multiplier (scroll-edge fade)
    Ref<Texture> icon;            //!< resolved once at layout time (not per frame)
    bool iconBlock = false;       //!< icon missing/opaque -> draw ghost (computed at layout)

    bool Contains(float px, float py) const
    {
        return px >= x && px <= x + w && py >= y && py <= y + h;
    }
};

//! Which context's image size/nudge the tuning controls act on. A weapon shows in
//! up to three places (equipped slot, inventory grid, vicinity) and each keeps its
//! own size, so the controls target whichever one was right-clicked.
enum class TuneScope { Inventory, Vicinity, Loadout };

class DisplayInventory : public Display
{
    typedef Display base;

public:
    explicit DisplayInventory(ControlsContainer *parent, Person *player);
    ~DisplayInventory() override;

    Control *OnCreateCtrl(int type, int idc, const ParamEntry &cls) override;
    ControlObject *OnCreateObject(int type, int idc, const ParamEntry &cls) override;

    void OnDraw(EntityAI *vehicle, float alpha) override;
    void OnSimulate(EntityAI *vehicle) override;
    bool OnKeyDown(unsigned nChar, unsigned nRepCnt, unsigned nFlags) override;

    void OnLButtonDown(float x, float y);
    void OnLButtonUp(float x, float y);
    void OnMouseMove(float x, float y);

private:
    void Rebuild();
    void LayoutGroundPane();
    void LayoutGridPane();
    void LayoutLoadoutStrip();

    /// Height of the top loadout strip and the extra offset the storage grid
    /// starts at, in screen fractions (depend on the runtime aspect ratio).
    float LoadoutStripH() const;
    float StorageTopExtra() const;
    float ContentGap() const;                 //!< gap header->content and loadout->grid

    // Effective panel geometry: the base layout constants scaled by the edit-menu
    // window-size settings. Vicinity grows rightward from its left edge; the
    // inventory grows leftward from its fixed right edge (so it never runs off).
    float VicPaneX() const;                   //!< vicinity left edge
    float VicPaneW() const;                   //!< vicinity width  (scaled)
    float VicPaneHt() const;                  //!< vicinity height (scaled)
    float GridPaneX() const;                  //!< inventory left edge (scaled)
    float GridPaneW() const;                  //!< inventory width  (scaled)
    float GridPaneHt() const;                 //!< inventory height (scaled)
    int   Cols() const;                       //!< runtime inventory columns
    float CellFrac() const;                   //!< one grid cell, in screen fractions

    // --- Edit menu (polished settings panel) -------------------------------
    void DrawEditMenu(float alpha);
    void EditPanelRect(float &x, float &y, float &w, float &h) const;
    //! Inventory appearance and carrying settings.
    //! Everything below is page-relative, so the draw and click loops need no page logic of
    //! their own - they just iterate whatever the current page reports.
    int  EditPageCount() const;
    bool EditPageTabRect(int page, float &x, float &y, float &w, float &h) const;
    const char *EditPageTitle(int page) const;
    int  EditSliderCount() const;
    void EditSliderRect(int i, float &x, float &y, float &w, float &h) const;
    float EditSliderFrac(int i) const;         //!< current value as 0..1
    void EditSliderSet(int i, float frac);     //!< apply a 0..1 value
    void EditSliderLabel(int i, char *buf, int n) const;
    bool EditToggleRect(int which, float &x, float &y, float &w, float &h) const; //!< 0=theme 1=borders
    bool EditSwatchRect(float &x, float &y, float &w, float &h) const;            //!< accent preview
    bool EditResetRect(float &x, float &y, float &w, float &h) const;
    //! Route a click at (x,y) inside the edit menu; returns true if it was consumed.
    bool EditMenuClick(float x, float y);

    const InvTile *TileAt(float x, float y) const;
    bool PaneAt(float x, float y, InvPane &out) const;

    void DrawPaneTitle(const char *title, float px, float py, float pw, float alpha);
    void DrawGridBackground(float px, float py, float pw, float ph, float topExtra, float alpha);
    void DrawPageIndicator(float alpha);   //!< "Q  i/N  E" under the vicinity
    void DrawGridPager(float alpha);       //!< prev/next arrows under the storage grid
    //! which: 0 = prev (left), 1 = next (right). Returns false if the grid is a single page.
    bool GridPageArrowRect(int which, float &x, float &y, float &w, float &h) const;
    void DrawTile(const InvTile &tile, float alpha, bool highlighted);
    void DrawLoadoutTile(const InvTile &tile, float alpha, bool highlighted);
    /// Faint ghost silhouette drawn in an empty loadout slot.
    void DrawGhost(const char *path, float x, float y, float w, float h, float alpha);
    //! Per-side ghost silhouette icon for an empty loadout slot.
    //! slotKind: 0=primary(rifle) 1=secondary(launcher) 2=handgun(pistol).
    const char *LoadoutGhost(int slotKind) const;
    void DrawDragGhost(float alpha);
    void DrawTooltip(const InvTile &tile, float alpha);
    void DrawWeightBar(float alpha);

    //! One-time per open: bind the player's rifle/launcher/pistol to keys 1/2/3
    //! (only slots the player hasn't already customised).
    void SeedDefaultHotkeys();
    //! True if `cell` matches the class currently armed for binding (right-clicked).
    bool IsBindArmed(const InvCell &cell) const;
    //! True if THIS specific tile should show the armed "flash" - matches the armed
    //! class AND the exact tile context (loadout vs pane), so a duplicate copy in the
    //! vicinity doesn't flash alongside the one that was actually clicked.
    bool BindFlash(const InvCell &cell, bool isLoadoutTile, int loadoutKind) const;
    //! Draw the persistent hotkey-number badge in a tile's top-right corner, if bound.
    void DrawHotkeyBadge(const InvTile &tile, float alpha) const;

private:
    SRef<InventoryModel> _model;
    CInventoryPreview *_preview = nullptr;

    AutoArray<InvTile> _groundTiles;
    AutoArray<InvTile> _gridTiles;
    AutoArray<InvTile> _loadoutTiles;

    bool _lastLDown = false;   //!< edge-detect the left button in OnSimulate
    bool _lastRDown = false;   //!< edge-detect the right button (hotkey binding)
    RString _bindPending;      //!< kind-qualified key ("w:"/"m:") right-clicked, awaiting 1-0
    RString _bindPendingLabel; //!< that item's display name (for the confirm toast)
    const void *_bindPendingInst = nullptr; //!< clicked item pointer (badge/flash identity)
    bool    _bindArmedLoadout = false;  //!< armed tile was an equipped-loadout slot
    int     _bindArmedKind = -1;        //!< armed loadout slot kind (0/1/2), else -1
    InvPane _bindArmedPane = InvPane::Grid; //!< armed tile's pane (so only IT flashes)
    RString _hotkeyToast;      //!< top-left "bound to N" confirmation text
    float   _hotkeyToastTime = 0.0f;   //!< seconds the toast stays up
    bool _dragArmed = false;
    bool _dragging = false;
    InvCell _dragCell;
    float _dragStartX = 0, _dragStartY = 0;
    float _cursorX = 0, _cursorY = 0;
    static constexpr float DragThreshold = 0.01f;

    const InvTile *_hovered = nullptr;
    const void *_hoverItem = nullptr;   //!< hovered item identity (stable across
                                        //!< Rebuild) so the tooltip timer doesn't reset
    float _hoverTime = 0.0f;
    Foundation::UITime _lastUiTime;
    bool _uiClockStarted = false;
    float _resyncTime = 0.0f;
    static constexpr float TooltipDelay = 0.35f;

    // Vicinity is paginated (Reforger-style): Q = previous page, E = next.
    float _vicViewH     = 0.0f;         //!< usable height of one page
    int   _vicPage      = 0;            //!< current page (0-based)
    int   _vicPageCount = 1;            //!< total pages
    AutoArray<RString> _vicPageLabels;  //!< source name shown on each page
    AutoArray<int>     _vicPageSource;  //!< page -> VicinitySources() index (drop routing)

    // The storage grid is paginated too, so a full inventory doesn't spill past the
    // panel - prev/next arrows at the bottom flip pages.
    int _gridPage      = 0;
    int _gridPageCount = 1;
    int _gridRowsPerPage = 1;           //!< tile rows shown per grid page

    // Tile darkness sliders: empty-cell and occupied-tile alpha, outline alpha.
    float _debugOpacity    = 0.17f;     //!< empty-cell alpha (default 17%)
    float _occupiedOpacity = 0.34f;     //!< filled-tile alpha (default 34%)
    float _outlineOpacity  = 0.55f;     //!< tile outline alpha

    // Edit-menu interaction: which slider is being dragged (-1 = none), and whether
    // a global UI setting changed during the drag (so we save once on release).
    int   _activeSlider    = -1;
    bool  _uiSettingsDirty = false;
    //! Current settings page.
    int   _editPage        = 0;

    // Icon tuning tool (edit menu). Right-click an item to select its class here,
    // then adjust with the size slider + arrow keys (nudge) + [ ] , . ; ' (footprint).
    RString   _editIcon;                          //!< tuning key selected ("" = none)
    TuneScope _editScope = TuneScope::Inventory;  //!< which context's size/nudge to edit

    // --- layout (fractions of screen) --------------------------------------
    static constexpr float PaneY = 0.10f;
    static constexpr float PaneH = 0.78f;   //!< inventory (right) height
    // Vicinity panel proportioned like the Reforger reference (portrait, W:H~0.64
    // in pixels). Tuned for ~16:9; GroundW/VicH give that pixel aspect.
    static constexpr float GroundX = 0.03f;
    static constexpr float GroundW = 0.26f;
    static constexpr float VicH    = 0.72f; //!< vicinity height (portrait panel)
    static constexpr float GridX = 0.66f;
    static constexpr float GridW = 0.30f;
    static constexpr int   GridCols = 7;   //!< must match InventoryModel::GridCols
    static constexpr float CellPixelFrac = GridW / GridCols;   //!< one cell, in
                                                                //!< screen-fraction units
};

} // namespace Poseidon
