//! m2v-renderarea (darwin-lisa): metal2vulkan-lisa.patch's trimmed render areas (M2V_RENDER_AREA=1) change no pixel.
//!
//! On a device that executes (lavapipe), a 96x80 BGRA8 target is cleared to grey in a pass of its own, then drawn into by
//! passes that load and store it, each draw under a scissor (one panel; two panels apart; a draw past the edge), CoreAnimation's
//! glass panels in miniature, and read back. The program prints the read-back bytes' hash and checks the drawn and undrawn
//! pixels; run once without and once with M2V_RENDER_AREA=1, the hashes must match.
//!
//!   m2v-renderarea LOADER RUNTIME_EXAMPLES.ll

use metal2vulkan::metal::{PixelFormat, PrimitiveType, TextureUsage};
use metal2vulkan::pipeline::{ColorAttachmentDescriptor, RenderPipelineDescriptor, StageSource};
use metal2vulkan_runtime::{
    BlitOption, ClearColor, CommandBufferStatus, Device, DeviceOptions, LoadAction, MetalLevel, Origin, PipelineOptions,
    RenderPassDescriptor, ResourceOptions, ScissorRect, ShaderCacheOptions, Size, StoreAction, TextureDescriptor,
};
use std::path::PathBuf;
use std::process::Command;
use std::time::Duration;

const W: u64 = 96;
const H: u64 = 80;

fn e(what: &'static str) -> impl Fn(metal2vulkan_runtime::Error) -> String {
    move |x| format!("{what}: {x:?}")
}

