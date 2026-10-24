
#include <Poseidon/Core/Application.hpp>

#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/Graphics/Rendering/Draw/Font.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/Foundation/Strings/Bstring.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <stdarg.h>

#include <Poseidon/UI/Text/ScreenTextLayout.hpp>
#include <stdio.h>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/platform.hpp>

namespace Poseidon
{
void Engine::DrawText(const Point2DAbs& pos, float size, Font* font, PackedColor color, const char* text)
{
    DrawText(pos, size, Rect2DClipAbs, font, color, text);
}

void Engine::DrawText(const Point2DFloat& pos, float size, Font* font, PackedColor color, const char* text)
{
    Rect2DFloat clip(-1000, -1000, 2000, 2000);
    DrawText(pos, size, clip, font, color, text);
}

void Engine::DrawText(const Point2DFloat& pos, float sizeEx, const Rect2DFloat& clip, Font* font, PackedColor color,
                      const char* text)
{
    if (!*text)
    {
        return;
    }

    Point2DAbs posA;
    Convert(posA, pos);
    posA.x = toInt(posA.x) + 0.5f;
    posA.y = toInt(posA.y) + 0.5f;
    Rect2DAbs clipA;
    Convert(clipA, clip);
    DrawText(posA, sizeEx, clipA, font, color, text);
}

void Engine::DrawText(const Point2DAbs& pos, float sizeEx, const Rect2DAbs& clip, Font* font, PackedColor color,
                      const char* text)
{
    if (!font || !text || !*text)
    {
        return;
    }

    if (font->IsFreeType())
    {
        DrawTextFreeType(pos, sizeEx, clip, font, color, text);
        return;
    }

    Point2DAbs posA = pos;
    float h = Height2D();
    AspectSettings as;
    GetAspectSettings(as);
    if (font->Height() <= 0 || font->_nChars <= 0)
        return;
    float sizeH = h * sizeEx * (1.0 / 600) / font->Height();
    float sizeW = sizeH * Width() * as.topFOV / (Height() * as.leftFOV);
    const int spaceChar = 'o' - Font::StartChar;
    // an Arma 3 font has a space of its own (its advance); OFP's fonts use the width of 'o'
    const int spaceWidth = font->_nChars > 0 && font->_infos[0].advance >= 0 ? font->_infos[0].advance
                           : (spaceChar >= 0 && spaceChar < font->_nChars) ? font->_infos[spaceChar].width
                                                                          : 0;

    Draw2DPars pars;
    pars.spec = NoZBuf | IsAlpha | ClampU | ClampV | IsAlphaFog;
    pars.SetColor(color);
    pars.SetU(0, 1);
    pars.SetV(0, 1);

    for (int c; (c = *text++) != 0;)
    {
        c &= 0xff;
        if (c <= Font::StartChar)
        {
            posA.x += toInt(spaceWidth * sizeW);
            continue;
        }

        c -= Font::StartChar;
        if (c < 0 || c >= font->_nChars)
        {
            posA.x += toInt(spaceWidth * sizeW);
            continue;
        }

        // an Arma 3 font's blank or missing glyph (its space) only moves the pen
        const CharInfo& glyph = font->_infos[c];
        if (glyph.advance >= 0 && (glyph.width <= 0 || glyph.height <= 0 || glyph.nameTex.GetLength() == 0))
        {
            posA.x += toInt(glyph.Advance() * sizeW);
            continue;
        }

        Ref<Texture> texture = _fonts.Load(font, font->_infos[c].nameTex);
        PoseidonAssert(texture);

        float tx = font->_infos[c].xTex;
        float ty = font->_infos[c].yTex;
        float th = font->_infos[c].hTex;
        float tw = font->_infos[c].wTex;
        float sh = font->_infos[c].hTex * sizeH;
        float sw = font->_infos[c].wTex * sizeW;

        float ah = font->_infos[c].height;
        float aw = font->_infos[c].width;

        float xPart = aw / tw;
        float yPart = ah / th;

        pars.mip = TextBank()->UseMipmap(texture, 0, 0);
        if (!pars.mip.IsOK())
        {
            continue;
        }
        int texW = 1, texH = 1;
        if (texture)
        {
            texW = texture->AWidth();
            texH = texture->AHeight();
        }

        float invTW = 1.0 / texW;
        float invTH = 1.0 / texH;
        pars.SetU(tx * invTW, (tx + tw * xPart) * invTW);
        pars.SetV(ty * invTH, (ty + th * yPart) * invTH);

        Draw2D(pars,
               Rect2DAbs(posA.x + glyph.offsetX * sizeW, posA.y + glyph.offsetY * sizeH, sw * xPart, sh * yPart),
               clip);

        posA.x += toInt(glyph.Advance() * sizeW);
    }
}

void Engine::Draw3D(Vector3Par pos, Vector3Par down, Vector3Par dir, ClipFlags clip, PackedColor color, int spec,
                    Texture* tex, float x1c, float y1c, float x2c, float y2c)
{
    static Ref<ObjectColored> obj;
    if (!obj)
    {
        Ref<LODShapeWithShadow> lShape = new LODShapeWithShadow();
        Shape* shape = new Shape;
        lShape->AddShape(shape, 0);

        // initalize lod level
        shape->ReallocTable(4);
        const ClipFlags clip = ClipAll;
        shape->SetPos(0) = VZero;
        shape->SetPos(1) = VZero;
        shape->SetPos(2) = VZero;
        shape->SetPos(3) = VZero;
        shape->SetClip(0, clip);
        shape->SetClip(1, clip);
        shape->SetClip(2, clip);
        shape->SetClip(3, clip);
        shape->SetNorm(0) = VUp;
        shape->SetNorm(1) = VUp;
        shape->SetNorm(2) = VUp;
        shape->SetNorm(3) = VUp;
        // precalculate hints for possible optimizations
        shape->CalculateHints();
        lShape->CalculateHints();
        // change face parameters
        Poly face;
        face.Init();
        face.SetN(4);
        face.Set(0, 0);
        face.Set(1, 1);
        face.Set(2, 3);
        face.Set(3, 2);
        face.SetSpecial(ClampU | ClampV);
        shape->AddFace(face);
        shape->OrSpecial(IsAlpha | IsAlphaFog | BestMipmap);

        PoseidonAssert(shape->NPos() == 4);
        PoseidonAssert(shape->NNorm() == 4);
        PoseidonAssert(shape->NFaces() == 1);

        lShape->CalculateMinMax(true);
        lShape->OrSpecial(IsColored | IsOnSurface);

        obj = new ObjectColored(lShape, -1);
        obj->SetOrientation(M3Identity);
    }
    LODShape* lShape = obj->GetShape();

    obj->SetConstantColor(color);
    obj->SetSpecial(spec);
    // use global object
    Shape* shape = lShape->Level(0);
    Poly& face = shape->Face(shape->BeginFaces());

    shape->SetPos(0) = x1c * dir + y1c * down;
    shape->SetPos(1) = x2c * dir + y1c * down;
    shape->SetPos(2) = x1c * dir + y2c * down;
    shape->SetPos(3) = x2c * dir + y2c * down;

    Vector3 normal = down.CrossProduct(dir).Normalized();
    shape->SetNorm(0) = normal;
    shape->SetNorm(1) = normal;
    shape->SetNorm(2) = normal;
    shape->SetNorm(3) = normal;

    lShape->SetAutoCenter(false);
    lShape->CalculateMinMax(true);

    if (tex)
    {
        float u0 = tex->UToPhysical(x1c);
        float u1 = tex->UToPhysical(x2c);
        float v0 = tex->VToPhysical(y1c);
        float v1 = tex->VToPhysical(y2c);
        shape->SetUV(0, u0, v0);
        shape->SetUV(1, u1, v0);
        shape->SetUV(2, u0, v1);
        shape->SetUV(3, u1, v1);
    }
    else
    {
        shape->SetUV(0, 0, 0);
        shape->SetUV(1, 1, 0);
        shape->SetUV(2, 0, 1);
        shape->SetUV(3, 1, 1);
    }

    face.SetTexture(tex);

    shape->FindSections();

    obj->SetPosition(pos);
    obj->Draw(0, clip, *obj);
}

} // namespace Poseidon
#include <Poseidon/World/Scene/ObjLine.hpp>
namespace Poseidon
{

void PrepareTexture(Texture* texture, float z2, int special, float areaOTex);

void Engine::DrawLine3D(Vector3Par start, Vector3Par end, PackedColor color, int spec)
{
    static Ref<LODShapeWithShadow> shape;
    static Ref<ObjectColored> obj;

    if (!shape)
    {
        shape = ObjectLine::CreateShape();
    }

    if (!obj)
    {
        obj = new ObjectLineDiag(shape);
    }
    obj->SetSpecial(spec);
    obj->SetPosition(start);

    // apply color thickness factor to maintain DrawLine3D compatibility
    int a8 = toInt(color.A8() * 1.0 / 10);
    // if line width was non-zero, avoid being it zero now
    if (color.A8() > 0 && a8 <= 0)
    {
        a8 = 1;
    }
    else
    {
        saturate(a8, 0, 255);
    }
    color.SetA8(a8);

    obj->SetConstantColor(color);
    ObjectLine::SetPos(shape, VZero, end - start);
    obj->Draw(0, ClipAll, *obj);
}

void Engine::DrawText3D(Vector3Par pos, Vector3Par up, Vector3Par dir, ClipFlags clip, Font* font, PackedColor color,
                        int spec, const char* text, float x1c, float y1c, float x2c, float y2c)
{
    if (!*text)
    {
        return;
    }

    if (font->IsFreeType())
    {
        DrawTextFreeType3D(pos, up, dir, clip, font, color, spec, text, x1c, y1c, x2c, y2c);
        return;
    }

    static Ref<ObjectColored> objText3D;
    if (!objText3D)
    {
        Ref<LODShapeWithShadow> lShape = new LODShapeWithShadow();
        Shape* shape = new Shape;
        lShape->AddShape(shape, 0);

        shape->ReallocTable(4);
        const ClipFlags clipAll = ClipAll;
        shape->SetClip(0, clipAll);
        shape->SetClip(1, clipAll);
        shape->SetClip(2, clipAll);
        shape->SetClip(3, clipAll);
        shape->SetNorm(0) = VUp;
        shape->SetNorm(1) = VUp;
        shape->SetNorm(2) = VUp;
        shape->SetNorm(3) = VUp;
        shape->CalculateHints();
        lShape->CalculateHints();
        Poly face;
        face.Init();
        face.SetN(4);
        face.Set(0, 0);
        face.Set(1, 1);
        face.Set(2, 3);
        face.Set(3, 2);
        face.SetSpecial(ClampU | ClampV);
        shape->AddFace(face);
        shape->OrSpecial(IsAlpha | IsAlphaFog | NoZWrite);
        PoseidonAssert(shape->NPos() == 4);
        PoseidonAssert(shape->NNorm() == 4);
        PoseidonAssert(shape->NFaces() == 1);

        lShape->OrSpecial(IsColored | IsOnSurface);

        objText3D = new ObjectColored(lShape, -1);
        objText3D->SetOrientation(M3Identity);
    }

    objText3D->SetConstantColor(color);
    objText3D->SetSpecial(spec);
    LODShape* lShape = objText3D->GetShape();
    Shape* shape = lShape->Level(0);
    for (int i = 0; i < shape->NVertex(); i++)
    {
        shape->SetClip(i, clip | ClipAll);
    }
    shape->CalculateHints();

    float z2 = GScene->GetCamera()->Position().Distance2(pos);

    float fontMH = font->_maxHeight;
    float invMH = 1.0 / fontMH;
    float y1cf = y1c * fontMH;
    float y2cf = y2c * fontMH;

    float xPos = 0;
    Vector3 lPos = pos;
    for (int c; (c = *text++) != 0;)
    {
        c &= 0xff;
        if (c <= Font::StartChar)
        {
            const int cc = 'o' - Font::StartChar;
            float w = 0.8 * font->_infos[cc].width * invMH;
            xPos += w;
            lPos += w * dir;
            continue;
        }

        c -= Font::StartChar;

        float wp = font->_infos[c].width * invMH;
        if (xPos + wp >= x1c && xPos <= x2c)
        {
            float x1cf = 0;
            float x2cf = 1;
            if (x1c > xPos)
            {
                x1cf = (x1c - xPos) * (1.0 / wp);
            }
            if (x2c < xPos + wp)
            {
                x2cf = (x2c - xPos) * (1.0 / wp);
            }

            Poly& face = shape->Face(shape->BeginFaces());

            Ref<Texture> texture = _fonts.Load(font, font->_infos[c].nameTex);

            int texW = 1, texH = 1;
            if (texture)
            {
                texW = texture->AWidth();
                texH = texture->AHeight();
            }

            float tx = font->_infos[c].xTex;
            float ty = font->_infos[c].yTex;

            float ah = font->_infos[c].height;
            float aw = font->_infos[c].width;

            float invTW = 1.0 / texW;
            float invTH = 1.0 / texH;

            float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
            if (texture)
            {
                float bottom = floatMin(y2cf, ah);
                u0 = texture->UToPhysical((tx + x1cf * aw) * invTW);
                v0 = texture->VToPhysical((ty + y1cf) * invTH);
                u1 = texture->UToPhysical((tx + x2cf * aw) * invTW);
                v1 = texture->VToPhysical((ty + bottom) * invTH);

                float area = dir.CrossProduct(up).Size();
                float uvArea = (u1 - u0) * (v1 - v0);
                float areaOTex = area / uvArea;

                PrepareTexture(texture, z2, spec, areaOTex);
            }

            face.SetTexture(texture);

            float rh = -floatMin(ah * invMH, y2c);
            float rw = aw * invMH;
            shape->SetPos(0) = dir * (x1cf * rw) - up * y1c;
            shape->SetPos(1) = dir * (x2cf * rw) - up * y1c;
            shape->SetPos(2) = dir * (x1cf * rw) + up * rh;
            shape->SetPos(3) = dir * (x2cf * rw) + up * rh;

            Vector3 normal = dir.CrossProduct(up).Normalized();
            shape->SetNorm(0) = normal;
            shape->SetNorm(1) = normal;
            shape->SetNorm(2) = normal;
            shape->SetNorm(3) = normal;

            shape->SetUV(0, u0, v0);
            shape->SetUV(1, u1, v0);
            shape->SetUV(2, u0, v1);
            shape->SetUV(3, u1, v1);
            shape->FindSections();
            lShape->SetAutoCenter(false);
            lShape->CalculateMinMax(true);
            objText3D->SetPosition(lPos);
            objText3D->Draw(0, clip & ClipAll, *objText3D);
        }
        xPos += wp;
        lPos += wp * dir;
    }
}

Vector3 Engine::GetText3DWidth(Vector3Par dir, Font* font, const char* text)
{
    if (font->IsFreeType())
    {
        return GetText3DWidthFreeType(font, text) * dir;
    }

    float width = 0;
    for (int c; (c = *text++) != 0;)
    {
        c &= 0xff;
        if (c <= Font::StartChar)
        {
            const int cc = 'o' - Font::StartChar;
            width += 0.8 * font->_infos[cc].width;
        }
        else
        {
            const int cc = c - Font::StartChar;
            width += font->_infos[cc].width;
        }
    }
    float invMH = 1.0 / font->_maxHeight;
    return width * invMH * dir;
}

Vector3 CCALL Engine::GetText3DWidthF(Vector3Par dir, Font* font, const char* text, ...)
{
    BString<2048> buf;
    va_list arglist;
    va_start(arglist, text);
    vsprintf(buf, text, arglist);
    va_end(arglist);
    return GetText3DWidth(dir, font, buf);
}

void CCALL Engine::DrawTextF(const Point2DAbs& pos, float size, Font* font, PackedColor color, const char* text, ...)
{
    BString<2048> buf;
    va_list arglist;
    va_start(arglist, text);
    vsprintf(buf, text, arglist);
    va_end(arglist);
    DrawText(pos, size, font, color, buf);
}

void CCALL Engine::DrawTextF(const Point2DFloat& pos, float size, Font* font, PackedColor color, const char* text, ...)
{
    BString<2048> buf;
    va_list arglist;
    va_start(arglist, text);
    vsprintf(buf, text, arglist);
    va_end(arglist);
    DrawText(pos, size, font, color, buf);
}

void Engine::DrawText3DF(Vector3Par pos, Vector3Par up, Vector3Par dir, ClipFlags clip, Font* font, PackedColor color,
                         int spec, const char* text, ...)
{
    BString<2048> buf;
    va_list arglist;
    va_start(arglist, text);
    vsprintf(buf, text, arglist);
    va_end(arglist);
    DrawText3D(pos, up, dir, clip, font, color, spec, buf);
}

void CCALL Engine::DrawTextF(const Point2DFloat& pos, float size, const Rect2DFloat& clip, Font* font,
                             PackedColor color, const char* text, ...)
{
    BString<2048> buf;
    va_list arglist;
    va_start(arglist, text);
    vsprintf(buf, text, arglist);
    va_end(arglist);
    DrawText(pos, size, clip, font, color, buf);
}

float CCALL Engine::GetTextWidthF(float size, Font* font, const char* text, ...)
{
    BString<2048> buf;
    va_list arglist;
    va_start(arglist, text);
    vsprintf(buf, text, arglist);
    va_end(arglist);
    return GetTextWidth(size, font, buf);
}

void Engine::DrawTextVertical(const Point2DFloat& pos, float size, Font* font, PackedColor color, const char* text)
{
    Rect2DFloat clip(-1000, -1000, 2000, 2000);
    DrawTextVertical(pos, size, clip, font, color, text);
}

void Engine::DrawTextVertical(const Point2DFloat& pos, float sizeEx, const Rect2DFloat& clip, Font* font,
                              PackedColor color, const char* text)
{
    Fail("Obsolete, sizeH and sizeW calculation missing");
}

void CCALL Engine::DrawTextVerticalF(const Point2DFloat& pos, float size, Font* font, PackedColor color,
                                     const char* text, ...)
{
    char buf[2048];
    va_list arglist;
    va_start(arglist, text);
    ::vsprintf(buf, text, arglist);
    va_end(arglist);
    DrawTextVertical(pos, size, font, color, buf);
}

void CCALL Engine::DrawTextVerticalF(const Point2DFloat& pos, float size, const Rect2DFloat& clip, Font* font,
                                     PackedColor color, const char* text, ...)
{
    char buf[2048];
    va_list arglist;
    va_start(arglist, text);
    ::vsprintf(buf, text, arglist);
    va_end(arglist);
    DrawTextVertical(pos, size, clip, font, color, buf);
}

float Engine::GetTextWidth(float sizeEx, Font* font, const char* text)
{
    if (!font || !text || font->Height() <= 0)
        return 0.0f;

    float w = Width2D();
    AspectSettings as;
    GetAspectSettings(as);

    ui::ScreenTextBaseScale baseScale;
    if (!ui::ComputeScreenTextBaseScale(Height2D(), Width(), Height(), as.topFOV, as.leftFOV, font->Height(), sizeEx,
                                        &baseScale))
        return 0.0f;

    float sizeH = baseScale.sizeH;
    float sizeW = baseScale.sizeW;
    if (font->IsFreeType())
    {
        Poseidon::ui::FontRenderer* fr = font->FTRenderer();
        if (!fr || font->FTReferencePx() <= 0)
            return 0.0f;

        int pixelSize = font->FTPixelSizeForSizeH(baseScale.sizeH);
        ui::ScreenTextScale screenScale;
        if (!ui::FinalizeFreeTypeScreenTextScale(baseScale, font->FTReferencePx(), pixelSize, font->FTWidthScale(),
                                                 fr->GetAscent(pixelSize) + font->FTBaselineOffset(), &screenScale,
                                                 font->FTLetterSpacing()))
            return 0.0f;

        return ui::MeasureScreenTextWidth(*fr, screenScale, w, text);
    }

    float x = 0;
    const int spaceChar = 'o' - Font::StartChar;
    // an Arma 3 font has a space of its own (its advance); OFP's fonts use the width of 'o'
    const int spaceWidth = font->_nChars > 0 && font->_infos[0].advance >= 0 ? font->_infos[0].advance
                           : (spaceChar >= 0 && spaceChar < font->_nChars) ? font->_infos[spaceChar].width
                                                                          : 0;
    for (int c; (c = *text++) != 0;)
    {
        c &= 0xff;
        if (c <= Font::StartChar)
        {
            x += toInt(spaceWidth * sizeW);
            continue;
        }

        c -= Font::StartChar;
        if (c < 0 || c >= font->_nChars)
        {
            x += toInt(spaceWidth * sizeW);
            continue;
        }
        x += toInt(font->_infos[c].Advance() * sizeW);
    }
    return x / w;
}
} // namespace Poseidon
