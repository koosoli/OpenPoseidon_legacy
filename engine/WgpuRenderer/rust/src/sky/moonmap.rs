//! The whole world's moon surface: NASA LRO Wide Angle Camera global mosaic, an equirectangular R8
//! map with its mip chain (assets/earth/moon/moon_albedo.pmoon, written by tools/sky/make_moon_albedo.py,
//! which documents the format and the source). Credit: NASA/GSFC/Arizona State University.
//!
//! Read from the game folder, and only in a whole-world process (POSEIDON_WHOLE_WORLD=1): the island
//! game binds a 1x1 placeholder and its sky shader has no moon-body code at all (shaders::shader_defs).
//! A missing or broken file is the same placeholder, said so in the log; the moon then falls back to the
//! shader's procedural maria.

const HEADER: usize = 24;
/// The mean texel value after loading (sky.wgsl's MOON_MAP_MEAN = MEAN / 255). Below mid-grey so the
/// bright highlands and ray craters keep their range instead of clipping.
pub const MEAN: u32 = 96;

pub struct MoonMap {
    pub width: u32,
    pub height: u32,
    /// Mean of level 0 (0..255) as written. On load every level is rescaled so the mean lands on
    /// `MEAN`, which is the constant sky.wgsl divides by (MOON_MAP_MEAN): the map is relative albedo.
    pub mean: u32,
    /// Each mip level's texels, level 0 first.
    pub levels: Vec<Vec<u8>>,
    pub source: String,
}

impl MoonMap {
    fn placeholder(why: &str) -> Self {
        Self { width: 1, height: 1, mean: 128, levels: vec![vec![MEAN as u8]], source: format!("NONE ({why})") }
    }
}

fn parse(bytes: &[u8], source: &str) -> Option<MoonMap> {
    if bytes.len() < HEADER || &bytes[0..4] != b"PMON" {
        return None;
    }
    let u32_at = |o: usize| u32::from_le_bytes([bytes[o], bytes[o + 1], bytes[o + 2], bytes[o + 3]]);
    let (version, width, height, mips, mean) = (u32_at(4), u32_at(8), u32_at(12), u32_at(16), u32_at(20));
    if version != 1 || width == 0 || height == 0 || mips == 0 || mips > 16 || mean == 0 {
        return None;
    }
    let mut levels = Vec::with_capacity(mips as usize);
    let mut at = HEADER;
    for i in 0..mips {
        let (w, h) = ((width >> i).max(1) as usize, (height >> i).max(1) as usize);
        let body = bytes.get(at..at + w * h)?;
        levels.push(body.iter().map(|&v| (v as u32 * MEAN / mean).min(255) as u8).collect());
        at += w * h;
    }
    Some(MoonMap { width, height, mean, levels, source: source.to_string() })
}