fn main() -> Result<(), String> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        return Err("usage: m2v-renderarea LOADER RUNTIME_EXAMPLES.ll".into());
    }
    let mut options = DeviceOptions::default();
    options.loader = Some(PathBuf::from(&args[1]));
    options.metal_level = Some(MetalLevel::NEWEST);
    options.shader_cache = ShaderCacheOptions::in_directory("/tmp/m2v-renderarea-cache");
    let device = Device::system_default_with(&options).map_err(e("device"))?;
    let air = Command::new("llvm-as").arg("-o").arg("-").arg(&args[2]).output().map_err(|x| format!("llvm-as: {x}"))?;
    let library = metal2vulkan::library::Library::from_air(&air.stdout).map_err(|x| format!("library: {x:?}"))?;
    let library = device.new_library_with_compile_library(library);
    let vertex = library.new_function_with_name("colored_vertex").map_err(e("vertex"))?;
    let fragment = library.new_function_with_name("colored_fragment").map_err(e("fragment"))?;
    let mut d = RenderPipelineDescriptor::new(StageSource::Function(vertex.function_descriptor()));
    d.fragment = Some(StageSource::Function(fragment.function_descriptor()));
    d.color_attachments[0] = Some(ColorAttachmentDescriptor::new(PixelFormat::BGRA8Unorm));
    let state = device.new_render_pipeline_state(&d, &PipelineOptions::default()).map_err(e("pipeline"))?;

    let mut td = TextureDescriptor::texture_2d(PixelFormat::BGRA8Unorm, W, H, false);
    td.usage = TextureUsage::RENDER_TARGET;
    td.resource_options = ResourceOptions::private();
    let target = device.new_texture(&td).map_err(e("target"))?;
    let quad: [[f32; 2]; 6] = [[-1.0, 1.0], [1.0, 1.0], [-1.0, -1.0], [1.0, 1.0], [1.0, -1.0], [-1.0, -1.0]];
    let quad: Vec<u8> = quad.iter().flatten().flat_map(|v| v.to_le_bytes()).collect();
    let quad = device.new_buffer_with_bytes(&quad, ResourceOptions::shared()).map_err(e("buffer"))?;
    let colour = |c: [f32; 4]| -> Result<_, String> {
        let bytes: Vec<u8> = (0..6).flat_map(|_| c).flat_map(|v| v.to_le_bytes()).collect();
        device.new_buffer_with_bytes(&bytes, ResourceOptions::shared()).map_err(e("buffer"))
    };
    let red = colour([1.0, 0.0, 0.0, 1.0])?;
    let blue = colour([0.0, 0.0, 1.0, 1.0])?;
    let queue = device.new_command_queue().map_err(e("queue"))?;
    let cb = queue.command_buffer().map_err(e("command buffer"))?;
    // grey, the whole target (a Clear load: never trimmed)
    let mut pass = RenderPassDescriptor::new();
    pass.color_attachments[0].texture = Some(target.clone());
    pass.color_attachments[0].load_action = LoadAction::Clear;
    pass.color_attachments[0].store_action = StoreAction::Store;
    pass.color_attachments[0].clear_color = ClearColor::new(0.5, 0.5, 0.5, 1.0);
    cb.render_command_encoder(&pass).map_err(e("clear pass"))?.end_encoding().map_err(e("end"))?;
    // the panels: Load/Store passes, each draw a full-screen quad under a scissor
    let panels: [&[(ScissorRect, &metal2vulkan_runtime::Buffer)]; 3] = [
        &[(ScissorRect { x: 10, y: 12, width: 20, height: 16 }, &red)],
        &[(ScissorRect { x: 4, y: 50, width: 30, height: 20 }, &blue), (ScissorRect { x: 60, y: 8, width: 24, height: 40 }, &red)],
        &[(ScissorRect { x: 80, y: 70, width: 16, height: 10 }, &blue)],
    ];
    for draws in panels {
        let mut pass = RenderPassDescriptor::new();
        pass.color_attachments[0].texture = Some(target.clone());
        pass.color_attachments[0].load_action = LoadAction::Load;
        pass.color_attachments[0].store_action = StoreAction::Store;
        let mut enc = cb.render_command_encoder(&pass).map_err(e("panel pass"))?;
        enc.set_render_pipeline_state(&state).map_err(e("state"))?;
        enc.set_vertex_buffer(Some(&quad), 0, 0).map_err(e("bind"))?;
        for (rect, c) in draws {
            enc.set_scissor_rect(*rect).map_err(e("scissor"))?;
            enc.set_vertex_buffer(Some(c), 0, 1).map_err(e("bind"))?;
            enc.draw_primitives(PrimitiveType::Triangle, 0, 6, 1, 0).map_err(e("draw"))?;
        }
        enc.end_encoding().map_err(e("end"))?;
    }
    let readback = device.new_buffer(W * 4 * H, ResourceOptions::shared()).map_err(e("buffer"))?;
    let mut blit = cb.blit_command_encoder().map_err(e("blit"))?;
    blit.copy_from_texture_to_buffer(&target, 0, 0, Origin::new(0, 0, 0), Size::new(W, H, 1), &readback, 0, W * 4, W * 4 * H,
        BlitOption::None).map_err(e("copy"))?;
    blit.end_encoding().map_err(e("end"))?;
    cb.commit().map_err(e("commit"))?;
    cb.wait_until_completed_for(Duration::from_secs(10)).map_err(|_| "no completion".to_string())?;
    if cb.status() != CommandBufferStatus::Completed {
        return Err(format!("{:?} {:?}", cb.status(), cb.error()));
    }
    let contents = readback.contents().ok_or("no contents")?;
    // SAFETY: the buffer's bytes, no command buffer writes them now.
    let px = unsafe { std::slice::from_raw_parts(contents.as_ptr(), (W * 4 * H) as usize) };
    let at = |x: u64, y: u64| -> [u8; 4] {
        let i = ((y * W + x) * 4) as usize;
        [px[i], px[i + 1], px[i + 2], px[i + 3]]
    };
    // BGRA bytes: red is [0,0,255,255], blue [255,0,0,255], grey [128,128,128,255]
    let checks = [((15, 20), [0, 0, 255, 255]), ((10, 60), [255, 0, 0, 255]), ((70, 30), [0, 0, 255, 255]),
        ((85, 75), [255, 0, 0, 255]), ((50, 40), [128, 128, 128, 255]), ((0, 0), [128, 128, 128, 255]),
        ((95, 79), [255, 0, 0, 255]), ((40, 20), [128, 128, 128, 255])];
    let mut bad = 0;
    for ((x, y), want) in checks {
        let got = at(x, y);
        let ok = got.iter().zip(want).all(|(g, w)| (*g as i32 - w as i32).abs() <= 1);
        bad += usize::from(!ok);
        if !ok {
            println!("pixel ({x},{y}) is {got:?}, not {want:?}");
        }
    }
    let hash = px.iter().fold(0xcbf29ce484222325u64, |h, b| (h ^ *b as u64).wrapping_mul(0x100000001b3));
    println!("M2V_RENDER_AREA={} hash {hash:016x}, {bad} pixel check(s) failed",
        std::env::var("M2V_RENDER_AREA").unwrap_or_else(|_| "0".into()));
    let _ = std::fs::remove_dir_all("/tmp/m2v-renderarea-cache");
    Ok(())
}
