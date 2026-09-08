//! The Milky Way band, as a small equirectangular texture.
//!
//! WHY A TEXTURE AT ALL, when the stars are a catalogue. Points cannot make a continuous glow.
//! The unresolved Milky Way is exactly that: light from stars too faint and too numerous to
//! draw individually, and no amount of catalogue detail produces it.
//!
//! WHY IT IS TINY (1024x512). tools/sky/make_milkyway.py strips the point sources out of the
//! source map before downsampling -- our bright stars come from starcat.rs, and keeping the
//! texture's would draw every one of them twice, once sharp and once as a blurry blob
//! underneath. What survives is diffuse by construction, with no detail finer than the filter
//! radius, so a larger map would carry nothing a 4 MB one does not. That is what makes it
//! affordable to compile in.
//!
//! WHY THERE IS NO IMAGE DECODER HERE. The source is a 36 MB PIZ-compressed EXR. Decoding it
//! would cost real time on EVERY launch to produce a bit-identical result each time. The
//! conversion happens once, offline, and this module reads a header and hands over a slice --
//! which is the pattern tools/ already follows for every other derived asset in this repo.
//!
//! ON-DISK FORMAT (`.pskytex`), written by tools/sky/make_milkyway.py:
//!     0   u8[4]   magic "PSKY"
//!     4   u32 LE  version (1)
//!     8   u32 LE  width
//!     12  u32 LE  height
//!     16  u32 LE  format (1 = RGBA16F equirectangular, +X at u=0, +Y up, LINEAR light)
//!     20  u32 LE  reserved
//!     24  f16[]   width*height*4, row 0 at declination +90
//!
//! Licence: NASA/Goddard SVS "Deep Star Maps 2020", derived from ESA Gaia DR2. Public domain;
//! recorded in THIRD_PARTY_NOTICES.md with the credit NASA asks for.

/// The shipped band, compiled in. Same argument as the star catalogue: a file that must never
/// be missing should not be able to go missing.
const EMBEDDED: &[u8] = include_bytes!("../../../../../resources/sky/milkyway.pskytex");

const HEADER: usize = 24;
const MAGIC: &[u8; 4] = b"PSKY";
const FORMAT_RGBA16F: u32 = 1;

pub struct MilkyWay {
    pub width: u32,
    pub height: u32,
    /// RGBA16F texels, ready for a straight upload.
    pub texels: Vec<u8>,
    /// What was actually loaded, for the log line -- so "the band looks wrong" can be told
    /// apart from "the band is a fallback" without a debugger.
    pub source: String,
}

impl MilkyWay {
    /// A single black texel. Not an error path to be silent about: it renders as no band at
    /// all, which is a visible, diagnosable state rather than a crash or a garbage sky.
    fn fallback(why: &str) -> Self {
        Self {
            width: 1,
            height: 1,
            texels: vec![0u8; 8],
            source: format!("NONE ({why})"),
        }
    }
}

fn parse(bytes: &[u8], source: &str) -> Option<MilkyWay> {
    if bytes.len() < HEADER || &bytes[0..4] != MAGIC {
        return None;
    }
    let u32_at = |o: usize| u32::from_le_bytes([bytes[o], bytes[o + 1], bytes[o + 2], bytes[o + 3]]);
    let version = u32_at(4);
    let width = u32_at(8);
    let height = u32_at(12);
    let format = u32_at(16);
    if version != 1 || format != FORMAT_RGBA16F || width == 0 || height == 0 {
        return None;
    }
    // 4 channels x 2 bytes. Checked rather than assumed: a truncated file would otherwise be a
    // panic inside wgpu's upload, several layers away from the cause.
    let want = (width as usize).checked_mul(height as usize)?.checked_mul(8)?;
    let body = bytes.get(HEADER..HEADER + want)?;
    Some(MilkyWay {
        width,
        height,
        texels: body.to_vec(),
        source: source.to_string(),
    })
}

/// `WGR_MILKYWAY_TEX` overrides the embedded band with a file, so a different or larger map can
/// be tried without a rebuild. Anything that fails to parse falls back to black and says so.
pub fn load() -> MilkyWay {
    if let Ok(p) = std::env::var("WGR_MILKYWAY_TEX") {
        match std::fs::read(&p) {
            Ok(bytes) => match parse(&bytes, &p) {
                Some(mw) => return mw,
                None => eprintln!("[wgr] sky: {p} is not a valid .pskytex file - ignoring"),
            },
            Err(e) => eprintln!("[wgr] sky: cannot read {p}: {e} - ignoring"),
        }
    }
    parse(EMBEDDED, "embedded NASA SVS Deep Star Maps 2020")
        .unwrap_or_else(|| MilkyWay::fallback("the EMBEDDED band failed to parse - build error"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_embedded_band_parses_and_is_the_size_the_tool_writes() {
        let mw = load();
        assert!(mw.source.starts_with("embedded"), "got {}", mw.source);
        assert_eq!(mw.width, 1024);
        assert_eq!(mw.height, 512);
        assert_eq!(mw.texels.len(), 1024 * 512 * 8);
    }

    #[test]
    fn the_band_is_not_black_and_not_saturated() {
        // The point-source strip in make_milkyway.py takes the peak well below 1.0; a band that
        // came back all zeros (a bad convert) or pinned at 1.0 (the strip skipped) would both
        // pass a size check and look wrong only in game.
        let mw = load();
        let mut peak = 0.0f32;
        let mut sum = 0.0f64;
        let mut n = 0u64;
        // RGB only. The first version of this test walked every half in the buffer and failed
        // with "peak 1" -- which was the ALPHA channel, written as a constant 1.0 by the
        // converter. The data was right and the test was wrong, which is the more dangerous of
        // the two ways an assertion can be miscalibrated: it would have sent someone to debug a
        // correct conversion.
        for px in mw.texels.chunks_exact(8) {
            for c in px[..6].chunks_exact(2) {
                let v = half_to_f32(u16::from_le_bytes([c[0], c[1]]));
                peak = peak.max(v);
                sum += v as f64;
                n += 1;
            }
        }
        let mean = (sum / n as f64) as f32;
        assert!(peak > 0.05, "band is black, peak {peak}");
        assert!(peak < 0.95, "band looks unstripped, peak {peak}");
        assert!(mean > 0.0001, "band carries no energy, mean {mean}");
    }

    /// Minimal IEEE half decode, so the test needs no dependency.
    fn half_to_f32(h: u16) -> f32 {
        let sign = ((h >> 15) & 1) as u32;
        let exp = ((h >> 10) & 0x1f) as u32;
        let man = (h & 0x3ff) as u32;
        let bits = match exp {
            0 if man == 0 => sign << 31,
            0 => {
                // subnormal: renormalise
                let mut e = -1i32;
                let mut m = man;
                while m & 0x400 == 0 {
                    m <<= 1;
                    e -= 1;
                }
                ((sign << 31) | (((127 - 15 + e + 1) as u32) << 23) | ((m & 0x3ff) << 13)) as u32
            }
            0x1f => (sign << 31) | (0xff << 23) | (man << 13),
            _ => (sign << 31) | ((exp + 127 - 15) << 23) | (man << 13),
        };
        f32::from_bits(bits)
    }
}
