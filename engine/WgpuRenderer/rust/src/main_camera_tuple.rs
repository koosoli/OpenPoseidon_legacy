//! Optional completed-main-frame NONJITTERED camera provenance. No readback/pixels/quality proof.
use crate::ffi::WgrCamera;
pub const VERSION: u32 = 1;
pub const BYTES: u32 = 192;
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct WgrMainCameraTuple {
    pub version:u32, pub struct_bytes:u32, pub status:u32, pub camera_index:u32,
    pub generation:u64,
    pub source:u32, pub render_width:u32, pub render_height:u32,
    pub output_width:u32, pub output_height:u32, pub reserved:u32,
    pub camera_position:[f32;4], pub projection:[f32;16], pub view:[f32;16],
}
const _: () = assert!(std::mem::size_of::<WgrMainCameraTuple>() == BYTES as usize);
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Selection { pub index:usize, pub source:u32 }
// Production rendering keeps its existing front-entry priority and default index0.
// Diagnostic refuses source0 rather than certifying a camera with no actual main batch.
pub fn select(terrain:Option<usize>,draw:Option<usize>,water:Option<usize>,grass:Option<usize>)->Selection {
    if let Some(index)=terrain{return Selection{index,source:1};}
    if let Some(index)=draw{return Selection{index,source:2};}
    if let Some(index)=water{return Selection{index,source:3};}
    if let Some(index)=grass{return Selection{index,source:4};}
    Selection{index:0,source:0}
}
// Status:1 captured,2 no main source/index,3 nonfinite,4 invalid viewport,5 not1:1,6 serial exhausted.
// Only complete() publishes. begin() clears prior completion even if this render later fails.
pub struct Tracker { generation:u64, candidate:Option<WgrMainCameraTuple>, completed:Option<WgrMainCameraTuple> }
const _: () = assert!(std::mem::size_of::<Tracker>() <= 2 * BYTES as usize + 32);
impl Tracker {
    pub fn new()->Self {Self{generation:0,candidate:None,completed:None}}
    pub fn begin(&mut self) {self.completed=None;self.candidate=None;self.generation=self.generation.saturating_add(1);}
    pub fn capture(&mut self,s:Selection,cameras:&[WgrCamera],render:(u32,u32),output:(u32,u32)) {
        let mut tuple=WgrMainCameraTuple{version:VERSION,struct_bytes:BYTES,status:1,
            camera_index:u32::try_from(s.index).unwrap_or(u32::MAX),generation:self.generation,source:s.source,
            render_width:render.0,render_height:render.1,output_width:output.0,output_height:output.1,..Default::default()};
        if self.generation==0||self.generation==u64::MAX {tuple.status=6;}
        else if s.source==0||s.source>4||s.index>=cameras.len(){tuple.status=2;}
        else if render.0==0||render.1==0||render.0>16384||render.1>16384||output.0==0||output.1==0||output.0>16384||output.1>16384{tuple.status=4;}
        else if render!=output{tuple.status=5;}
        else {
            let camera=&cameras[s.index];tuple.projection=camera.proj;tuple.view=camera.view;
            tuple.camera_position=[camera.cam_pos[0],camera.cam_pos[1],camera.cam_pos[2],0.0];
            if tuple.projection.iter().chain(tuple.view.iter()).chain(tuple.camera_position.iter()).any(|v|!v.is_finite()){tuple.status=3;}
        }
        self.candidate=Some(tuple);
    }
    pub fn complete(&mut self){self.completed=self.candidate.take();}
    pub fn snapshot(&self)->Option<WgrMainCameraTuple>{self.completed}
}
#[cfg(test)]
mod tests {
    use super::*;
    fn camera()->WgrCamera {
        let mut c:WgrCamera=bytemuck::Zeroable::zeroed();
        c.proj[0]=1.;c.proj[5]=2.;c.proj[10]=1.;c.proj[11]=1.;c.proj[14]=-0.1;
        c.view[0]=1.;c.view[5]=1.;c.view[10]=1.;c.view[15]=1.;
        c.cam_pos=[10.,20.,30.,0.];c
    }
    #[test] fn front_priority_and_invalid_first_are_not_relabelled() {
        assert_eq!(select(Some(9),Some(0),Some(1),Some(2)),Selection{index:9,source:1});
        assert_eq!(select(None,Some(3),Some(1),Some(2)),Selection{index:3,source:2});
        assert_eq!(select(None,None,Some(1),Some(2)),Selection{index:1,source:3});
        assert_eq!(select(None,None,None,Some(2)),Selection{index:2,source:4});
        let mut t=Tracker::new();t.begin();t.capture(select(Some(9),Some(0),None,None),&[camera()],(640,480),(640,480));t.complete();assert_eq!(t.snapshot().unwrap().status,2);
        t.begin();t.capture(select(None,None,None,None),&[camera()],(640,480),(640,480));t.complete();assert_eq!(t.snapshot().unwrap().status,2);
    }
    #[test] fn actual_matrix_bits_remain_original_before_prepared_jitter() {
        let mut original=camera();original.proj[2]=-0.0;
        let mut t=Tracker::new();t.begin();t.capture(select(None,Some(0),None,None),&[original],(640,480),(640,480));
        let mut prepared=original;prepared.proj[8]=0.125;assert!(t.snapshot().is_none());t.complete();
        let snapshot=t.snapshot().unwrap();assert_eq!(snapshot.status,1);assert_eq!(snapshot.projection[2].to_bits(),(-0.0f32).to_bits());
        assert_eq!(snapshot.projection[8],0.);assert_ne!(snapshot.projection[8],prepared.proj[8]);assert_eq!(snapshot.camera_position,[10.,20.,30.,0.]);
    }
    #[test] fn incomplete_failed_call_never_exposes_previous_success() {
        let mut t=Tracker::new();t.begin();t.capture(select(Some(0),None,None,None),&[camera()],(640,480),(640,480));t.complete();let old=t.snapshot().unwrap();
        t.begin();assert!(t.snapshot().is_none());t.capture(select(Some(0),None,None,None),&[camera()],(640,480),(640,480));assert!(t.snapshot().is_none());
        t.complete();assert!(t.snapshot().unwrap().generation>old.generation);
    }
    #[test] fn viewport_scale_nonfinite_and_serial_refusals() {
        let mut t=Tracker::new();let selection=select(Some(0),None,None,None);
        for (render,output,status) in [((0,480),(640,480),4),((640,480),(1280,960),5),((16385,480),(16385,480),4)] {
            t.begin();t.capture(selection,&[camera()],render,output);t.complete();assert_eq!(t.snapshot().unwrap().status,status);
        }
        let mut bad=camera();bad.view[3]=f32::NAN;t.begin();t.capture(selection,&[bad],(640,480),(640,480));t.complete();assert_eq!(t.snapshot().unwrap().status,3);
        t.generation=u64::MAX-1;t.begin();t.capture(selection,&[camera()],(640,480),(640,480));t.complete();assert_eq!(t.snapshot().unwrap().status,6);
    }
}
