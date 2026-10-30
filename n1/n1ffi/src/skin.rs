//! Skinned meshes on the CPU side: path -> handle, loading on a background thread (`AssetDb::skeletal`, N7's
//! parser), and each frame's palettes and draw list. Palette = InvRef[b] * SpaceBases[b] * LocalToWorld, as N7
//! (`scenefmt::skel::skin_palette`); bones the game left unset (NaN), or outside the primitive's current LOD (B-265), are
//! rebuilt from the reference pose.

use crate::vkr::{Renderer, SkinDraw, SkinFrame, SkinInst};
use assetdb::{AssetDb, SkelAsset};
use scenekit::{NONE, Resolver};
use std::collections::HashMap;
use std::sync::Arc;
use std::sync::mpsc::{Receiver, Sender, channel};

/// One skinned primitive this frame (C layout: `N1SkelInstance` in n1ffi.h).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SkelInstance {
    /// LocalToWorld, UE3 FMatrix as stored.
    pub transform: [f32; 16],
    /// `n1_skel_mesh` handle.
    pub mesh: u32,
    /// Range in the frame's bone array: SpaceBases (component-space bone matrices), 16 floats each.
    pub bone_offset: u32,
    pub bone_count: u32,
    /// Range in the frame's material handle array, per mesh material slot (N1_NONE keeps the mesh's own).
    pub mat_offset: u32,
    pub mat_count: u32,
    /// Bounds sphere (world): centre and radius, for culling.
    pub bounds: [f32; 4],
    /// `n1_env` handle of the owner's DynamicLightEnvironmentComponent (N1_NONE: sun and sky only).
    pub env: u32,
    /// B-265: the LOD the game posed this frame (`PredictedLODLevel`): only that LOD's `RequiredBones` are current in
    /// SpaceBases. 0 (or a LOD the mesh doesn't have): every bone is taken as given.
    pub lod: u32,
    /// B-277(e): bit 0 = the owner has `bSpecialHighlight` (the engine outlines it in red).
    pub flags: u32,
}

/// `SkelInstance::flags` bit 0 (`bots::outline::kSkelOutline` on the C++ side).
pub const FLAG_OUTLINE: u32 = 1;

enum State {
    Pending {
        requested: bool,
    },
    Ready {
        gpu: u32,
        asset: Arc<SkelAsset>,
        materials: Vec<u32>,
    },
    Missing,
}

pub struct Skins {
    by_path: HashMap<String, u32>,
    paths: Vec<String>,
    states: Vec<State>,
    tx: Sender<(u32, String)>,
    rx: Receiver<(u32, Option<Arc<SkelAsset>>)>,
    pub frame: SkinFrame,
    pub culled: u32,
    pub skipped: u32,
    /// B-199: loads a draw had to ask for, draws that found a preloaded mesh still loading, preloads queued.
    pub lazy: u64,
    pub late: u64,
    pub preloads: u64,
    preloaded: Vec<bool>,
    /// B-287: `build` calls so far, the clock `used` and `boundaries` are on; the clock at each level change.
    clock: u64,
    used: Vec<u64>,
    /// B-287: skinned meshes eviction gave back that nothing has named since: not waiting for anything.
    idle: Vec<bool>,
    boundaries: Vec<u64>,
    /// B-287: skinned meshes given back to the renderer, cumulative.
    pub evicted: u64,
}

impl Skins {
    pub fn new(db: Arc<AssetDb>) -> Self {
        let (tx, jobs) = channel::<(u32, String)>();
        let (done, rx) = channel();
        std::thread::Builder::new()
            .name("n1 skel loader".into())
            .spawn(move || {
                while let Ok((h, path)) = jobs.recv() {
                    if done.send((h, db.skeletal(&path))).is_err() {
                        break;
                    }
                }
            })
            .expect("spawn skel loader");
        Self {
            by_path: HashMap::new(),
            paths: Vec::new(),
            states: Vec::new(),
            tx,
            rx,
            frame: SkinFrame::default(),
            culled: 0,
            skipped: 0,
            lazy: 0,
            late: 0,
            preloads: 0,
            preloaded: Vec::new(),
            clock: 0,
            used: Vec::new(),
            idle: Vec::new(),
            boundaries: Vec::new(),
            evicted: 0,
        }
    }

