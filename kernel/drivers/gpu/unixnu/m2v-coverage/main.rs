//! m2v-coverage (darwin-lisa, T0b): CoreAnimation 26's shaders through metal2vulkan and turnip on the host.
//!
//! Every pipeline goes through metal2vulkan's runtime as the AdrenoMetal plug-in makes it
//! (`newRenderPipelineStateWithDescriptor:` / `newComputePipelineStateWithFunction:`): AIR to SPIR-V,
//! then `vkCreate*Pipelines` on turnip, which compiles the SPIR-V to ir3. On the host turnip runs
//! over Mesa's drm-shim (`freedreno_noop`, `FD_GPU_ID=635`: the Adreno 642L's row), so nothing
//! executes, but every compile is the one the phone would do.
//!
//!   m2v-coverage LOADER CASES.tsv LIBRARY.metallib... > RESULTS.tsv   (run.sh: the environment)
//!   m2v-coverage LOADER --constants=FUNCTION LIBRARY.metallib...      (the constants it declares)
//!
//! LOADER: the Vulkan loader (`/usr/lib64/libvulkan.so.1`, with `VK_DRIVER_FILES` at the host turnip).
//!
//! CASES.tsv (cases-from-logs.py): `render VERT FRAG C0 C1 C2 VCONST FCONST ATTR0 SEEN`, the
//! pipelines CoreAnimation linked on the phone. `library` lines are added here for every function
//! of the first library: each kernel as a compute pipeline, each vertex function alone (no
//! fragment function, no attachment), each fragment function the cases do not name after a vertex function of
//! its family (`*_vert_lph` for `*_lph`), with every declared constant 0.
//! Output: one line a pipeline, `ok` or the refusal, and the time it took.

use std::collections::BTreeMap;
use std::path::PathBuf;
use std::time::Instant;

use metal2vulkan::library::{FunctionConstantValue, FunctionConstantValues};
use metal2vulkan::metal::{DataType, FunctionType, PixelFormat, VertexFormat};
use metal2vulkan::pipeline::{
    ColorAttachmentDescriptor, RenderPipelineDescriptor, StageSource, VertexAttributeDescriptor,
    VertexBufferLayoutDescriptor, VertexDescriptor,
};
use metal2vulkan_runtime::{
    Device, DeviceOptions, Function, Library, MetalLevel, PipelineOptions, ShaderCacheOptions,
};

type Result<T> = std::result::Result<T, String>;

/// The bytes of one component of a Metal data type, and how many components it has.
fn shape(ty: DataType) -> (usize, usize) {
    let name = ty.name().trim_start_matches("MTLDataType");
    let count = match name.chars().last() {
        Some(c @ '2'..='4') => c.to_digit(10).unwrap_or(1) as usize,
        _ => 1,
    };
    let base = name.trim_end_matches(|c: char| c.is_ascii_digit());
    let size = match base {
        "Bool" | "Char" | "UChar" => 1,
        "Short" | "UShort" | "Half" | "BFloat" => 2,
        "Long" | "ULong" => 8,
        _ => 4,
    };
    (size, count)
}

/// A constant's value from the integer the logs print (the first component; a wider type takes it
/// little-endian across its bytes, as RenderBox's `uint4 _shader_state` arrives).
fn value(ty: DataType, raw: u128) -> FunctionConstantValue {
    let (size, count) = shape(ty);
    let bytes = raw.to_le_bytes();
    let mut out = vec![0u8; size * count];
    let n = out.len().min(16);
    out[..n].copy_from_slice(&bytes[..n]);
    if ty.name() == "MTLDataTypeFloat" {
        out[..4].copy_from_slice(&(raw as f32).to_le_bytes());
    }
    FunctionConstantValue::new(ty, out)
}

/// The function's declared constants, each with the value `given` names or 0.
fn constants(f: &Function, given: &BTreeMap<u32, u128>) -> Result<FunctionConstantValues> {
    let declared = f.function_constants_dictionary().map_err(|e| format!("constants: {e:?}"))?;
    let mut values = FunctionConstantValues::defaults();
    for (name, c) in declared {
        let v = value(c.data_type(), given.get(&c.index()).copied().unwrap_or(0));
        values = if c.index() == u32::MAX { values.with_name(name, v) } else { values.with_index(c.index(), v) };
    }
    Ok(values)
}

