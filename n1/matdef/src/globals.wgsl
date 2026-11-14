struct Globals {
    view_proj: mat4x4<f32>,
    cam_pos: vec4<f32>,   // xyz world (Y-up), w = time in seconds
    sun_dir: vec4<f32>,   // xyz = direction TO the sun, w = 1 if present
    sun_col: vec4<f32>,   // rgb = colour * brightness, w = B-205 scale on the env key's specular
    amb_up: vec4<f32>,    // w: B-244 dynamic-light flags (bit 0 after the modulated shadows, bit 1 per-light LightVector, bit 2 specular); B-245 bit 3: env key specular as the port's
    amb_dn: vec4<f32>,
    fog_col: vec4<f32>,   // rgb, w = enabled
    fog_par: vec4<f32>,   // x density, y fog height (world Y), z start distance, w extinction distance (<= 0: none)
    tune: vec4<f32>,      // x lightmap gain, y spec gain, z vertex-light gain, w debug mode
    tune2: vec4<f32>,     // x vertex colour channel map, y sun-visibility luma, z normal-map strength, w unused
    tune3: vec4<f32>,     // N10: x light-environment gain, y ambient kept under an environment, z dynamic light gain, w vertex-lightmap gamma (B-189)
    vol_n: vec4<f32>,     // B-310: x = fog volume count; B-327: y = near plane, z = soft edges on, w = planar PixelDepth
    vols: array<vec4<f32>, 24>,  // per volume: (center, kind), (half extents or radius, density), (colour, 0)
    dsky_up: vec4<f32>,   // B-328: w = 1 when the frame has a dynamic-object sky
    dsky_dn: vec4<f32>,
};

@group(0) @binding(0) var<uniform> g: Globals;
