//! m2v-formats (darwin-lisa, T2 prerequisite): how metal2vulkan's runtime samples the pixel formats
//! CoreAnimation's text and layer backing stores use, drawn and read back.
//!
//! Each case is a texture of one Metal pixel format (optionally a swizzled view), every texel the
//! same bytes, sampled by `sampled_fragment` of the repository's own fixture (`runtime_examples.ll`,
//! assembled by `llvm-as`) over the whole of an RGBA8Unorm target, which is read back and compared
//! with what Metal presents: A8Unorm as (0, 0, 0, a), R8 as (r, 0, 0, 1), RG8 as (r, g, 0, 1),
//! BGRA8 with red and blue in Metal's order.
//!
//!   m2v-formats LOADER RUNTIME_EXAMPLES.ll
//!
//! On a device that executes (lavapipe) every case must match; under drm-shim nothing executes, so
//! there only the making of each texture, view and pipeline is checked (the read-back is zeros).

use metal2vulkan::metal::{PixelFormat, PrimitiveType, TextureSwizzle as S, TextureSwizzleChannels, TextureUsage};
use metal2vulkan::pipeline::{ColorAttachmentDescriptor, RenderPipelineDescriptor, StageSource};
use metal2vulkan_runtime::{
    BlitOption, ClearColor, CommandBufferStatus, Device, DeviceOptions, LoadAction, MetalLevel, Origin, PipelineOptions,
    Region, RenderPassDescriptor, ResourceOptions, ShaderCacheOptions, Size, StoreAction, TextureDescriptor,
};
use std::path::PathBuf;
use std::process::Command;
use std::time::Duration;

const SIDE: u64 = 8;

struct Case {
    name: &'static str,
    format: PixelFormat,
    texel: &'static [u8],
    swizzle: Option<TextureSwizzleChannels>,
    expect: [u8; 4],
}

fn swizzle(r: S, g: S, b: S, a: S) -> Option<TextureSwizzleChannels> {
    Some(TextureSwizzleChannels { red: r, green: g, blue: b, alpha: a })
}

fn cases() -> Vec<Case> {
    vec![
        Case { name: "A8Unorm (CA's alpha-only text backing)", format: PixelFormat::A8Unorm, texel: &[0x80], swizzle: None, expect: [0, 0, 0, 0x80] },
        Case { name: "R8Unorm", format: PixelFormat::R8Unorm, texel: &[0x80], swizzle: None, expect: [0x80, 0, 0, 0xff] },
        Case { name: "R8Unorm viewed (1,1,1,r): a mask as A8", format: PixelFormat::R8Unorm, texel: &[0x80], swizzle: swizzle(S::One, S::One, S::One, S::Red), expect: [0xff, 0xff, 0xff, 0x80] },
        Case { name: "R8Unorm viewed (r,r,r,1): grey", format: PixelFormat::R8Unorm, texel: &[0x80], swizzle: swizzle(S::Red, S::Red, S::Red, S::One), expect: [0x80, 0x80, 0x80, 0xff] },
        Case { name: "RG8Unorm", format: PixelFormat::RG8Unorm, texel: &[0x80, 0x40], swizzle: None, expect: [0x80, 0x40, 0, 0xff] },
        Case { name: "RG8Unorm viewed (r,r,r,g): LA8", format: PixelFormat::RG8Unorm, texel: &[0x80, 0x40], swizzle: swizzle(S::Red, S::Red, S::Red, S::Green), expect: [0x80, 0x80, 0x80, 0x40] },
        Case { name: "BGRA8Unorm premultiplied", format: PixelFormat::BGRA8Unorm, texel: &[0x10, 0x20, 0x30, 0x80], swizzle: None, expect: [0x30, 0x20, 0x10, 0x80] },
        Case { name: "RGBA8Unorm premultiplied", format: PixelFormat::RGBA8Unorm, texel: &[0x30, 0x20, 0x10, 0x80], swizzle: None, expect: [0x30, 0x20, 0x10, 0x80] },
        Case { name: "RGBA16Float", format: PixelFormat::RGBA16Float, texel: &[0x00, 0x38, 0x00, 0x34, 0x00, 0x30, 0x00, 0x3c], swizzle: None, expect: [0x80, 0x40, 0x20, 0xff] },
    ]
}

type Result<T> = std::result::Result<T, String>;