fn parse_constants(text: &str) -> BTreeMap<u32, u128> {
    text.split_whitespace()
        .filter_map(|p| p.split_once('='))
        .filter_map(|(i, v)| Some((i.parse().ok()?, v.parse().ok()?)))
        .collect()
}

struct Libraries(Vec<(String, Library)>);

impl Libraries {
    /// The function named `name` made without constant values: the one whose
    /// `function_constants_dictionary` answers (a specialized one answers none, as Metal's).
    fn plain(&self, name: &str) -> Result<Function> {
        let (_, library) = self
            .0
            .iter()
            .find(|(_, l)| l.function_names().iter().any(|n| n == name))
            .ok_or_else(|| format!("no library has {name}"))?;
        library.new_function_with_name(name).map_err(|e| format!("{name}: {e:?}"))
    }

    fn function(&self, name: &str, given: &BTreeMap<u32, u128>) -> Result<(String, Function)> {
        let (path, library) = self
            .0
            .iter()
            .find(|(_, l)| l.function_names().iter().any(|n| n == name))
            .ok_or_else(|| format!("no library has {name}"))?;
        let plain = library.new_function_with_name(name).map_err(|e| format!("{name}: {e:?}"))?;
        let values = constants(&plain, given)?;
        let f = library
            .new_function_with_name_constant_values(name, &values)
            .map_err(|e| format!("{name}: {e:?}"))?;
        Ok((path.clone(), f))
    }
}

/// The vertex descriptor CoreAnimation's draws use: the function's attributes interleaved in buffer
/// 1 (attribute 0 as the logs give it), one per vertex.
fn vertex_descriptor(f: &Function, attr0: &str) -> Result<Option<VertexDescriptor>> {
    let attributes = match f.vertex_attributes().map_err(|e| format!("attributes: {e:?}"))? {
        Some(a) if !a.is_empty() => a,
        _ => return Ok(None),
    };
    let logged: Vec<u32> = attr0.split(':').filter_map(|x| x.parse().ok()).collect();
    let buffer = logged.get(2).copied().unwrap_or(1);
    let mut d = VertexDescriptor::new();
    let mut offset = 0u32;
    for a in attributes {
        let format = match (a.attribute_index(), logged.first()) {
            (0, Some(&raw)) => VertexFormat::try_from(raw).map_err(|e| format!("{e:?}"))?,
            _ => {
                let name = format!("MTLVertexFormat{}", a.attribute_type().name().trim_start_matches("MTLDataType"));
                VertexFormat::from_name(&name).ok_or_else(|| format!("no vertex format for {name}"))?
            }
        };
        let (size, count) = shape(a.attribute_type());
        d.attributes.insert(a.attribute_index(), VertexAttributeDescriptor::new(format, offset, buffer));
        offset += ((size * count) as u32).max(4).next_multiple_of(4);
    }
    d.layouts.insert(buffer, VertexBufferLayoutDescriptor::new(Some(offset.max(4))));
    Ok(Some(d))
}

fn color(raw: &str) -> Result<Option<ColorAttachmentDescriptor>> {
    match raw.parse::<u32>() {
        Ok(0) | Err(_) => Ok(None),
        Ok(v) => Ok(Some(ColorAttachmentDescriptor::new(PixelFormat::try_from(v).map_err(|e| format!("{e:?}"))?))),
    }
}

