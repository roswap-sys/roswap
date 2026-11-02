// SPDX-License-Identifier: GPL-2.0
use log::{error, info, warn};
use roswap::config::{Config, Role};
use roswap::provider::ProviderRuntime;
use std::env;
use std::io::IsTerminal;
use std::process;
use std::sync::atomic::AtomicBool;
use std::sync::Arc;



fn main() {
    init_tracing();

    let cfg = init_config();

    init_roswapd_runtime(cfg);

}

fn init_roswapd_runtime(cfg:  Config){
    match cfg.role {
        Role::Provider => {
            let mut runtime = ProviderRuntime::init(cfg.provider).unwrap_or_else(|e| {
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
        Role::Controller => {
            warn!("controller role is a placeholder");
            error!("unsupported role: {}", cfg.role);
            process::exit(1);
        }
    }
}


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

fn init_config()->Config{

    let mut args = env::args();
    let prog = args.next().unwrap_or_else(|| "roswapd".to_string());
    let config_path = match (args.next(), args.next()) {
        (Some(path), None) => path,
        _ => {
            eprintln!("Usage: {prog} <config.toml>");
            process::exit(1);
        }
    };

    let cfg = Config::load(&config_path).unwrap_or_else(|e| {
        error!("failed to load {config_path}: {e}");
        process::exit(1);
    });
    info!("roswapd starting role={} config={config_path}", cfg.role);
    cfg
}