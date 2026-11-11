// SPDX-License-Identifier: GPL-2.0
use log::{error, info, warn};
use roswap::config::{Config, Role};
use roswap::provider::ProviderRuntime;
use roswap::rpc::serve_provider;
use roswap::sys;
use std::env;
use std::io::IsTerminal;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::process;
use std::sync::atomic::{AtomicBool, AtomicPtr, Ordering};
use std::sync::Arc;
use std::thread::JoinHandle;

fn main() {
    init_tracing();

    let cfg = init_config();

    init_roswapd_runtime(&cfg);
}

static STOP_FLAG: AtomicPtr<AtomicBool> = AtomicPtr::new(std::ptr::null_mut());

fn publish_stop_flag(stop: &Arc<AtomicBool>) {
    STOP_FLAG.store(Arc::as_ptr(stop) as *mut AtomicBool, Ordering::Release);
}

extern "C" fn on_stop_signal(_sig: i32) {
    let ptr = STOP_FLAG.load(Ordering::Acquire);
    if !ptr.is_null() {
        unsafe { (*ptr).store(true, Ordering::Relaxed) };
    }
}

fn install_stop_signals(stop: &Arc<AtomicBool>) {
    publish_stop_flag(stop);
    unsafe {
        sys::signal(sys::SIGINT, Some(on_stop_signal));
        sys::signal(sys::SIGTERM, Some(on_stop_signal));
    }
}

fn init_roswapd_runtime(cfg: &Config) {
    match cfg.role {
        Role::Provider => {
            let mut runtime = ProviderRuntime::init(&cfg.provider).unwrap_or_else(|e| {
                error!("provider init failed: {e}");
                process::exit(1);
            });
            runtime.log_summary();

            let stop = Arc::new(AtomicBool::new(false));
            install_stop_signals(&stop);
            let grpc_failed = Arc::new(AtomicBool::new(false));
            let grpc = spawn_provider_rpc(
                cfg.provider.grpc_addr,
                Arc::clone(&stop),
                Arc::clone(&grpc_failed),
            );
            let run = runtime.run_until_stopped(Arc::clone(&stop));
            stop.store(true, Ordering::Relaxed);
            let _ = grpc.join();
            if let Err(e) = run {
                error!("provider runtime error: {e}");
                process::exit(1);
            }
            if grpc_failed.load(Ordering::Relaxed) {
                error!("grpc thread failed");
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

fn init_config() -> Config {
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

fn spawn_provider_rpc(
    addr: std::net::SocketAddr,
    stop: Arc<AtomicBool>,
    failed: Arc<AtomicBool>,
) -> JoinHandle<()> {
    info!("grpc service listening on {addr}");
    std::thread::spawn(move || {
        let stop_thread = Arc::clone(&stop);
        let run = catch_unwind(AssertUnwindSafe(|| {
            let rt = tokio::runtime::Builder::new_current_thread()
                .enable_io()
                .enable_time()
                .build()
                .map_err(|e| e.to_string())?;
            rt.block_on(serve_provider(addr, stop_thread))
                .map_err(|e| e.to_string())
        }));
        if !matches!(run, Ok(Ok(()))) {
            error!("grpc thread failed");
            failed.store(true, Ordering::Relaxed);
            stop.store(true, Ordering::Relaxed);
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sighandler_sets_the_published_flag() {
        let stop = Arc::new(AtomicBool::new(false));
        publish_stop_flag(&stop);
        on_stop_signal(sys::SIGTERM);
        assert!(stop.load(Ordering::Relaxed));
    }
}