fn render(device: &Device, libs: &Libraries, f: &[&str]) -> Result<String> {
    let (vert, frag, c0, attr0) = (f[1], f[2], f[3], f[8]);
    let fc = parse_constants(f[7]);
    // CoreAnimation hands both functions one MTLFunctionConstantValues: the vertex function takes
    // the fragment's value of every constant the fragment declares (the logged set, 0 where not
    // logged), and the trace line's value of the others (its own: 3, 0, 46 of fixed_vert_lph_spc)
    let mut vc = parse_constants(f[6]);
    if frag != "-" {
        for (_, c) in libs.plain(frag)?.function_constants_dictionary().map_err(|e| format!("{e:?}"))? {
            vc.insert(c.index(), fc.get(&c.index()).copied().unwrap_or(0));
        }
    }
    let (lib, v) = libs.function(vert, &vc)?;
    let mut d = RenderPipelineDescriptor::new(StageSource::Function(v.function_descriptor()));
    d.vertex_descriptor = vertex_descriptor(&v, attr0)?;
    if frag != "-" {
        let (_, fr) = libs.function(frag, &fc)?;
        d.fragment = Some(StageSource::Function(fr.function_descriptor()));
        d.color_attachments[0] = color(c0)?;
        d.color_attachments[1] = color(f[4])?;
        d.color_attachments[2] = color(f[5])?;
    }
    // A pipeline logged without a trace line has no record of attachments 1 and 2. Where the
    // fragment function needs one (it reads or writes it, `ColorAttachment { index }`), the
    // attachment is tried in attachment 0's format, RGBA16Float (115: the uber shader's mask, as
    // every traced pipeline of it has), R32Float, R32Uint and R16Uint (an integer output), in turn.
    let untraced = [false, f[4] == "-", f[5] == "-"];
    let mut tries: [Vec<&str>; 3] = [vec![], vec![c0, "115", "55", "53", "13"], vec![c0, "115", "55", "53", "13"]];
    loop {
        let e = match device.new_render_pipeline_state(&d, &PipelineOptions::default()) {
            Ok(_) => return Ok(lib),
            Err(e) => format!("{e:?}"),
        };
        let at = (1..3).find(|i| untraced[*i] && e.contains(&format!("ColorAttachment {{ index: {i} }}")));
        match at.and_then(|i| (!tries[i].is_empty()).then(|| (i, tries[i].remove(0)))) {
            Some((i, format)) => d.color_attachments[i] = color(format)?,
            None => return Err(e),
        }
    }
}

fn compute(device: &Device, libs: &Libraries, name: &str) -> Result<String> {
    let (lib, k) = libs.function(name, &BTreeMap::new())?;
    device
        .new_compute_pipeline_state_with_function(&k, &PipelineOptions::default())
        .map(|_| lib)
        .map_err(|e| format!("{e:?}"))
}

/// The `library` cases: every function of `library` the logged cases leave out. A fragment
/// function gets `*` for its vertex function: [`partners`] lists the ones to try.
fn library_cases(library: &Library, cases: &[Vec<String>]) -> Vec<Vec<String>> {
    let named: Vec<&str> = cases.iter().flat_map(|c| [c[1].as_str(), c[2].as_str()]).collect();
    let mut out = Vec::new();
    for name in library.function_names() {
        let Ok(f) = library.new_function_with_name(&name) else { continue };
        let row = |kind: &str, v: &str, fr: &str, c0: &str| {
            [kind, v, fr, c0, "0", "0", "-", "-", "-", "0"].iter().map(|s| s.to_string()).collect::<Vec<_>>()
        };
        match f.function_type() {
            Ok(FunctionType::Kernel) => out.push(row("kernel", &name, "-", "0")),
            Ok(FunctionType::Vertex) => out.push(row("vertex", &name, "-", "0")),
            Ok(FunctionType::Fragment) if !named.contains(&name.as_str()) => out.push(row("fragment", "*", &name, "80")),
            _ => {}
        }
    }
    out
}

/// The vertex functions to try under fragment function `frag`, most likely first: the logged
/// partner of its `_lph` twin (with the twin's suffix), then those of its own suffix, then the rest.
fn partners(library: &Library, cases: &[Vec<String>], frag: &str) -> Vec<String> {
    let suffix = frag.rsplit('_').next().unwrap_or("");
    let twin = frag.replace("_lpf", "_lph");
    let mut out: Vec<String> = cases
        .iter()
        .filter(|c| c[2] == twin || c[2] == frag)
        .map(|c| c[1].replace("_lph", &format!("_{suffix}")))
        .collect();
    let vertices: Vec<String> = library
        .function_names()
        .into_iter()
        .filter(|n| matches!(library.new_function_with_name(n).map(|f| f.function_type()), Ok(Ok(FunctionType::Vertex))))
        .collect();
    out.retain(|v| vertices.contains(v));
    out.extend(vertices.iter().filter(|v| v.ends_with(suffix)).cloned());
    out.extend(vertices.iter().cloned());
    out.dedup();
    let mut seen = std::collections::BTreeSet::new();
    out.retain(|v| seen.insert(v.clone()));
    out
}

