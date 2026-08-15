use glam::Vec3;

/// A benchmark viewpoint. Height cases matter more than anything else here: an
/// aircraft view puts an enormous amount of world on screen and is the case that
/// breaks naive far-field schemes.
#[derive(Clone, Copy)]
pub struct View {
    pub eye: Vec3,
    pub yaw: f32,
    pub pitch: f32,
    pub fov_y: f32,
}

pub struct Basis {
    pub fwd: Vec3,
    pub right: Vec3,
    pub up: Vec3,
}

impl View {
    pub fn basis(&self) -> Basis {
        let (sy, cy) = self.yaw.sin_cos();
        let (sp, cp) = self.pitch.sin_cos();
        let fwd = Vec3::new(cp * sy, sp, cp * cy).normalize();
        let right = fwd.cross(Vec3::Y).normalize();
        let up = right.cross(fwd).normalize();
        Basis { fwd, right, up }
    }
    pub fn tan_half_fov(&self) -> f32 {
        (self.fov_y * 0.5).tan()
    }
}

/// The four standard camera heights, placed at the middle of the world and aimed
/// at the horizon with a slight downward tilt appropriate to the altitude.
pub fn standard_views(extent: f32, ground_h: f32) -> Vec<(&'static str, View)> {
    let c = extent * 0.5;
    let mk = |name: &'static str, alt: f32, pitch_deg: f32| {
        (
            name,
            View {
                // Placed a sixth of the way in along +Z and aimed down +Z, so even
                // the aircraft view (which looks near the horizon and therefore sees
                // ground only from ~6 km out) has world in front of it rather than
                // off-map sky.
                eye: Vec3::new(c, ground_h + alt, extent * 0.15),
                yaw: 0.0,
                pitch: pitch_deg.to_radians(),
                fov_y: 55.0f32.to_radians(),
            },
        )
    };
    vec![
        mk("ground", 2.0, -1.0),
        mk("hill", 300.0, -3.0),
        mk("helicopter", 1000.0, -8.0),
        mk("aircraft", 5000.0, -14.0),
    ]
}
