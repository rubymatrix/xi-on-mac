// The PlayOnline session is the portable C in ../pol, compiled into the launcher.
fn main() {
    let sources = ["polcrypt.c", "polnet.c", "polsession.c", "pol_ffi.c"];
    let mut build = cc::Build::new();
    build.include("../pol").std("c11");
    for s in sources {
        build.file(format!("../pol/{s}"));
    }
    for h in ["polcrypt.h", "polnet.h", "polsession.h"] {
        println!("cargo:rerun-if-changed=../pol/{h}");
    }
    for s in sources {
        println!("cargo:rerun-if-changed=../pol/{s}");
    }
    if std::env::var("CARGO_CFG_WINDOWS").is_ok() {
        build.define("_CRT_SECURE_NO_WARNINGS", None);
        println!("cargo:rustc-link-lib=ws2_32");
    }
    build.compile("pol");
    tauri_build::build();
}
