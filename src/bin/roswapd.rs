// SPDX-License-Identifier: GPL-2.0
use log::{error, info, warn};
use roswap::provider::{ProviderConfig, ProviderRuntime};
use std::env;
use std::net::SocketAddrV4;
use std::path::PathBuf;
use std::process;
use std::io::IsTerminal;
use std::sync::atomic::AtomicBool;
use std::sync::Arc;

fn init_tracing() {
    let (level, log_filter) = match env::var("RUST_LOG").ok().as_deref() {
        Some("error") => (tracing::Level::ERROR, log::LevelFilter::Error),
        Some("warn") => (tracing::Level::WARN, log::LevelFilter::Warn),
        Some("debug") => (tracing::Level::DEBUG, log::LevelFilter::Debug),
        Some("trace") => (tracing::Level::TRACE, log::LevelFilter::Trace),
        _ => (tracing::Level::INFO, log::LevelFilter::Info),
    };
    tracing_subscriber::fmt()
        .with_max_level(level)
        .with_target(true)
        .with_ansi(std::io::stderr().is_terminal())
        .with_writer(std::io::stderr)
        .init();
    // log 宏转给上面的 tracing 订阅器。本机 rust-analyzer 展开不了 tracing 的 event 宏。
    let _ = tracing_log::LogTracer::builder()
        .with_max_level(log_filter)
        .init();
}

fn print_usage(prog: &str) {
    eprintln!(
        "Usage: {prog} 
            [--role provider|controller] 
            [--device <dev>] 
            [--listen <ip:port>] 
            [--mem-dir <path>] 
            [--chunk-size-mb <mb>] 
            [--chunks <n>] 
            [--epoch <e>]"
    );
}


fn main() {
    init_tracing();

    let args: Vec<String> = env::args().collect();
    let prog = args.first().map(|s| s.as_str()).unwrap_or("roswapd");

    let mut role = String::from("provider");
    let mut cfg = ProviderConfig::default();

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--role" => {
                i += 1;
                if i >= args.len() {
                    print_usage(prog);
                    process::exit(1);
                }
                role = args[i].clone();
            }
            "--device" => {
                i += 1;
                if i >= args.len() {
                    print_usage(prog);
                    process::exit(1);
                }
                cfg.device_name = Some(args[i].clone());
            }
            "--listen" => {
                i += 1;
                if i >= args.len() {
                    print_usage(prog);
                    process::exit(1);
                }
                cfg.listen_addr = args[i].parse::<SocketAddrV4>().unwrap_or_else(|e| {
                    error!("invalid --listen address {}: {e}", args[i]);
                    process::exit(1);
                });
            }
            "--mem-dir" => {
                i += 1;
                if i >= args.len() {
                    print_usage(prog);
                    process::exit(1);
                }
                cfg.mem_dir = PathBuf::from(&args[i]);
            }
            "--chunk-size-mb" => {
                i += 1;
                if i >= args.len() {
                    print_usage(prog);
                    process::exit(1);
                }
                cfg.chunk_size_mb = args[i].parse::<usize>().unwrap_or_else(|e| {
                    error!("invalid --chunk-size-mb {}: {e}", args[i]);
                    process::exit(1);
                });
            }
            "--chunks" => {
                i += 1;
                if i >= args.len() {
                    print_usage(prog);
                    process::exit(1);
                }
                cfg.num_chunks = args[i].parse::<usize>().unwrap_or_else(|e| {
                    error!("invalid --chunks {}: {e}", args[i]);
                    process::exit(1);
                });
            }
            "--epoch" => {
                i += 1;
                if i >= args.len() {
                    print_usage(prog);
                    process::exit(1);
                }
                cfg.node_epoch = args[i].parse::<u32>().unwrap_or_else(|e| {
                    error!("invalid --epoch {}: {e}", args[i]);
                    process::exit(1);
                });
            }
            "-h" | "--help" => {
                print_usage(prog);
                return;
            }
            other => {
                error!("unknown option: {other}");
                print_usage(prog);
                process::exit(1);
            }
        }
        i += 1;
    }

    info!("roswapd starting role={role}");

    match role.as_str() {
        "provider" => {
            let mut runtime = ProviderRuntime::init(cfg).unwrap_or_else(|e| {
                error!("provider init failed: {e}");
                process::exit(1);
            });
            runtime.log_summary();

            let stop = Arc::new(AtomicBool::new(false));
            if let Err(e) = runtime.run_until_stopped(stop) {
                error!("provider runtime error: {e}");
                process::exit(1);
            }
        }
        "controller" => {
            warn!("controller role is a placeholder");
            error!("unsupported role: {role}");
        }
        _ => {
            error!("unsupported role: {role}");
            print_usage(prog);
            process::exit(1);
        }
    }
}