fn run(device: &Device, library: &metal2vulkan_runtime::Library, case: &Case) -> Result<[u8; 4]> {
    fn e(w: &'static str) -> impl Fn(metal2vulkan_runtime::Error) -> String {
        move |x| format!("{w}: {x:?}")
    }
    let vertex = library.new_function_with_name("colored_vertex").map_err(e("vertex"))?;
    let fragment = library.new_function_with_name("sampled_fragment").map_err(e("fragment"))?;
    let mut d = RenderPipelineDescriptor::new(StageSource::Function(vertex.function_descriptor()));
    d.fragment = Some(StageSource::Function(fragment.function_descriptor()));
    d.color_attachments[0] = Some(ColorAttachmentDescriptor::new(PixelFormat::RGBA8Unorm));
    let state = device.new_render_pipeline_state(&d, &PipelineOptions::default()).map_err(e("pipeline"))?;

    let mut td = TextureDescriptor::texture_2d(case.format, SIDE, SIDE, false);
    td.usage = TextureUsage::SHADER_READ | TextureUsage::PIXEL_FORMAT_VIEW;
    let texture = device.new_texture(&td).map_err(e("texture"))?;
    let bytes: Vec<u8> = (0..SIDE * SIDE).flat_map(|_| case.texel.iter().copied()).collect();
    let row = SIDE * case.texel.len() as u64;
    texture.replace_region(Region::new_2d(0, 0, SIDE, SIDE), 0, 0, &bytes, row, row * SIDE).map_err(e("upload"))?;
    let sampled = match case.swizzle {
        Some(s) => texture.new_texture_view_with_swizzle(case.format, metal2vulkan::metal::TextureType::Type2D, 0..1, 0..1, s).map_err(e("swizzled view"))?,
        None => texture.clone(),
    };

    let mut target = TextureDescriptor::texture_2d(PixelFormat::RGBA8Unorm, SIDE, SIDE, false);
    target.usage = TextureUsage::RENDER_TARGET;
    target.resource_options = ResourceOptions::private();
    let target = device.new_texture(&target).map_err(e("target"))?;
    let quad: [[f32; 2]; 6] = [[-1.0, 1.0], [1.0, 1.0], [-1.0, -1.0], [1.0, 1.0], [1.0, -1.0], [-1.0, -1.0]];
    let quad: Vec<u8> = quad.iter().flatten().flat_map(|v| v.to_le_bytes()).collect();
    let colors: Vec<u8> = (0..6).flat_map(|_| [1.0f32; 4]).flat_map(|v| v.to_le_bytes()).collect();
    let quad = device.new_buffer_with_bytes(&quad, ResourceOptions::shared()).map_err(e("buffer"))?;
    let colors = device.new_buffer_with_bytes(&colors, ResourceOptions::shared()).map_err(e("buffer"))?;
    let mut pass = RenderPassDescriptor::new();
    let c = &mut pass.color_attachments[0];
    c.texture = Some(target.clone());
    c.load_action = LoadAction::Clear;
    c.store_action = StoreAction::Store;
    c.clear_color = ClearColor::new(0.5, 0.5, 0.5, 0.5);
    let readback = device.new_buffer(SIDE * 4 * SIDE, ResourceOptions::shared()).map_err(e("buffer"))?;
    let queue = device.new_command_queue().map_err(e("queue"))?;
    let cb = queue.command_buffer().map_err(e("command buffer"))?;
    let mut enc = cb.render_command_encoder(&pass).map_err(e("encoder"))?;
    enc.set_render_pipeline_state(&state).map_err(e("state"))?;
    enc.set_vertex_buffer(Some(&quad), 0, 0).map_err(e("bind"))?;
    enc.set_vertex_buffer(Some(&colors), 0, 1).map_err(e("bind"))?;
    enc.set_fragment_texture(Some(&sampled), 0).map_err(e("bind texture"))?;
    enc.draw_primitives(PrimitiveType::Triangle, 0, 6, 1, 0).map_err(e("draw"))?;
    enc.end_encoding().map_err(e("end"))?;
    let mut blit = cb.blit_command_encoder().map_err(e("blit"))?;
    blit.copy_from_texture_to_buffer(&target, 0, 0, Origin::new(0, 0, 0), Size::new(SIDE, SIDE, 1), &readback, 0, SIDE * 4,
        SIDE * 4 * SIDE, BlitOption::None).map_err(e("copy"))?;
    blit.end_encoding().map_err(e("end"))?;
    cb.commit().map_err(e("commit"))?;
    cb.wait_until_completed_for(Duration::from_secs(10)).map_err(|_| "no completion".to_string())?;
    if cb.status() != CommandBufferStatus::Completed {
        return Err(format!("{:?} {:?}", cb.status(), cb.error()));
    }
    let contents = readback.contents().ok_or("no contents")?;
    // SAFETY: the buffer's bytes, no command buffer writes them now.
    let texels = unsafe { std::slice::from_raw_parts(contents.as_ptr(), (SIDE * 4 * SIDE) as usize) };
    let at = ((SIDE / 2) * SIDE + SIDE / 2) as usize * 4;
    Ok([texels[at], texels[at + 1], texels[at + 2], texels[at + 3]])
}

fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        return Err("usage: m2v-formats LOADER RUNTIME_EXAMPLES.ll".into());
    }
    let mut options = DeviceOptions::default();
    options.loader = Some(PathBuf::from(&args[1]));
    options.metal_level = Some(MetalLevel::NEWEST);
    options.shader_cache = ShaderCacheOptions::in_directory("/tmp/m2v-formats-cache");
    let device = Device::system_default_with(&options).map_err(|e| format!("device: {e:?}"))?;
    println!("device: {}", device.name());
    let air = Command::new("llvm-as").arg("-o").arg("-").arg(&args[2]).output().map_err(|e| format!("llvm-as: {e}"))?;
    let library = metal2vulkan::library::Library::from_air(&air.stdout).map_err(|e| format!("library: {e:?}"))?;
    let library = device.new_library_with_compile_library(library);
    let mut bad = 0;
    for case in cases() {
        match run(&device, &library, &case) {
            Ok(got) => {
                let ok = got.iter().zip(case.expect).all(|(g, x)| (*g as i32 - x as i32).abs() <= 1);
                bad += usize::from(!ok);
                println!("{} {:<44} sampled {:02x?}, Metal presents {:02x?}", if ok { "ok  " } else { "DIFF" }, case.name, got, case.expect);
            }
            Err(why) => {
                bad += 1;
                println!("FAIL {:<44} {why}", case.name);
            }
        }
    }
    let _ = std::fs::remove_dir_all("/tmp/m2v-formats-cache");
    println!("{bad} case(s) differ or fail");
    Ok(())
}
