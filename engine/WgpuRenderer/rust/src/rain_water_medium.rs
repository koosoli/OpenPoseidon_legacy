//! Render-only terrain identity. No water mass, geometry or public ABI fields.
use crate::ffi::{WgrRainWaterSourceKey, WgrTerrainParams};

#[derive(Clone, Copy, Debug, PartialEq)]
struct Domain { range: u32, spacing: f32, land_range: u32 }

#[derive(Default)]
pub(crate) struct SourceLifetime {
    world: Option<u64>,
    domain: Option<Domain>,
    source: Option<WgrRainWaterSourceKey>,
    index_ready: bool,
    materials_ready: bool,
    revision: u64,
}
impl SourceLifetime {
    fn bump(&mut self) { self.revision = self.revision.wrapping_add(1); }
    pub fn begin_height(&mut self, p: &WgrTerrainParams, valid: bool) {
        self.source = None;
        self.domain = (valid && p.hm_width >= 2 && p.hm_width == p.hm_height
            && p.terrain_grid.is_finite() && p.terrain_grid > 0.0
            && p.land_grid.is_finite() && p.land_grid > 0.0 && p.land_range > 0
            && p.world_origin == glam::Vec2::ZERO).then_some(Domain {
                range:p.hm_width, spacing:p.terrain_grid, land_range:p.land_range });
        self.bump();
    }
    pub fn set_source(&mut self, s: WgrRainWaterSourceKey) {
        self.source = None;
        if s.world_token == 0 {
            self.world=None;self.domain=None;self.index_ready=false;self.materials_ready=false;self.bump();return;
        }
        if !self.domain.is_some_and(|d|
            d.range == s.terrain_range && d.spacing == s.terrain_spacing) {
            self.bump(); return;
        }
        if self.world != Some(s.world_token) {
            self.index_ready = false;
            self.materials_ready = false;
        }
        self.world = Some(s.world_token);
        self.source = Some(s);
        self.bump();
    }
    pub fn replace_index(&mut self, width:u32, height:u32, valid:bool) {
        self.index_ready = valid && self.source.is_some() && self.domain.is_some_and(|d|
            width == d.land_range && height == d.land_range);
        self.bump();
    }
    pub fn replace_materials(&mut self, valid:bool) {
        self.materials_ready = valid && self.source.is_some();
        self.bump();
    }
    pub fn revision(&self)->u64 { self.revision }
    pub fn control(&self)->[u32;4] {
        match self.source {
            Some(s) if self.index_ready && self.materials_ready =>
                [1,s.generation as u32,(s.generation>>32) as u32,0],
            _ => [0;4],
        }
    }
}

pub(crate) struct Resources<'a> {
    pub params: &'a wgpu::Buffer,
    pub indices: &'a wgpu::Texture,
    pub materials: &'a wgpu::Buffer,
    pub revision: u64,
    pub control: [u32;4],
}

#[cfg(test)]
mod tests {
    use super::*;
    fn fixture()->(WgrTerrainParams,WgrRainWaterSourceKey) {
        let mut p:WgrTerrainParams=bytemuck::Zeroable::zeroed();
        p.hm_width=2048;p.hm_height=2048;p.land_range=256;p.terrain_grid=6.25;p.land_grid=50.;
        let s=WgrRainWaterSourceKey{world_token:11,generation:3,height_revision:2,terrain_range:2048,terrain_spacing:6.25};
        (p,s)
    }
    #[test]
    fn current_world_resources_are_required_and_independently_versioned() {
        let (p,s)=fixture();let mut life=SourceLifetime::default();
        assert_eq!(life.control(),[0;4]);
        life.replace_index(256,256,true);life.replace_materials(true);
        life.begin_height(&p,true);life.set_source(s);
        assert_eq!(life.control(),[0;4],"Pre-source uploads cannot prove current world");
        life.replace_index(256,256,true);assert_eq!(life.control(),[0;4]);
        let a=life.revision();life.replace_materials(true);assert!(life.revision()>a);
        assert_eq!(life.control(),[1,3,0,0]);
        let a=life.revision();life.replace_index(256,256,true);assert!(life.revision()>a);
        life.replace_index(1,1,true);assert_eq!(life.control(),[0;4]);
        life.replace_index(256,256,true);life.replace_materials(false);assert_eq!(life.control(),[0;4]);
    }
    #[test]
    fn same_size_world_swap_never_reuses_old_source_rows() {
        let (p,s)=fixture();let mut life=SourceLifetime::default();
        life.begin_height(&p,true);life.set_source(s);life.replace_index(256,256,true);life.replace_materials(true);
        life.begin_height(&p,true);assert_eq!(life.control(),[0;4]);
        let mut other=s;other.world_token=12;life.set_source(other);
        assert_eq!(life.control(),[0;4]);life.replace_materials(true);assert_eq!(life.control(),[0;4]);
        life.replace_index(256,256,true);assert_eq!(life.control(),[1,3,0,0]);
    }
    #[test]
    fn explicit_world_reset_rejects_reused_pointer_and_same_dimensions() {
        let (p,s)=fixture();let mut life=SourceLifetime::default();
        life.begin_height(&p,true);life.set_source(s);life.replace_index(256,256,true);life.replace_materials(true);
        life.set_source(bytemuck::Zeroable::zeroed());assert_eq!(life.control(),[0;4]);
        life.set_source(s);life.replace_index(256,256,true);life.replace_materials(true);
        assert_eq!(life.control(),[0;4],"A source/table before the new height receipt cannot reuse old geometry");
        life.begin_height(&p,true);life.set_source(s);assert_eq!(life.control(),[0;4]);
        life.replace_index(256,256,true);assert_eq!(life.control(),[0;4]);
        life.replace_materials(true);assert_eq!(life.control(),[1,3,0,0]);
    }
    #[test]
    fn same_world_height_edit_preserves_materials_but_needs_real_source_receipt() {
        let (p,s)=fixture();let mut life=SourceLifetime::default();
        life.begin_height(&p,true);life.set_source(s);life.replace_index(256,256,true);life.replace_materials(true);
        life.begin_height(&p,true);assert_eq!(life.control(),[0;4]);
        let mut edited=s;edited.height_revision+=1;edited.generation=0x100000004;
        life.set_source(edited);assert_eq!(life.control(),[1,4,1,0]);
        edited.terrain_spacing=12.5;life.set_source(edited);assert_eq!(life.control(),[0;4]);
        life.begin_height(&p,false);life.set_source(s);assert_eq!(life.control(),[0;4]);
    }
}
