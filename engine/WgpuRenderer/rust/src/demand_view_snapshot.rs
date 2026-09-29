//! Optional CPU-only immutable demand-camera provenance. NOT COUNT, visibility,
//! cache publication, GPU completion, pixel quality or geometry retirement proof.
use crate::{ffi::WgrCamera,main_camera_tuple::Selection};
use crate::gfx3d::{cull::MainCountIdentity,view_reference_facts as views};
pub const VERSION:u32=1;
pub const MAX_VIEWS:usize=40;
pub const BYTES:u32=8096;
pub const MAIN:u32=1;
pub const SOLAR:u32=2;
pub const LOCAL:u32=3;
pub const INTERIOR:u32=4;
pub const GI:u32=5;
pub const REFLECTION:u32=6;
pub const NONJITTERED:u32=1;
pub const RETAINED_MATRIX:u32=2;
pub const PLANNED_MATRIX:u32=4;
#[repr(C)]
#[derive(Clone,Copy,Debug,Default,PartialEq)]
pub struct Row {
    pub id:u32,pub kind:u32,pub status:u32,pub provenance:u32,
    pub generation:u64,pub matrix_generation:u64,
    pub viewport_width:u32,pub viewport_height:u32,pub viewport_origin_x:u32,pub viewport_origin_y:u32,
    pub origin:[f32;4],pub projection:[f32;16],pub view:[f32;16],
    pub flags:u32,pub reserved:u32,
}
#[repr(C)]
#[derive(Clone,Copy,Debug,PartialEq)]
pub struct WgrDemandViewSnapshot {
    pub version:u32,pub struct_bytes:u32,pub status:u32,pub view_count:u32,
    pub generation:u64,pub instance_epoch:u64,pub required:u64,pub known:u64,pub unknown:u64,
    pub target_token:u64,pub source_generation:u64,pub target_revision:u64,
    pub model_id:u32,pub source_status:u32,pub main_camera_index:u32,pub main_camera_source:u32,pub rows:[Row;MAX_VIEWS],
}
impl Default for WgrDemandViewSnapshot {
    fn default()->Self {Self{version:VERSION,struct_bytes:BYTES,status:0,view_count:views::VIEWS as u32,
        generation:0,instance_epoch:0,required:0,known:0,unknown:0,target_token:0,source_generation:0,target_revision:0,
        model_id:u32::MAX,source_status:0,main_camera_index:u32::MAX,main_camera_source:0,rows:[Row::default();MAX_VIEWS]}}
}
const _:()=assert!(std::mem::size_of::<Row>()==200);
const _:()=assert!(std::mem::size_of::<WgrDemandViewSnapshot>()==BYTES as usize);
pub fn valid_layout(bytes:u32,version:u32)->bool {bytes==BYTES&&version==VERSION}
pub fn startup_enabled(demand:Option<&str>,fixture:Option<&str>)->bool {demand==Some("1")&&fixture==Some("1")}
// Same enabled-family policy as ApplicabilityPlan, before resource/draw gates.
// Oversize packet counts refuse the ENTIRE set; never silently clamp/drop views.
pub fn required_mask(solar:u32,local:u32,interior:bool,gi:bool,reflection:bool)->Option<u64> {
    if solar>4||local>24{return None;}
    let mut mask=1u64<<views::MAIN;
    mask|=((1u64<<solar)-1)<<views::CASCADES.start;
    mask|=((1u64<<local)-1)<<views::LOCALS.start;
    if interior {mask|=((1u64<<(views::INTERIOR.end-views::INTERIOR.start))-1)<<views::INTERIOR.start;}
    if gi {mask|=1u64<<views::GI;}
    if reflection {mask|=1u64<<views::REFLECTION;}
    Some(mask)
}
pub struct Tracker {
    generation:u64,candidate:Option<WgrDemandViewSnapshot>,completed:Option<WgrDemandViewSnapshot>,captured:u64,
}
const _:()=assert!(std::mem::size_of::<Tracker>()<=2*BYTES as usize+64);
impl Tracker {
    pub fn new()->Self {Self{generation:0,candidate:None,completed:None,captured:0}}
    pub fn begin(&mut self) {
        self.completed=None;self.candidate=None;self.captured=0;self.generation=self.generation.saturating_add(1);
    }
    pub fn configure(&mut self,required:Option<u64>,instance_epoch:u64,source:Option<MainCountIdentity>) {
        let mut out=WgrDemandViewSnapshot{generation:self.generation,instance_epoch,..Default::default()};
        match required.filter(|m|m&!views::VIEW_MASK==0&&m&(1u64<<views::MAIN)!=0) {
            Some(mask)=>{out.required=mask;out.unknown=mask;},
            None=>{out.status=3;out.required=views::VIEW_MASK;out.unknown=views::VIEW_MASK;}
        }
        if self.generation==0||self.generation==u64::MAX||instance_epoch==0||instance_epoch==u64::MAX {out.status=3;}
        if let Some(source)=source.filter(|s|s.token!=0&&s.token!=u64::MAX&&s.cull_epoch==instance_epoch&&
            s.source_generation!=0&&s.source_generation!=u64::MAX&&s.target_revision!=0&&s.model_id!=u32::MAX) {
            out.target_token=source.token;out.source_generation=source.source_generation;
            out.target_revision=source.target_revision;out.model_id=source.model_id;out.source_status=1;
        }
        for i in 0..views::VIEWS {out.rows[i].id=i as u32+1;out.rows[i].generation=self.generation;}
        if self.candidate.is_some(){out.status=3;} // configuration is immutable for one attempt.
        self.candidate=Some(out);
    }
    pub fn capture(&mut self,index:usize,mut row:Row) {
        let Some(out)=self.candidate.as_mut() else{return;};
        if out.status==3{return;}
        if index>=views::VIEWS||out.required&(1u64<<index)==0{return;}
        let bit=1u64<<index;
        if self.captured&bit!=0 {out.known&=!bit;out.unknown|=bit;out.rows[index].status=6;return;}
        self.captured|=bit;row.id=index as u32+1;row.generation=self.generation;row.matrix_generation=self.generation;
        row.reserved=0;
        // 1 numeric tuple known; 2 nonfinite; 3 invalid viewport; 4 missing camera;
        // 5 missing actual matrix/target; 6 duplicate. None of these is a draw fact.
        if row.status==0 {
            row.status=if row.origin.iter().chain(row.projection.iter()).chain(row.view.iter()).any(|v|!v.is_finite()) {2}
                else if row.viewport_width==0||row.viewport_height==0||row.viewport_width>16384||row.viewport_height>16384||
                    row.viewport_origin_x>16384||row.viewport_origin_y>16384 {3}
                else if row.flags&NONJITTERED==0||row.kind==0||row.provenance==0 {5}
                else {1};
        }
        out.rows[index]=row;
        if row.status==1 {out.known|=bit;out.unknown&=!bit;}
    }
    pub fn verify_instance_epoch(&mut self,epoch:u64) {
        if let Some(out)=self.candidate.as_mut() {if out.instance_epoch!=epoch {
            out.status=3;out.known=0;out.unknown=out.required;out.source_status=0;
        }}
    }
    pub fn capture_main(&mut self,selection:Selection,cameras:&[WgrCamera],viewport:(u32,u32)) {
        if let Some(out)=self.candidate.as_mut() {
            out.main_camera_index=u32::try_from(selection.index).unwrap_or(u32::MAX);out.main_camera_source=selection.source;
        }
        let mut row=Row{kind:MAIN,provenance:1,flags:NONJITTERED,
            viewport_width:viewport.0,viewport_height:viewport.1,..Default::default()};
        match cameras.get(selection.index).filter(|_|selection.source>=1&&selection.source<=4) {
            Some(camera)=>{row.origin=[camera.cam_pos[0],camera.cam_pos[1],camera.cam_pos[2],0.];row.projection=camera.proj;row.view=camera.view;},
            None=>row.status=4,
        }
        self.capture(views::MAIN,row);
    }
    pub fn complete(&mut self) {
        if let Some(mut out)=self.candidate.take() {
            if out.status!=3 {out.status=if out.unknown==0 {1}else{2};}
            self.completed=Some(out);
        }
    }
    pub fn abort(&mut self){self.candidate=None;self.completed=None;}
    pub fn snapshot(&self)->Option<&WgrDemandViewSnapshot>{self.completed.as_ref()}
}
pub fn combined_row(kind:u32,provenance:u32,projection:[f32;16],origin:[f32;4],viewport:(u32,u32,u32,u32),flags:u32)->Row {
    let mut view=[0.;16];view[0]=1.;view[5]=1.;view[10]=1.;view[15]=1.;
    Row{kind,provenance,origin,projection,view,viewport_width:viewport.0,viewport_height:viewport.1,
        viewport_origin_x:viewport.2,viewport_origin_y:viewport.3,flags:flags|NONJITTERED,..Default::default()}
}
#[cfg(test)]
mod demand_view_snapshot_tests {
    use super::*;
    fn identity()->MainCountIdentity {MainCountIdentity{token:7,cull_epoch:9,model_id:2,source_generation:11,target_revision:13}}
    fn row()->Row {let mut m=[0.;16];m[0]=1.;m[5]=1.;m[10]=1.;m[15]=1.;combined_row(SOLAR,2,m,[1.,2.,3.,0.],(1024,1024,0,0),0)}
    #[test] fn enabled_masks_match_existing_authoritative_policy_and_refuse_excess_counts() {
        for solar in 0..=4 {for local in 0..=24 {for interior in [false,true] {for gi in [false,true] {for reflection in [false,true] {
            assert_eq!(required_mask(solar,local,interior,gi,reflection),views::ApplicabilityPlan::from_frame(identity(),solar,local,interior,gi,reflection).map(|p|p.required));
        }}}}}
        assert_eq!(required_mask(5,0,false,false,false),None);assert_eq!(required_mask(0,25,false,false,false),None);
        assert_eq!(required_mask(4,24,true,true,true),Some(views::VIEW_MASK));
        assert!(!startup_enabled(None,Some("1")));assert!(!startup_enabled(Some("1"),None));assert!(startup_enabled(Some("1"),Some("1")));
    }
    #[test] fn all_current_views_known_without_count_readbacks_and_exact_bits_viewports_retained() {
        let mut t=Tracker::new();t.begin();t.configure(Some(views::VIEW_MASK),9,Some(identity()));
        for index in 0..views::VIEWS {let mut r=row();r.projection[12]=-0.;r.viewport_origin_x=index as u32;t.capture(index,r);}
        assert!(t.snapshot().is_none());t.complete();let out=t.snapshot().unwrap();
        assert_eq!((out.status,out.required,out.known,out.unknown),(1,views::VIEW_MASK,views::VIEW_MASK,0));
        assert_eq!((out.target_token,out.source_status,out.source_generation),(7,1,11));
        assert_eq!(out.rows[35].viewport_origin_x,35);assert_eq!(out.rows[0].projection[12].to_bits(),(-0.0f32).to_bits());
        assert!(out.rows[36..].iter().all(|r|r==&Row::default()));
    }
    #[test] fn missing_enabled_views_never_disappear_and_nonfinite_duplicate_invalid_viewport_are_unknown() {
        let mut t=Tracker::new();t.begin();t.configure(required_mask(1,0,true,true,true),9,None);
        let mut bad=row();bad.projection[0]=f32::NAN;t.capture(views::MAIN,bad);
        bad=row();bad.viewport_width=0;t.capture(1,bad);
        t.capture(views::INTERIOR.start,row());t.capture(views::INTERIOR.start,row());
        t.complete();let out=t.snapshot().unwrap();assert_eq!(out.status,2);
        assert_eq!(out.known,0);assert_eq!(out.unknown,out.required);assert_eq!(out.source_status,0);
        assert_eq!(out.rows[views::INTERIOR.start].status,6);
    }
    #[test] fn begin_abort_and_refused_source_clear_old_completion() {
        let mut t=Tracker::new();t.begin();t.configure(Some(1),9,Some(identity()));t.capture(0,row());t.complete();assert!(t.snapshot().is_some());
        t.begin();assert!(t.snapshot().is_none());let mut wrong=identity();wrong.cull_epoch+=1;
        t.configure(Some(1),9,Some(wrong));t.capture(0,row());t.abort();assert!(t.snapshot().is_none());
        t.begin();t.configure(Some(1),9,Some(wrong));t.capture(0,row());t.complete();assert_eq!(t.snapshot().unwrap().source_status,0);
        t.begin();t.configure(None,9,None);t.complete();assert_eq!(t.snapshot().unwrap().status,3);
        t.generation=u64::MAX-1;t.begin();t.configure(Some(1),9,None);t.complete();assert_eq!(t.snapshot().unwrap().status,3);
    }
    #[test] fn main_original_camera_and_retained_or_planned_matrix_flags_are_independent_content_evidence() {
        let mut camera:WgrCamera=bytemuck::Zeroable::zeroed();camera.proj=row().projection;camera.view=row().view;camera.cam_pos=[10.,20.,30.,0.];
        let mut t=Tracker::new();t.begin();t.configure(required_mask(0,0,true,true,false),9,None);
        t.capture_main(Selection{index:0,source:2},&[camera],(1600,900));
        let mut retained=row();retained.flags|=RETAINED_MATRIX;t.capture(views::INTERIOR.start,retained);
        let mut planned=row();planned.flags|=PLANNED_MATRIX;t.capture(views::GI,planned);t.complete();
        let out=t.snapshot().unwrap();assert_eq!(out.rows[0].origin,[10.,20.,30.,0.]);assert_eq!(out.rows[0].projection,camera.proj);
        assert_eq!(out.rows[views::INTERIOR.start].flags,NONJITTERED|RETAINED_MATRIX);
        assert_eq!(out.rows[views::GI].flags,NONJITTERED|PLANNED_MATRIX);assert_eq!(out.status,2); // four uncaptured interior directions remain required.
    }
}
