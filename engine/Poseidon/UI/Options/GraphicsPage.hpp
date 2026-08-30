#pragma once

// Graphics settings page for the production OptionsShell.
//
// Live-apply screen — every row change flows to the engine on the
// frame it happens, no Apply button (vs Display's restart-required
// pattern with ConfirmRevert).  Persistence on Unmount mirrors
// AudioPage: the user's session-end state writes to graphics.cfg.
//
// WithCloseRow appends a Close action after the last kRow* entry.
//
// Quality Preset row is meta-state: selecting Low/Med/High/Ultra
// stamps the four tier rows from that preset's bundle (write-through).
// Touching any tier row re-derives the preset to "Custom" without
// snapping back.  Per-user knobs (Anti-aliasing / Supersampling /
// Multitexturing / VSync / FPS Cap / Brightness / Gamma) are NOT
// preset-driven; they keep their value across preset changes.

#include <Poseidon/UI/Settings/GraphicsConfig.hpp>
#include <Poseidon/UI/Options/ScrollListPage.hpp>

#include <array>
#include <string>


namespace Poseidon
{
class GraphicsPage : public ScrollListPage
{
public:
	GraphicsPage();

	const char* TitleText() const override;

	static int BrightnessToSlider(float value);
	static float SliderToBrightness(int slider);
	static int GammaToSlider(float value);
	static float SliderToGamma(int slider);
	static int FpsCapValueToIndex(int fps);
	static int MsaaSamplesToIndex(int samples);
	static int MsaaIndexToSamples(int index);
	static int RenderScaleToIndex(float scale);
	static float RenderScaleIndexToValue(int index);

	void Mount(OptionsShell& shell) override;
	void OnReshown(OptionsShell& shell) override;
	void Unmount(OptionsShell& shell) override;

protected:
	OptionsScrollList::Provider& ProviderRef() override { return m_provider; }

private:
	static const char* CloseLabel();
	static const char* CloseDescription();

	class GraphicsProvider : public OptionsScrollList::Provider
	{
	public:
		enum : int {
			kRowPreset         = 0,
			kRowTerrain        = 1,
			kRowObjectLod      = 2,
			kRowShadow         = 3,
			kRowParticles      = 4,
			// Grass sits with the other quality tiers, immediately after Particles,
			// because it is the most expensive of them by a wide margin (28.6% of the
			// GPU frame at Ultra) and a player hunting for frames should meet it early.
			kRowGrass          = 5,
			kRowVsync          = 6,
			kRowFpsCap         = 7,
			kRowBrightness     = 8,
			kRowGamma          = 9,
			kRowAdvanced       = 10,
			kRowAntiAliasing   = 11,
			kRowSupersampling  = 12,
			kRowMultitexturing = 13,
			kRowUpscaler       = 14,
			kRowUpscalerQuality = 15,
			kRowSharpness      = 16,
			kRowCount          = 17,
		};

		void SetPage(class GraphicsPage* page) { m_page = page; }

		int  RowCount() const override         { return kRowCount; }
		const char* RowLabel(int row) const override;
		const char* RowDescription(int row) const override;
		OptionsScrollList::RowDef RowFor(int row) const override;
		int  RowValue(int row) const override;
		void SetRowValue(int row, int v) override;
		const char* SliderValueText(int row) const override;
		OptionsScrollList::Kind RowKind(int row) const override;

	private:
		GraphicsPage* m_page = nullptr;
		mutable std::string m_sliderValueText;
	};

	GraphicsConfig                  m_cfg;
	GraphicsProvider                m_graphics;
	OptionsScrollList::WithCloseRow m_provider{m_graphics, CloseLabel(), CloseDescription()};

	std::array<std::string, 5>      m_presetLabels;
	std::array<const char*, 5>      m_presetCStrs{};
	std::array<std::string, 5>      m_terrainLabels;
	std::array<const char*, 5>      m_terrainCStrs{};
	std::array<std::string, 4>      m_tierFourLabels;
	std::array<const char*, 4>      m_tierFourCStrs{};
	std::array<std::string, 4>      m_shadowLabels;
	std::array<const char*, 4>      m_shadowCStrs{};
	std::array<std::string, 3>      m_particlesLabels;
	std::array<const char*, 3>      m_particlesCStrs{};
	// Off / Low / Medium / High / Ultra — the only tier row that offers both Off
	// and the full five steps.
	std::array<std::string, 5>      m_grassLabels;
	std::array<const char*, 5>      m_grassCStrs{};
	std::array<std::string, 4>      m_msaaLabels;
	std::array<const char*, 4>      m_msaaCStrs{};
	std::array<std::string, 5>      m_renderScaleLabels;
	std::array<const char*, 5>      m_renderScaleCStrs{};
	std::array<std::string, 4>      m_upscalerLabels;
	std::array<const char*, 4>      m_upscalerCStrs{};
	std::array<std::string, 4>      m_upscalerQualityLabels;
	std::array<const char*, 4>      m_upscalerQualityCStrs{};
	std::array<std::string, 2>      m_offOnLabels;
	std::array<const char*, 2>      m_offOnCStrs{};
	std::array<std::string, 3>      m_vsyncLabels;
	std::array<const char*, 3>      m_vsyncCStrs{};
	std::array<std::string, 7>      m_fpsCapLabels;
	std::array<const char*, 7>      m_fpsCapCStrs{};

	void RefreshLocalizedChoices();

	friend class GraphicsProvider;
};

} // namespace Poseidon