    pub fn handle(&mut self, path: &str) -> u32 {
        if let Some(&h) = self.by_path.get(path) {
            return h;
        }
        let h = self.states.len() as u32;
        self.by_path.insert(path.to_string(), h);
        self.paths.push(path.to_string());
        self.states.push(State::Pending { requested: false });
        self.preloaded.push(false);
        self.used.push(0);
        self.idle.push(false);
        h
    }

    /// B-199: load a skeletal mesh before anything draws it. Returns true when it queued.
    pub fn preload(&mut self, path: &str) -> bool {
        let h = self.handle(path);
        if let State::Pending { requested } = &mut self.states[h as usize] {
            if !*requested {
                *requested = true;
                self.preloaded[h as usize] = true;
                self.preloads += 1;
                let _ = self.tx.send((h, path.to_string()));
                return true;
            }
        }
        false
    }

    /// B-287: a level change: gives back the skinned meshes nothing named since `keep` levels ago (the handle stays
    /// valid and reloads when named again). `Resolver::level_boundary` is the same rule for static meshes.
    pub fn level_boundary(&mut self, r: &mut Renderer, db: &AssetDb, keep: usize) {
        self.clock += 1;
        self.boundaries.push(self.clock);
        let Some(cutoff) = scenekit::eviction_cutoff(&self.boundaries, keep) else {
            return;
        };
        for (h, state) in self.states.iter_mut().enumerate() {
            if !matches!(state, State::Ready { .. }) || self.used[h] >= cutoff {
                continue;
            }
            if let State::Ready { gpu, .. } = state {
                r.free_skin_mesh(*gpu);
            }
            db.forget_skeletal(&self.paths[h]);
            *state = State::Pending { requested: false };
            self.preloaded[h] = false;
            self.idle[h] = true;
            self.evicted += 1;
        }
    }

    /// Preloaded meshes not loaded yet.
    pub fn preload_pending(&self) -> u32 {
        self.states
            .iter()
            .zip(&self.preloaded)
            .filter(|(s, p)| **p && matches!(s, State::Pending { .. }))
            .count() as u32
    }

    /// (ready, pending, missing)
    pub fn counts(&self) -> (u32, u32, u32) {
        let mut c = (0, 0, 0);
        for (h, s) in self.states.iter().enumerate() {
            match s {
                State::Ready { .. } => c.0 += 1,
                // B-287: one eviction gave back (`idle`: not asked for again yet) isn't waiting for anything.
                State::Pending { requested: false } if self.idle[h] => {}
                State::Pending { .. } => c.1 += 1,
                State::Missing => c.2 += 1,
            }
        }
        c
    }

    /// Takes finished loads and uploads at most `budget` of them.
    pub fn pump(&mut self, r: &mut Renderer, res: &mut Resolver, budget: usize) -> usize {
        self.pump_until(r, res, budget, None)
    }

    /// As `pump`, stopping once `deadline` has passed (after at least one).
    pub fn pump_until(
        &mut self,
        r: &mut Renderer,
        res: &mut Resolver,
        budget: usize,
        deadline: Option<std::time::Instant>,
    ) -> usize {
        let mut n = 0;
        while n < budget {
            if n > 0 && deadline.is_some_and(|d| std::time::Instant::now() >= d) {
                break;
            }
            let Ok((h, asset)) = self.rx.try_recv() else {
                break;
            };
            self.used[h as usize] = self.clock;
            self.states[h as usize] = match asset {
                Some(a) => match r.add_skin_mesh(&a.verts, &a.indices) {
                    Ok(gpu) => {
                        let materials =
                            a.materials.iter().map(|m| res.material_handle(m)).collect();
                        State::Ready {
                            gpu,
                            asset: a,
                            materials,
                        }
                    }
                    Err(e) => {
                        eprintln!("n1ffi: skinned mesh upload failed: {e:#}");
                        State::Missing
                    }
                },
                None => State::Missing,
            };
            n += 1;
        }
        n
    }

