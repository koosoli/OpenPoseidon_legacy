#pragma once

#include <Poseidon/Foundation/Types/RemoveLinks.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Core/Types.hpp>
#include <Poseidon/Graphics/Rendering/Draw/FontMapping.hpp>


namespace Poseidon { namespace ui { class FontRenderer; } }

namespace Poseidon
{

struct CharInfo
{
	 // original char. dimensions
	int height;
	int width;

	RStringB nameTex;
	int xTex, yTex;
	int wTex, hTex; // nearest power of 2
	// Arma 3's packed fonts: where in the line the glyph is drawn, and how far it moves the pen
	// (-1: its width, as OFP's cell fonts)
	int offsetX = 0, offsetY = 0;
	int advance = -1;
	int Advance() const {return advance >= 0 ? advance : width;}
};

class Font: public RefCountWithLinks
{
	friend class Engine;
	friend class FontCache;

	private:

	enum {StartChar=32};
	int _nChars;
	AutoArray<CharInfo> _infos;

	int _maxHeight;
	char _name[32];

	int _langID;

	bool _isFreeType = false;
	ui::FontRenderer* _ftRenderer = nullptr;
	// FreeType pixelSize that pairs with _maxHeight to match the classic bitmap
	// cell. Draw-time pixelSize is picked from this via FTPixelSizeForSizeH.
	int _ftReferencePx = 0;
	float _ftWidthScale = 1.0f;
	// Atlas-px Y shift applied to the glyph pen so OFL replacements that sit
	// higher / lower than the bitmap baseline can be re-aligned.  + = down.
	float _ftBaselineOffset = 0.0f;
	// Atlas-px gap inserted after each glyph's natural advance, so the user
	// can tighten/loosen letter spacing without re-tuning widthScale.
	float _ftLetterSpacing = 0.0f;

	public:
	Font();
	void DoDestruct();

	const char *Name() const {return _name;}
	void Load( const char *name );
	float Height() const; // normalized height

	int GetLangID() const {return _langID;}
	void SetLangID(int langID) {_langID = langID;}

	bool IsFreeType() const { return _isFreeType; }
	ui::FontRenderer* FTRenderer() const { return _ftRenderer; }
	int FTReferencePx() const { return _ftReferencePx; }
	int MaxHeight() const { return _maxHeight; }
	float FTWidthScale() const { return _ftWidthScale; }
	float FTBaselineOffset() const { return _ftBaselineOffset; }
	float FTLetterSpacing() const { return _ftLetterSpacing; }

	// Atlas rasterization pixelSize for the given sizeH, bucketed to keep the
	// per-size glyph cache bounded.
	int FTPixelSizeForSizeH(float sizeH) const;

	~Font() override;

};

bool HasFreeTypeFontMapping(const char* lowName);
} // namespace Poseidon
