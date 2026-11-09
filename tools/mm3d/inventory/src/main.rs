//! mm3d-inventory: usa el descubrimiento de funciones de 3dsrecomp (MIT) sobre un
//! `code.bin` descomprimido y su `exheader.bin`, sin contenedor ni cifrado.
//!
//! uso: mm3d-inventory <code.bin> <exheader.bin> <salida.csv> [--title-id HEX16] [--no-hints]
//!
//! CSV: entry,thumb,size,instructions,source   (entry en hex; para Thumb 3dsrecomp usa el bit 0)
//! Resumen (a stderr): funciones, Thumb, hash de codigo (FNV-1a 64 del segmento de texto) y hints aplicados.

use recomp3ds::build::{bundled, code_hash};
use recomp3ds::discover::{self, Mode, Source};
use recomp3ds::image::Image;
use recomp3ds::rom::ExHeader;
use std::collections::BTreeMap;
use std::io::Write;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.len() < 3 {
        eprintln!("uso: mm3d-inventory <code.bin> <exheader.bin> <salida.csv> [--title-id HEX16] [--no-hints]");
        std::process::exit(2);
    }
    let code = std::fs::read(&args[0]).expect("no se pudo leer code.bin");
    let exheader = std::fs::read(&args[1]).expect("no se pudo leer exheader.bin");
    let mut title_id: Option<u64> = None;
    let mut use_hints = true;
    let mut i = 3;
    while i < args.len() {
        match args[i].as_str() {
            "--title-id" => {
                i += 1;
                title_id = u64::from_str_radix(&args[i], 16).ok();
            }
            "--no-hints" => use_hints = false,
            other => {
                eprintln!("opcion desconocida: {other}");
                std::process::exit(2);
            }
        }
        i += 1;
    }
    let header = ExHeader::read(&exheader);
    let mut program = Image::from_code(&header, &code).into_program(&[], &[]);
    let hash = code_hash(&program);
    let mut analysis = discover::analyze(&program);
    let first = analysis.functions.len();
    let mut applied = 0usize;
    if use_hints {
        if let Some(id) = title_id {
            let text = program.text.base..program.text.end();
            let mut hinted: Vec<u32> = bundled(id, hash).into_iter().filter(|a| text.contains(&(a & !1))).collect();
            hinted.sort_unstable();
            hinted.dedup();
            let new: Vec<u32> = hinted.into_iter().filter(|a| !analysis.functions.contains_key(a)).collect();
            if !new.is_empty() {
                applied = new.len();
                program.seeds.extend(new.iter().map(|&a| (a, Source::Hint)));
                analysis = discover::analyze(&program);
            }
        }
    }
    let mut out = std::io::BufWriter::new(std::fs::File::create(&args[2]).expect("no se pudo crear el CSV"));
    writeln!(out, "entry,thumb,size,instructions,source").unwrap();
    let mut by_source: BTreeMap<String, usize> = BTreeMap::new();
    let mut thumb = 0usize;
    for (&entry, f) in &analysis.functions {
        let is_thumb = f.mode == Mode::Thumb;
        thumb += is_thumb as usize;
        let last = f.instructions.iter().copied().max().unwrap_or(entry & !1);
        let step = if is_thumb { 2 } else { 4 };
        let size = last.saturating_sub(entry & !1) + step;
        let source = format!("{:?}", f.source);
        *by_source.entry(source.clone()).or_default() += 1;
        writeln!(out, "0x{:08X},{},{},{},{}", entry, is_thumb as u8, size, f.instructions.len(), source).unwrap();
    }
    // Mapa de palabras (4 bytes): 1 si 3dsrecomp clasifica la palabra como codigo, 0 si es literal/dato/desconocido.
    // Sirve para ignorar pools y tablas inline al calcular firmas de funciones.
    // Mapa de modos por media-palabra (2 bytes): 0 sin clasificar, 1 instruccion ARM, 2 instruccion Thumb, 3 ambos.
    // Para Thumb tambien se marca la segunda mitad de los pares BL/BLX (primera mitad 0xF000..0xF7FF).
    let mut modes = vec![0u8; program.text.bytes.len() / 2];
    let text_base = program.text.base;
    let mut mark = |address: u32, mode: u8| {
        let index = (address.wrapping_sub(text_base) / 2) as usize;
        if index < modes.len() {
            modes[index] = if modes[index] == 0 || modes[index] == mode { mode } else { 3 };
        }
    };
    for (&entry, f) in &analysis.functions {
        for &a in &f.instructions {
            if f.mode == Mode::Thumb {
                let h = a & !1;
                mark(h, 2);
                if let Some(first) = program.text.read16(h) {
                    if (0xF000..0xF800).contains(&first) {
                        mark(h + 2, 2);
                    }
                }
            } else {
                mark(a, 1);
                mark(a + 2, 1);
            }
        }
        let _ = entry;
    }
    std::fs::write(format!("{}.modes", args[2]), modes).expect("no se pudo escribir el mapa de modos");
    let words: Vec<u8> = (0..analysis.map.len()).step_by(4).map(|i| (analysis.map[i] == discover::Byte::Code) as u8).collect();
    std::fs::write(format!("{}.map", args[2]), words).expect("no se pudo escribir el mapa");
    eprintln!("texto: base=0x{:08X} fin=0x{:08X}  hash_codigo={:016X}", program.text.base, program.text.end(), hash);
    eprintln!("funciones: {} (antes de hints: {first}); hints aplicados: {applied}", analysis.functions.len());
    eprintln!("thumb: {thumb}");
    eprintln!("por origen: {by_source:?}");
}
