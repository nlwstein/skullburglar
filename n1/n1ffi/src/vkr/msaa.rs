use anyhow::Result;
use ash::vk;

use crate::log::{self, n1log};
use super::{DEPTH_FORMAT, Garbage, HDR_FORMAT, Renderer, Rt};

const COLOR_USAGE: vk::ImageUsageFlags = vk::ImageUsageFlags::COLOR_ATTACHMENT;
const DEPTH_USAGE: vk::ImageUsageFlags = vk::ImageUsageFlags::DEPTH_STENCIL_ATTACHMENT;

pub(super) fn samples(instance: &ash::Instance, device: vk::PhysicalDevice, requested: u32) -> vk::SampleCountFlags {
    let mut resolve = vk::PhysicalDeviceDepthStencilResolveProperties::default();
    let mut props = vk::PhysicalDeviceProperties2::default().push_next(&mut resolve);
    let standard = unsafe {
        instance.get_physical_device_properties2(device, &mut props);
        props.properties.limits.standard_sample_locations != 0
    };
    let count = vk::SampleCountFlags::from_raw(requested);
    let supports = |format, usage| unsafe {
        instance.get_physical_device_image_format_properties(device, format, vk::ImageType::TYPE_2D,
            vk::ImageTiling::OPTIMAL, usage, vk::ImageCreateFlags::empty())
            .is_ok_and(|p| p.sample_counts.contains(count))
    };
    if standard && resolve.supported_depth_resolve_modes.contains(vk::ResolveModeFlags::SAMPLE_ZERO)
        && supports(HDR_FORMAT, COLOR_USAGE) && supports(DEPTH_FORMAT, DEPTH_USAGE)
    {
        n1log!(log::INFO, "summary: n1: {requested}x MSAA");
        count
    } else {
        n1log!(log::WARN, "summary: n1: {requested}x MSAA unavailable on this device, drawing at 1x");
        vk::SampleCountFlags::TYPE_1
    }
}

impl Renderer {
    pub(super) fn ensure_msaa(&mut self, width: u32, height: u32) -> Result<()> {
        if self.msaa.as_ref().is_some_and(|r| r[0].width == width && r[0].height == height) {
            return Ok(());
        }
        let mut make = |format, usage, aspect, name| -> Result<Rt> {
            let (image, view, alloc) = self.image(format, width, height, 1, self.samples, usage, aspect, name)?;
            Ok(Rt { image, view, alloc, width, height })
        };
        let color = make(HDR_FORMAT, COLOR_USAGE, vk::ImageAspectFlags::COLOR, "n1 MSAA color")?;
        let depth = match make(DEPTH_FORMAT, DEPTH_USAGE, vk::ImageAspectFlags::DEPTH, "n1 MSAA depth") {
            Ok(rt) => rt,
            Err(e) => {
                self.free(Garbage::Image(color.image, color.view, color.alloc));
                return Err(e);
            }
        };
        if let Some(old) = self.msaa.replace([color, depth]) {
            for rt in old {
                self.defer(Garbage::Image(rt.image, rt.view, rt.alloc));
            }
        }
        Ok(())
    }

    // The depth resolve keeps sample 0.
    // Shift any geometry replay that tests against it so the 1x rasterizer tests the same triangle point.
    pub(super) fn depth_sample_offset(&self) -> [f32; 2] {
        match self.samples {
            vk::SampleCountFlags::TYPE_2 => [0.25, 0.25],
            vk::SampleCountFlags::TYPE_4 => [-0.125, -0.375],
            _ => [0.0, 0.0],
        }
    }
}
