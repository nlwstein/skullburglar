// Shared by the scene shader and the skinning shader (N10). Needs `envs: array<vec4<f32>>` declared by the includer.
// UE3 point light: pow(saturate(1 - (d/radius)^2), FalloffExponent); spot adds a squared cone ramp.
fn point_term(a: vec4<f32>, b: vec4<f32>, wpos: vec3<f32>, N: vec3<f32>) -> vec3<f32> {
    let d = a.xyz - wpos;
    let dist2 = dot(d, d);
    let x = dist2 * a.w * a.w;
    if (x >= 1.0) { return vec3<f32>(0.0); }
    let L = d * inverseSqrt(max(dist2, 1e-6));
    let w = 1.0 - x;
    var att = w * w;
    if (b.w > 0.0) { att = pow(w, b.w); }
    return b.rgb * att * max(dot(N, L), 0.0);
}

// The light environment composite UE3 gives a dynamic object: one directional, one point, one SH light.
// B-189 splits it: the port draws the directional (and point) passes before its modulated shadows and the SH pass
// after them, so only `env_key` is shadowed.
fn env_key(e: u32, wpos: vec3<f32>, N: vec3<f32>) -> vec3<f32> {
    let base = 13u * e;
    let dd = envs[base];
    let acc = envs[base + 1u].rgb * dd.w * max(dot(N, dd.xyz), 0.0);
    return acc + point_term(envs[base + 2u], envs[base + 3u], wpos, N);
}

fn env_sh(e: u32, N: vec3<f32>) -> vec3<f32> {
    let base = 13u * e;
    // SH basis in UE3 axes: (x, y, z) = (X, Z, Y) of our Y-up world. Band weights are UE3's Lambert diffuse transfer
    // (pi, 2pi/3, pi/4): W4 read them off the port's SH light pass (0.886228 = 0.282095 pi, 2.0944, 0.785398).
    let x = N.x; let y = N.z; let z = N.y;
    var sh = envs[base + 4u].rgb * (0.886227);
    sh = sh + envs[base + 5u].rgb * (-0.488603 * y * 2.0943951);
    sh = sh + envs[base + 6u].rgb * (0.488603 * z * 2.0943951);
    sh = sh + envs[base + 7u].rgb * (-0.488603 * x * 2.0943951);
    sh = sh + envs[base + 8u].rgb * (1.092548 * x * y * 0.7853982);
    sh = sh + envs[base + 9u].rgb * (-1.092548 * y * z * 0.7853982);
    sh = sh + envs[base + 10u].rgb * (0.315392 * (3.0 * z * z - 1.0) * 0.7853982);
    sh = sh + envs[base + 11u].rgb * (-1.092548 * x * z * 0.7853982);
    sh = sh + envs[base + 12u].rgb * (0.546274 * (x * x - y * y) * 0.7853982);
    return max(sh, vec3<f32>(0.0));
}

fn env_light(e: u32, wpos: vec3<f32>, N: vec3<f32>) -> vec3<f32> {
    return env_key(e, wpos, N) + env_sh(e, N);
}