pub fn load() -> MoonMap {
    if !cfg!(test) && std::env::var("POSEIDON_WHOLE_WORLD").as_deref() != Ok("1") {
        return MoonMap::placeholder("not a whole-world process");
    }
    let path = match std::env::var("WGR_MOON_MAP") {
        Ok(p) => std::path::PathBuf::from(p),
        Err(_) => std::env::current_exe()
            .ok()
            .and_then(|p| p.parent().map(|d| d.to_path_buf()))
            .unwrap_or_else(|| std::path::PathBuf::from("."))
            .join("assets")
            .join("earth")
            .join("moon")
            .join("moon_albedo.pmoon"),
    };
    let shown = path.display().to_string();
    match std::fs::read(&path) {
        Ok(bytes) => parse(&bytes, &shown).unwrap_or_else(|| MoonMap::placeholder(&format!("{shown} is not a valid .pmoon"))),
        Err(e) => MoonMap::placeholder(&format!("cannot read {shown}: {e}")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_written_map_parses_with_its_mip_chain() {
        let mut bytes = b"PMON".to_vec();
        for v in [1u32, 4, 2, 3, 100] {
            bytes.extend_from_slice(&v.to_le_bytes());
        }
        bytes.extend_from_slice(&[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]);
        let m = parse(&bytes, "t").expect("parses");
        assert_eq!((m.width, m.height, m.mean), (4, 2, 100));
        assert_eq!(m.levels.iter().map(Vec::len).collect::<Vec<_>>(), vec![8, 2, 1]);
        assert_eq!(m.levels[2], vec![(11 * MEAN / 100) as u8], "rescaled to the shader's mean");
        assert!(parse(&bytes[..bytes.len() - 1], "t").is_none(), "a truncated chain must not parse");
    }
}

/// The moon's topography: NASA LRO LOLA LDEM_16 resampled to an equirectangular height map with its mip
/// chain (assets/earth/moon/moon_height.pmoh, tools/sky/make_moon_relief.py documents the format and the
/// source), uploaded as R16Float KILOMETRES above the 1737.4 km reference sphere (~8 m steps at 10 km;
/// 16-bit unorm is not filterable without an optional feature). Same placeholder rules as the albedo map:
/// a 1x1 zero when it is not there (a smooth sphere).
pub struct MoonRelief {
    pub width: u32,
    pub height: u32,
    /// Each level as little-endian f16 kilometres, level 0 first.
    pub levels: Vec<Vec<u8>>,
    /// Lowest and highest height (metres), for bounding the ray march.
    pub range: [f32; 2],
    pub source: String,
}

fn f32_to_f16_bits(v: f32) -> u16 {
    let x = v.to_bits();
    let sign = ((x >> 16) & 0x8000) as u16;
    let exp = ((x >> 23) & 0xff) as i32 - 127 + 15;
    let mant = x & 0x7f_ffff;
    if exp <= 0 {
        if exp < -10 {
            return sign;
        }
        let m = (mant | 0x80_0000) >> (1 - exp);
        return sign | ((m + 0x1000) >> 13) as u16;
    }
    if exp >= 31 {
        return sign | 0x7c00;
    }
    let rounded = ((exp as u32) << 10 | (mant >> 13)) + ((mant >> 12) & 1);
    sign | rounded as u16
}

fn parse_relief(bytes: &[u8], source: &str) -> Option<MoonRelief> {
    const HEAD: usize = 28;
    if bytes.len() < HEAD || &bytes[0..4] != b"PMOH" {
        return None;
    }
    let u32_at = |o: usize| u32::from_le_bytes([bytes[o], bytes[o + 1], bytes[o + 2], bytes[o + 3]]);
    let f32_at = |o: usize| f32::from_le_bytes([bytes[o], bytes[o + 1], bytes[o + 2], bytes[o + 3]]);
    let (version, width, height, mips) = (u32_at(4), u32_at(8), u32_at(12), u32_at(16));
    let (lo, hi) = (f32_at(20), f32_at(24));
    if version != 1 || width == 0 || height == 0 || mips == 0 || mips > 16 || !(hi > lo) {
        return None;
    }
    let mut levels = Vec::with_capacity(mips as usize);
    let mut at = HEAD;
    for i in 0..mips {
        let (w, h) = ((width >> i).max(1) as usize, (height >> i).max(1) as usize);
        let body = bytes.get(at..at + w * h * 2)?;
        let mut level = Vec::with_capacity(w * h * 2);
        for q in body.chunks_exact(2) {
            let metres = lo + (hi - lo) * (u16::from_le_bytes([q[0], q[1]]) as f32 / 65535.0);
            level.extend_from_slice(&f32_to_f16_bits(metres / 1000.0).to_le_bytes());
        }
        levels.push(level);
        at += w * h * 2;
    }
    Some(MoonRelief { width, height, levels, range: [lo, hi], source: source.to_string() })
}

pub fn load_relief() -> MoonRelief {
    let placeholder = |why: String| MoonRelief { width: 1, height: 1, levels: vec![vec![0, 0]], range: [0.0, 0.0], source: format!("NONE ({why})") };
    if !cfg!(test) && std::env::var("POSEIDON_WHOLE_WORLD").as_deref() != Ok("1") {
        return placeholder("not a whole-world process".into());
    }
    let path = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.to_path_buf()))
        .unwrap_or_else(|| std::path::PathBuf::from("."))
        .join("assets")
        .join("earth")
        .join("moon")
        .join("moon_height.pmoh");
    let shown = path.display().to_string();
    match std::fs::read(&path) {
        Ok(bytes) => parse_relief(&bytes, &shown).unwrap_or_else(|| placeholder(format!("{shown} is not a valid .pmoh"))),
        Err(e) => placeholder(format!("cannot read {shown}: {e}")),
    }
}

#[cfg(test)]
mod relief_tests {
    use super::*;

    #[test]
    fn half_floats_round_trip_the_heights_that_matter() {
        for km in [0.0f32, 1.5, -8.962, 10.625, 0.001] {
            let bits = f32_to_f16_bits(km);
            let back = half_to_f32(bits);
            assert!((back - km).abs() <= km.abs() * 1.0e-3 + 1.0e-4, "{km} -> {back}");
        }
    }

    fn half_to_f32(h: u16) -> f32 {
        let s = if h & 0x8000 != 0 { -1.0 } else { 1.0 };
        let e = ((h >> 10) & 0x1f) as i32;
        let m = (h & 0x3ff) as f32;
        if e == 0 {
            return s * m * 2f32.powi(-24);
        }
        s * (1.0 + m / 1024.0) * 2f32.powi(e - 15)
    }
}
