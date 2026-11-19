// Fog in the scene's Y-up world. Mode: 0 opaque, 1 additive, 2 modulate.
fn apply_fog(col0: vec3<f32>, wpos: vec3<f32>, mode: u32) -> vec3<f32> {
    var col = col0;
    // height fog, as UE3's (W4, decoded from the port's fog pass): a layer below the fog height; the fogged length
    // is the part of the camera ray inside it past the start distance, along view depth.
    if (g.fog_col.w > 0.5) {
        let fog_color = select(select(g.fog_col.rgb, vec3<f32>(0.0), mode == 1u), vec3<f32>(1.0), mode == 2u);
        let vz = (g.view_proj * vec4<f32>(wpos, 1.0)).w;
        if (vz > g.fog_par.z) {
            var dz = wpos.y - g.cam_pos.y;
            if (abs(dz) <= 0.001) { dz = 0.001; }
            let fr = abs(clamp((g.fog_par.y - g.cam_pos.y) / dz, 0.0, 1.0) - clamp((-1.0e30 - g.cam_pos.y) / dz, 0.0, 1.0));
            let dist = (vz - g.fog_par.z) * fr;
            var t = exp(-g.fog_par.x * dist);
            if (g.fog_par.w > 0.0 && dist >= g.fog_par.w) { t = 0.0; }
            col = mix(fog_color, col, t);
        }
    }
    // B-310: fog volumes, analytic along the camera ray clipped to each volume: tau = density * length inside,
    // composited as scene * exp(-tau) + colour * (1 - exp(-tau)).
    let ro = g.cam_pos.xyz;
    let rd = wpos - ro;
    // The port's volume pass (B-310, decoded from the capture) integrates density over PLANAR view depth between the
    // volume's near and far faces, clamped to the scene: tau = density * dz_view, so the ray length is scaled by
    // the fragment's view depth (not its Euclidean distance).
    let rl = max((g.view_proj * vec4<f32>(wpos, 1.0)).w, 0.0);
    let nv = u32(g.vol_n.x);
    for (var k = 0u; k < nv; k = k + 1u) {
        let a = g.vols[3u * k];
        let b = g.vols[3u * k + 1u];
        let vc = g.vols[3u * k + 2u].rgb;
        let fog_color = select(select(vc, vec3<f32>(0.0), mode == 1u), vec3<f32>(1.0), mode == 2u);
        var t0 = 0.0;
        var t1 = 1.0;
        if (a.w < 0.5) {
            let inv = vec3<f32>(1.0) / select(rd, vec3<f32>(1.0e-6), abs(rd) < vec3<f32>(1.0e-6));
            let ta = (a.xyz - b.xyz - ro) * inv;
            let tb = (a.xyz + b.xyz - ro) * inv;
            let lo = min(ta, tb);
            let hi = max(ta, tb);
            t0 = max(0.0, max(lo.x, max(lo.y, lo.z)));
            t1 = min(1.0, min(hi.x, min(hi.y, hi.z)));
        } else {
            let oc = ro - a.xyz;
            let qa = dot(rd, rd);
            let qb = dot(oc, rd);
            let qc = dot(oc, oc) - b.x * b.x;
            let disc = qb * qb - qa * qc;
            if (disc > 0.0) {
                let sq = sqrt(disc);
                t0 = max(0.0, (-qb - sq) / qa);
                t1 = min(1.0, (-qb + sq) / qa);
            } else {
                t1 = -1.0;
            }
        }
        if (t1 > t0) {
            let tr = exp(-b.w * (t1 - t0) * rl);
            col = mix(fog_color, col, tr);
        }
    }
    return col;
}
