//! BC (DXT) blocks to plain texels, for GPUs without the BC formats (most Android GPUs, among them the Quest 3's
//! Adreno). The same decoder as the pre-1.0 renderer's (src/gfx/vk/texture.cpp): BC1/BC2/BC3 to RGBA8 and BC5 to RG8.
//! The texels keep their encoding, so an sRGB texture decodes to an `_SRGB` format and the sampler still converts.

use scenekit::TexFormat;

/// Bytes per decoded texel.
pub fn decoded_bpp(f: TexFormat) -> usize {
    match f {
        TexFormat::Bc5 => 2,
        _ => 4,
    }
}

/// A BC4 block (also BC3's alpha and each BC5 channel): 16 values written `stride` bytes apart.
fn bc4(block: &[u8], out: &mut [u8], stride: usize) {
    let (a0, a1) = (block[0] as u32, block[1] as u32);
    let mut p = [block[0], block[1], 0, 0, 0, 0, 0, 0];
    if a0 > a1 {
        for i in 1..7u32 {
            p[i as usize + 1] = (((7 - i) * a0 + i * a1 + 3) / 7) as u8;
        }
    } else {
        for i in 1..5u32 {
            p[i as usize + 1] = (((5 - i) * a0 + i * a1 + 2) / 5) as u8;
        }
        p[6] = 0;
        p[7] = 255;
    }
    let mut bits = 0u64;
    for i in 0..6 {
        bits |= (block[2 + i] as u64) << (8 * i);
    }
    for i in 0..16 {
        out[i * stride] = p[((bits >> (3 * i)) & 7) as usize];
    }
}

/// A BC1 colour block into 16 RGBA texels; BC2 and BC3 always use the four-colour mode.
fn bc1(block: &[u8], out: &mut [u8; 64], four_colour: bool) {
    let c0 = block[0] as u32 | (block[1] as u32) << 8;
    let c1 = block[2] as u32 | (block[3] as u32) << 8;
    let mut p = [[0u8; 4]; 4];
    for (i, c) in [c0, c1].into_iter().enumerate() {
        p[i] = [
            ((c >> 11) * 255 / 31) as u8,
            (((c >> 5) & 63) * 255 / 63) as u8,
            ((c & 31) * 255 / 31) as u8,
            255,
        ];
    }
    let opaque = four_colour || c0 > c1;
    for ch in 0..3 {
        let (a, b) = (p[0][ch] as u32, p[1][ch] as u32);
        if opaque {
            p[2][ch] = ((2 * a + b + 1) / 3) as u8;
            p[3][ch] = ((a + 2 * b + 1) / 3) as u8;
        } else {
            p[2][ch] = ((a + b) / 2) as u8;
            p[3][ch] = 0;
        }
    }
    p[2][3] = 255;
    p[3][3] = if opaque { 255 } else { 0 };
    let idx = u32::from_le_bytes([block[4], block[5], block[6], block[7]]);
    for i in 0..16 {
        out[4 * i..4 * i + 4].copy_from_slice(&p[((idx >> (2 * i)) & 3) as usize]);
    }
}

/// One mip of `width` x `height` texels (tightly packed blocks) to tightly packed texels; None when the data is short.
pub fn decode_mip(f: TexFormat, width: u32, height: u32, data: &[u8]) -> Option<Vec<u8>> {
    let block_bytes = match f {
        TexFormat::Bc1 => 8,
        TexFormat::Bc2 | TexFormat::Bc3 | TexFormat::Bc5 => 16,
        _ => return None,
    };
    let (w, h) = (width.max(1) as usize, height.max(1) as usize);
    let (bx_n, by_n) = (w.div_ceil(4), h.div_ceil(4));
    if data.len() < bx_n * by_n * block_bytes {
        return None;
    }
    let bpp = decoded_bpp(f);
    let mut out = vec![0u8; w * h * bpp];
    let mut t = [0u8; 64];
    for by in 0..by_n {
        for bx in 0..bx_n {
            let b = &data[(by * bx_n + bx) * block_bytes..][..block_bytes];
            match f {
                TexFormat::Bc1 => bc1(b, &mut t, false),
                TexFormat::Bc2 => {
                    bc1(&b[8..], &mut t, true);
                    for i in 0..16 {
                        t[4 * i + 3] = ((b[i / 2] >> (4 * (i & 1))) & 15) * 17;
                    }
                }
                TexFormat::Bc3 => {
                    bc1(&b[8..], &mut t, true);
                    bc4(b, &mut t[3..], 4);
                }
                _ => {
                    bc4(b, &mut t, 2);
                    bc4(&b[8..], &mut t[1..], 2);
                }
            }
            // Crop the 4x4 block to the mip (mips smaller than a block, and odd sizes).
            for y in 0..4 {
                let ty = by * 4 + y;
                if ty >= h {
                    break;
                }
                let x0 = bx * 4;
                let n = 4.min(w - x0);
                let dst = (ty * w + x0) * bpp;
                out[dst..dst + n * bpp].copy_from_slice(&t[y * 4 * bpp..(y * 4 + n) * bpp]);
            }
        }
    }
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bc1_solid_and_crop() {
        // c0 = pure red (0xF800), c1 = black, every index 0: all red, opaque.
        let block = [0x00, 0xF8, 0x00, 0x00, 0, 0, 0, 0];
        let out = decode_mip(TexFormat::Bc1, 2, 1, &block).unwrap();
        assert_eq!(out, vec![255, 0, 0, 255, 255, 0, 0, 255]);
    }

    #[test]
    fn bc1_three_colour_transparent() {
        // c0 <= c1 selects three colours plus transparent black; index 3 everywhere.
        let block = [0x00, 0x00, 0x1F, 0x00, 0xFF, 0xFF, 0xFF, 0xFF];
        let out = decode_mip(TexFormat::Bc1, 4, 4, &block).unwrap();
        assert!(out.chunks(4).all(|t| t == [0, 0, 0, 0]));
    }

    #[test]
    fn bc5_two_channels() {
        // Red endpoints 10/10, green 200/200: every texel (10, 200).
        let mut block = [0u8; 16];
        block[0] = 10;
        block[1] = 10;
        block[8] = 200;
        block[9] = 200;
        let out = decode_mip(TexFormat::Bc5, 4, 4, &block).unwrap();
        assert!(out.chunks(2).all(|t| t == [10, 200]));
    }

    #[test]
    fn short_data_is_refused() {
        assert!(decode_mip(TexFormat::Bc3, 8, 8, &[0u8; 32]).is_none());
    }
}