    /// Builds this frame's palettes and draws.
    pub fn build(
        &mut self,
        r: &mut Renderer,
        res: &mut Resolver,
        view_proj: &[f32; 16],
        instances: &[SkelInstance],
        bones: &[f32],
        materials: &[u32],
        env_map: &[u32],
    ) {
        let planes = scenekit::frustum_planes(view_proj);
        let f = &mut self.frame;
        f.insts.clear();
        f.bones.clear();
        f.draws.clear();
        self.culled = 0;
        self.skipped = 0;
        for inst in instances {
            let Some(state) = self.states.get_mut(inst.mesh as usize) else {
                continue;
            };
            self.used[inst.mesh as usize] = self.clock;
            self.idle[inst.mesh as usize] = false;
            if let State::Pending { requested } = state {
                if *requested {
                    self.late += self.preloaded[inst.mesh as usize] as u64;
                } else {
                    *requested = true;
                    self.lazy += 1;
                    scenekit::lazy_log("skeletal mesh", &self.paths[inst.mesh as usize]);
                    let _ = self
                        .tx
                        .send((inst.mesh, self.paths[inst.mesh as usize].clone()));
                }
                self.skipped += 1;
                continue;
            }
            let State::Ready {
                gpu,
                asset,
                materials: mesh_mats,
            } = state
            else {
                self.skipped += 1;
                continue;
            };
            let b = inst.bounds;
            if planes
                .iter()
                .any(|p| p[0] * b[0] + p[1] * b[1] + p[2] * b[2] + p[3] < -b[3])
            {
                self.culled += 1;
                continue;
            }
            // Palette.
            let n = asset.parents.len();
            let base = f.bones.len() as u32;
            let required = if r.knobs.lod_bones {
                lod_required(asset, inst.lod)
            } else {
                None
            };
            let sb = space_bases(asset, inst, bones, required);
            for bi in 0..n {
                f.bones.push(scenekit::mul(
                    &scenekit::mul(&asset.inv_ref[bi], &sb[bi]),
                    &inst.transform,
                ));
            }
            let ii = f.insts.len() as u32;
            let env = if inst.env == NONE {
                NONE
            } else {
                env_map.get(inst.env as usize).copied().unwrap_or(NONE)
            };
            f.insts.push(SkinInst {
                bone_base: base,
                env,
                bounds: inst.bounds,
                outline: inst.flags & FLAG_OUTLINE != 0,
            });
            for s in &asset.sections {
                let over = if (s.material as u32) < inst.mat_count {
                    materials
                        .get((inst.mat_offset as usize) + s.material)
                        .copied()
                        .unwrap_or(NONE)
                } else {
                    NONE
                };
                let h = if over != NONE {
                    over
                } else {
                    mesh_mats.get(s.material).copied().unwrap_or(NONE)
                };
                let mut id = res.material_id(r, h);
                if id == scenekit::SKIP {
                    continue;
                }
                if id == NONE || id as usize >= r.materials.len() {
                    id = r.grey_material();
                }
                f.draws.push(SkinDraw {
                    material: id,
                    mesh: *gpu,
                    first_index: s.first_index,
                    index_count: s.index_count,
                    instance: ii,
                });
            }
        }
    }
}

/// The bones current for `lod`: `None` (all of them) for LOD0 or a LOD the mesh doesn't have.
fn lod_required(asset: &SkelAsset, lod: u32) -> Option<&[bool]> {
    if lod == 0 {
        return None;
    }
    asset
        .lod_required
        .get(lod as usize)
        .map(|r| r.as_slice())
        .filter(|r| r.len() == asset.parents.len())
}

/// Component-space bases for every mesh bone: the game's SpaceBases where it computed them this frame (finite, sane,
/// and in `required` when given), else the reference pose relative to the parent's base. Parents precede children
/// in UE3's bone order, so a rebuilt chain (a face, a hand) follows its posed ancestor.
fn space_bases(
    asset: &SkelAsset,
    inst: &SkelInstance,
    bones: &[f32],
    required: Option<&[bool]>,
) -> Vec<[f32; 16]> {
    let n = asset.parents.len();
    let mut sb: Vec<[f32; 16]> = Vec::with_capacity(n);
    for bi in 0..n {
        let given = (bi < inst.bone_count as usize)
            .then(|| {
                let o = (inst.bone_offset as usize + bi) * 16;
                bones
                    .get(o..o + 16)
                    .map(|s| <[f32; 16]>::try_from(s).unwrap())
            })
            .flatten()
            .filter(plausible_bone)
            .filter(|_| required.is_none_or(|r| r[bi]));
        sb.push(match given {
            Some(m) => m,
            None => {
                let (q, t) = asset.ref_local[bi];
                let local = quat_trans(q, t);
                if bi == 0 {
                    local
                } else {
                    scenekit::mul(&local, &sb[asset.parents[bi].max(0) as usize])
                }
            }
        });
    }
    sb
}