/// The AIR slice of a universal metallib (big-endian fat header, an AIR64 `MTLB` beside Apple's GPU
/// binary, as QuartzCore's ships): Metal hands the plug-in that slice; anything else as it is.
fn air_slice(bytes: &[u8]) -> &[u8] {
    let be = |at: usize| u32::from_be_bytes(bytes[at..at + 4].try_into().unwrap_or([0; 4]));
    if bytes.len() < 8 || be(0) != 0xcafe_babe {
        return bytes;
    }
    for i in 0..be(4) as usize {
        let at = 8 + 20 * i;
        let (offset, size) = (be(at + 8) as usize, be(at + 12) as usize);
        if bytes.get(offset..offset + 4) == Some(b"MTLB") && offset + size <= bytes.len() {
            return &bytes[offset..offset + size];
        }
    }
    bytes
}

fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 4 {
        return Err("usage: m2v-coverage LOADER CASES.tsv|--constants=NAME LIBRARY.metallib...".into());
    }
    let mut options = DeviceOptions::default();
    options.loader = Some(PathBuf::from(&args[1]));
    options.metal_level = Some(MetalLevel::NEWEST);
    // a fresh cache directory a run (M2V_CACHE), so that every pipeline is compiled, not read back
    let cache = std::env::var("M2V_CACHE").unwrap_or_else(|_| "/tmp/m2v-coverage-cache".into());
    let _ = std::fs::remove_dir_all(&cache);
    options.shader_cache = ShaderCacheOptions::in_directory(cache);
    let device = Device::system_default_with(&options).map_err(|e| format!("device: {e:?}"))?;
    eprintln!("device: {}", device.name());
    let mut libraries = Vec::new();
    for path in &args[3..] {
        let bytes = std::fs::read(path).map_err(|e| format!("{path}: {e}"))?;
        match device.new_library_with_data(air_slice(&bytes)) {
            Ok(l) => libraries.push((path.clone(), l)),
            Err(e) => eprintln!("{path}: not a library here: {e:?}"),
        }
    }
    let libs = Libraries(libraries);
    // m2v-coverage TURNIP.so --constants NAME LIBRARY...: the constants a function declares
    if let Some(name) = args[2].strip_prefix("--constants=") {
        for (n, c) in libs.plain(name)?.function_constants_dictionary().map_err(|e| format!("{e:?}"))? {
            println!("{}\t{n}\t{}\t{}", c.index(), c.data_type().name(), c.is_required());
        }
        return Ok(());
    }
    let mut cases: Vec<Vec<String>> = std::fs::read_to_string(&args[2])
        .map_err(|e| format!("{}: {e}", args[2]))?
        .lines()
        .map(|l| l.split('\t').map(str::to_string).collect())
        .filter(|f: &Vec<String>| f.len() >= 10)
        .collect();
    let extra = library_cases(&libs.0[0].1, &cases);
    cases.extend(extra);
    for c in &cases {
        let f: Vec<&str> = c.iter().map(String::as_str).collect();
        let began = Instant::now();
        let mut tried = 0;
        let mut used = f[1].to_string();
        let outcome = match f[0] {
            "kernel" => compute(&device, &libs, f[1]),
            "fragment" => {
                let mut last = Err("no vertex function to pair it with".to_string());
                for v in partners(&libs.0[0].1, &cases, f[2]) {
                    let mut g = f.clone();
                    g[1] = &v;
                    tried += 1;
                    last = render(&device, &libs, &g);
                    // a vertex function whose outputs are not this fragment function's inputs: the next
                    if !matches!(&last, Err(e) if e.contains("UnwrittenFragmentInputs") || e.contains("MismatchedFragmentInput")) {
                        used = v.clone();
                        break;
                    }
                }
                last
            }
            _ => render(&device, &libs, &f),
        };
        let ms = began.elapsed().as_secs_f64() * 1e3;
        let (status, detail) = match outcome {
            Ok(lib) => ("ok".to_string(), lib.rsplit('/').nth(1).unwrap_or("").to_string()),
            Err(e) => ("refused".to_string(), e.replace(['\n', '\t'], " ").chars().take(400).collect()),
        };
        let mut row = c[..10].to_vec();
        row[1] = if tried > 0 { format!("{used} (of {tried})") } else { used };
        println!("{}\t{status}\t{ms:.1}\t{detail}", row.join("\t"));
    }
    Ok(())
}
