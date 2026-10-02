// Build BOF files as part of `cargo build --release`
use std::env;
use std::fs;
use std::io;
use std::path::PathBuf;
use std::process::Command;

enum Arch {
    X64,
    X86,
}

fn main() {
    println!("cargo:rerun-if-changed=s3-udc2-bof");

    if env::var("PROFILE").as_deref() == Ok("release") {
        build_bof("s3-udc2", Arch::X64);
        build_bof("s3-udc2", Arch::X86);
        build_bof("s3-udc2-patchable", Arch::X64);
        build_bof("s3-udc2-patchable", Arch::X86);
        build_bof("s3-udc2-migrate", Arch::X64);
        build_bof("s3-udc2-migrate", Arch::X86);
    }
}

fn build_bof(bof_name: &str, arch: Arch) {
    let out_dir = PathBuf::from("s3-udc2-bof/bofs");
    fs::create_dir_all(&out_dir).expect("Could not create folder: s3-udc2-bof/bofs");

    let (compiler, obj_file) = match arch {
        Arch::X64 => ("x86_64-w64-mingw32-g++", format!("{bof_name}.x64.o")),
        Arch::X86 => ("i686-w64-mingw32-gcc", format!("{bof_name}.x86.o")),
    };
    let obj_path = out_dir.join(&obj_file);
    let obj_path_str = obj_path.to_str().expect("could not build obj path");

    let result = Command::new(compiler)
        .args([
            "-c",
            &format!("s3-udc2-bof/{bof_name}.cpp"),
            "-o",
            obj_path_str,
        ])
        .status();

    match result {
        Ok(status) => {
            if !status.success() {
                panic!("{compiler} failed with exit status: {status}");
            }
        }

        Err(e) if e.kind() == io::ErrorKind::NotFound => {
            println!(
                "cargo:warning=Compiler '{compiler}' not found; \
                 skipping {obj_file} generation"
            );
        }

        Err(e) => {
            panic!("Failed to invoke {compiler}: {e}");
        }
    }
}