/// A component-space bone the game actually computed. UE3 only updates the bones the current LOD needs; the others
/// keep whatever the array held (seen: rotation entries of 1e31 on distant characters), and n1 always draws LOD0, so
/// such bones are rebuilt from the reference pose instead.
fn plausible_bone(m: &[f32; 16]) -> bool {
    m.iter().all(|x| x.is_finite())
        && (0..3).all(|r| m[r * 4..r * 4 + 3].iter().all(|x| x.abs() <= 64.0))
        && m[12..15].iter().all(|x| x.abs() <= 1.0e6)
}

/// UE3 FQuatRotationTranslationMatrix, row-major (as N7's `scenefmt::skel::quat_trans`).
fn quat_trans(q: [f32; 4], t: [f32; 3]) -> [f32; 16] {
    let [x, y, z, w] = q;
    let (x2, y2, z2) = (x + x, y + y, z + z);
    let (xx, xy, xz) = (x * x2, x * y2, x * z2);
    let (yy, yz, zz) = (y * y2, y * z2, z * z2);
    let (wx, wy, wz) = (w * x2, w * y2, w * z2);
    [
        1.0 - (yy + zz),
        xy + wz,
        xz - wy,
        0.0,
        xy - wz,
        1.0 - (xx + zz),
        yz + wx,
        0.0,
        xz + wy,
        yz - wx,
        1.0 - (xx + yy),
        0.0,
        t[0],
        t[1],
        t[2],
        1.0,
    ]
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ident_at(t: [f32; 3]) -> [f32; 16] {
        quat_trans([0.0, 0.0, 0.0, 1.0], t)
    }

    /// Root -> spine -> jaw. LOD1 needs root and spine only (a distant or dead character's face isn't posed).
    fn asset() -> SkelAsset {
        SkelAsset {
            name: "test".into(),
            verts: Vec::new(),
            indices: Vec::new(),
            sections: Vec::new(),
            materials: Vec::new(),
            parents: vec![0, 0, 1],
            ref_local: vec![
                ([0.0, 0.0, 0.0, 1.0], [0.0, 0.0, 0.0]),
                ([0.0, 0.0, 0.0, 1.0], [0.0, 0.0, 100.0]),
                ([0.0, 0.0, 0.0, 1.0], [10.0, 0.0, 0.0]),
            ],
            inv_ref: vec![ident_at([0.0; 3]); 3],
            lod_required: vec![vec![true; 3], vec![true, true, false], vec![true; 2]],
        }
    }

    fn inst(lod: u32) -> SkelInstance {
        SkelInstance {
            transform: ident_at([0.0; 3]),
            mesh: 0,
            bone_offset: 0,
            bone_count: 3,
            mat_offset: 0,
            mat_count: 0,
            bounds: [0.0; 4],
            env: NONE,
            lod,
            flags: 0,
        }
    }

    /// The body fell (spine now lies along x at height 20); the jaw still holds its standing-pose base at z 100.
    fn fallen() -> Vec<f32> {
        [
            ident_at([0.0; 3]),
            ident_at([100.0, 0.0, 20.0]),
            ident_at([10.0, 0.0, 100.0]),
        ]
        .concat()
    }

    #[test]
    fn stale_bone_outside_lod_follows_its_parent() {
        let a = asset();
        let sb = space_bases(&a, &inst(1), &fallen(), lod_required(&a, 1));
        assert_eq!(&sb[1][12..15], &[100.0, 0.0, 20.0]);
        assert_eq!(
            &sb[2][12..15],
            &[110.0, 0.0, 20.0],
            "jaw rebuilt from the posed spine"
        );
    }

    #[test]
    fn lod0_and_switch_off_take_bones_as_given() {
        let a = asset();
        for (lod, req) in [(0, lod_required(&a, 0)), (1, None)] {
            let sb = space_bases(&a, &inst(lod), &fallen(), req);
            assert_eq!(&sb[2][12..15], &[10.0, 0.0, 100.0], "lod {lod}");
        }
    }

    #[test]
    fn unknown_or_malformed_lod_means_all_bones() {
        let a = asset();
        assert!(lod_required(&a, 7).is_none(), "LOD the mesh doesn't have");
        assert!(lod_required(&a, 2).is_none(), "flags not one per bone");
        assert_eq!(lod_required(&a, 1), Some(&[true, true, false][..]));
    }

    #[test]
    fn required_flags_from_indices() {
        assert_eq!(
            assetdb::required_flags(4, &[0, 2, 9]),
            vec![true, false, true, false]
        );
    }
}
