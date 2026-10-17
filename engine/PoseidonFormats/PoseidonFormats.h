#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef PF_BUILDING_DLL
  #ifdef _WIN32
    #define PF_API __declspec(dllexport)
  #else
    #define PF_API __attribute__((visibility("default")))
  #endif
#else
  #ifdef PF_IMPORTING_DLL
    #define PF_API __declspec(dllimport)
  #else
    #define PF_API
  #endif
#endif

typedef void* PF_HANDLE;

// Library lifecycle
PF_API int         pf_init(void);
PF_API void        pf_shutdown(void);
PF_API const char* pf_version(void);
PF_API const char* pf_last_error(void);
PF_API int         pf_has_safe_appframe(void);
PF_API int         pf_check_null_globals(void);
PF_API int         pf_init_step(int step);

// ── P3D Model ──────────────────────────────────────────────────────────────

PF_API PF_HANDLE   pf_model_load(const char* path);
PF_API void        pf_model_free(PF_HANDLE model);
PF_API const char* pf_model_format(PF_HANDLE model);
PF_API int         pf_model_lod_count(PF_HANDLE model);
PF_API float       pf_model_lod_resolution(PF_HANDLE model, int lod);
PF_API void        pf_model_bounding_center(PF_HANDLE model, float* xyz);
PF_API int         pf_model_autocenter(PF_HANDLE model);

// Geometry
PF_API int  pf_lod_vertex_count(PF_HANDLE model, int lod);
PF_API int  pf_lod_face_count(PF_HANDLE model, int lod);
PF_API void pf_lod_get_vertices(PF_HANDLE model, int lod, float* xyz);
PF_API void pf_lod_get_normals(PF_HANDLE model, int lod, float* xyz);
PF_API void pf_lod_get_uvs(PF_HANDLE model, int lod, float* uv);
PF_API void pf_lod_get_faces(PF_HANDLE model, int lod, int* indices);
PF_API void pf_lod_get_face_materials(PF_HANDLE model, int lod, int* mat_indices);
PF_API void pf_lod_get_vertex_flags(PF_HANDLE model, int lod, uint32_t* flags);
PF_API void pf_lod_get_face_flags(PF_HANDLE model, int lod, uint32_t* flags);

// Materials
PF_API int         pf_lod_material_count(PF_HANDLE model, int lod);
PF_API const char* pf_lod_material_texture(PF_HANDLE model, int lod, int mat);
PF_API const char* pf_lod_material_surface(PF_HANDLE model, int lod, int mat);

// Named selections (vertex groups)
PF_API int         pf_lod_selection_count(PF_HANDLE model, int lod);
PF_API const char* pf_lod_selection_name(PF_HANDLE model, int lod, int sel);
PF_API int         pf_lod_selection_vertex_count(PF_HANDLE model, int lod, int sel);
PF_API void        pf_lod_selection_get_vertices(PF_HANDLE model, int lod, int sel,
                                                  int* indices, float* weights);

// Proxies (attachment points)
PF_API int         pf_lod_proxy_count(PF_HANDLE model, int lod);
PF_API const char* pf_lod_proxy_path(PF_HANDLE model, int lod, int prx);
PF_API void        pf_lod_proxy_transform(PF_HANDLE model, int lod, int prx, float* mat4x4);

// Named properties
PF_API int         pf_lod_property_count(PF_HANDLE model, int lod);
PF_API const char* pf_lod_property_name(PF_HANDLE model, int lod, int prop);
PF_API const char* pf_lod_property_value(PF_HANDLE model, int lod, int prop);

// ── PAA/PAC Image ──────────────────────────────────────────────────────────

PF_API PF_HANDLE   pf_image_load(const char* path);
PF_API void        pf_image_free(PF_HANDLE img);
PF_API int         pf_image_width(PF_HANDLE img);
PF_API int         pf_image_height(PF_HANDLE img);
PF_API const char* pf_image_format(PF_HANDLE img);

/* --- RVMAT material source (AST-014) ---------------------------------------
 * Reads a material into the common source IR and reports its shape. Exposed so
 * the IR can be driven over the whole reference corpus from outside the build,
 * which is the only way to find the cases a hand-written fixture does not
 * contain -- TexGen blocks masquerading as stages were found exactly that way. */
PF_API PF_HANDLE   pf_material_load(const char* path);
PF_API void        pf_material_free(PF_HANDLE mat);
PF_API const char* pf_material_pixel_shader(PF_HANDLE mat);
PF_API int         pf_material_stage_count(PF_HANDLE mat);
PF_API int         pf_material_stage_index(PF_HANDLE mat, int i);
PF_API const char* pf_material_stage_texture(PF_HANDLE mat, int i);
PF_API const char* pf_material_stage_uvsource(PF_HANDLE mat, int i);
PF_API int         pf_material_stage_is_procedural(PF_HANDLE mat, int i);
PF_API int         pf_material_texgen_count(PF_HANDLE mat);
PF_API int         pf_material_other_class_count(PF_HANDLE mat);
PF_API void        pf_image_get_rgba(PF_HANDLE img, uint8_t* buffer);

