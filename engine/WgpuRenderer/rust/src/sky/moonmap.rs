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
