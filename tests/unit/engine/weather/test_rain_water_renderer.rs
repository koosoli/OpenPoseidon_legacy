// Compiled after the UNMODIFIED pure production function declarations from
// rain_water.rs. No wgpu instance or renderer build is required for this check.
#[test]
fn real_renderer_selection_preserves_physical_cells() {
    let p=WgrRainWaterParams {domain:[100.,200.,10.,7.],control:[3.,3.,1.,1.],generation:1,reserved:0};
    let mut cells=vec![[2.,0.,0.,0.];9]; cells[4]=[2.1,0.1,1.,0.];
    let mut next=p;next.generation+=1;
    assert_ne!(grid_identity(&p,0,3,3),grid_identity(&next,0,3,3));
    next=p;next.domain[3]+=100.;next.control[2]=0.;
    assert_eq!(grid_identity(&p,0,3,3),grid_identity(&next,0,3,3)); // pause/time/rain never reallocate cells
    assert!(valid_cells(&cells,3,3));
    assert_eq!(visible_cells(&cells,&p,[110.,3.,210.]),vec![0,1,3,4]);
    let before=cells.clone();
    assert!(visible_cells(&cells,&p,[5000.,3.,5000.]).is_empty());
    assert!(visible_cells(&cells,&p,[f32::MAX,0.,0.]).is_empty());
    assert!(visible_cells(&cells,&p,[f32::NAN,0.,0.]).is_empty());
    assert_eq!(visible_cells(&cells,&p,[110.,3.,210.]),vec![0,1,3,4]);
    assert_eq!(cells,before); // camera return never erases/advances water
    cells[4][1]=-0.1; assert!(!valid_cells(&cells,3,3));
    cells[4][1]=f32::NAN; assert!(!valid_cells(&cells,3,3));
    assert!(!valid_cells(&cells[..8],3,3));
    let mut bad=p;bad.control[0]=3.5;assert!(dimensions(&bad).is_none());
    bad=p;bad.domain[2]=0.;assert!(dimensions(&bad).is_none());
    bad=p;bad.domain[2]=f32::INFINITY;assert!(dimensions(&bad).is_none());
    bad=p;bad.control[0]=1025.;assert!(dimensions(&bad).is_none());
}
#[test]
fn production_camera_bound_and_source_is_not_a_noise_water_height() {
    let p=WgrRainWaterParams {domain:[0.,0.,0.125,10.],control:[1024.,1024.,1.,1.],generation:1,reserved:0};
    let cells=vec![[2.,0.02,0.,0.];MAX_CELLS];
    let selected=visible_cells(&cells,&p,[64.,3.,64.]);
    assert!(!selected.is_empty()&&selected.len()<=MAX_DRAW_CELLS);
    assert!(selected.iter().all(|&i|i as usize+1025<cells.len()));
}