// ── PBO Archive ────────────────────────────────────────────────────────────

PF_API PF_HANDLE   pf_pbo_open(const char* path);
PF_API void        pf_pbo_close(PF_HANDLE pbo);
PF_API int         pf_pbo_entry_count(PF_HANDLE pbo);
PF_API const char* pf_pbo_entry_name(PF_HANDLE pbo, int idx);
PF_API int         pf_pbo_entry_size(PF_HANDLE pbo, int idx);
PF_API int         pf_pbo_extract(PF_HANDLE pbo, int idx, uint8_t* buffer, int buffer_size);

// ── VFS (Virtual File System) ───────────────────────────────────────────────
// Mount game directory to load textures directly from PBO archives

PF_API int         pf_vfs_mount(const char* game_dir);      // returns PBO count
PF_API void        pf_vfs_unmount(void);
PF_API int         pf_vfs_mounted(void);                     // returns 1 if mounted
PF_API int         pf_vfs_read(const char* vfs_path, uint8_t* buffer, int buffer_size); // read raw bytes (NULL buf = query size)
PF_API PF_HANDLE   pf_vfs_image_load(const char* vfs_path);  // load PAA/PAC from VFS
PF_API PF_HANDLE   pf_vfs_find(const char* extension);       // list files by extension
PF_API int         pf_vfs_find_count(PF_HANDLE list);
PF_API const char* pf_vfs_find_path(PF_HANDLE list, int idx);
PF_API void        pf_vfs_find_free(PF_HANDLE list);

// ── RTM Animation ──────────────────────────────────────────────────────────

PF_API PF_HANDLE   pf_rtm_load(const char* path);
PF_API PF_HANDLE   pf_rtm_load_from_memory(const uint8_t* data, int size);
PF_API void        pf_rtm_free(PF_HANDLE rtm);
PF_API int         pf_rtm_phase_count(PF_HANDLE rtm);
PF_API int         pf_rtm_bone_count(PF_HANDLE rtm);
PF_API void        pf_rtm_step(PF_HANDLE rtm, float* xyz);        // [3]
PF_API const char* pf_rtm_bone_name(PF_HANDLE rtm, int bone);
PF_API float       pf_rtm_phase_time(PF_HANDLE rtm, int phase);
PF_API void        pf_rtm_bone_matrix(PF_HANDLE rtm, int phase, int bone, float* mat4x4); // [16] 4x4

// ── MLOD writing (the engine's MLODWriter, P3DM 28.256) ───────────────────────
// Build a model LOD by LOD, then write it. Positions and normals are model space (x, y up, z). Faces are given in
// FILE order: the loader reverses each face's winding on the way in (0<->1, and 2<->3 on quads), so a face read back
// through pf_lod_get_faces must be turned back the same way before it is written. Every call returns 0 (or -1) on
// failure with pf_last_error set; pf_mlod_write refuses anything the reader would reject or misread.

PF_API PF_HANDLE   pf_mlod_new(void);
PF_API void        pf_mlod_free(PF_HANDLE mlod);
// returns the new LOD's index, or -1. point_flags and normals_xyz may be NULL.
PF_API int         pf_mlod_add_lod(PF_HANDLE mlod, float resolution, int point_count, const float* xyz,
                                   const int32_t* point_flags, int normal_count, const float* normals_xyz);
// per face: vertex_counts[f] (3 or 4); points / normals [4 per face] (normals may be NULL: none); uvs [8 per face];
// flags (may be NULL); texture_index / material_index into the string tables (-1 or NULL: none)
PF_API int         pf_mlod_set_faces(PF_HANDLE mlod, int lod, int face_count, const int32_t* vertex_counts,
                                     const int32_t* points, const int32_t* normals, const float* uvs,
                                     const uint32_t* flags, const int32_t* texture_index,
                                     const char* const* textures, int texture_count,
                                     const int32_t* material_index, const char* const* materials,
                                     int material_count);
// weights: one O2 byte per point (1 = fully selected), may be NULL (all 1)
PF_API int         pf_mlod_add_selection(PF_HANDLE mlod, int lod, const char* name, int point_count,
                                         const int32_t* points, const uint8_t* weights, int face_count,
                                         const int32_t* faces);
PF_API int         pf_mlod_add_property(PF_HANDLE mlod, int lod, const char* name, const char* value);
PF_API int         pf_mlod_set_mass(PF_HANDLE mlod, int lod, const float* mass); // one per point
PF_API int         pf_mlod_write(PF_HANDLE mlod, const char* path);

// ── PAA writing (the engine's PAAEncoder; it picks the format from the alpha) ──
PF_API int         pf_paa_write_rgba(const uint8_t* rgba, int width, int height, const char* path);

#ifdef __cplusplus
}
#endif
