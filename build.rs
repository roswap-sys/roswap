fn main() {
    println!("cargo:rustc-link-lib=ibverbs");
    println!("cargo:rustc-link-lib=rdmacm");
    println!("cargo:rerun-if-changed=proto/roswap.proto");
    tonic_prost_build::compile_protos("proto/roswap.proto")
        .expect("compile proto/roswap.proto");
}
