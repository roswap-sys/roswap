// SPDX-License-Identifier: GPL-2.0
use crate::controller::ChunkReport;
use crate::proto::cluster_service_client::ClusterServiceClient;
use crate::proto::{ChunkDesc, HeartbeatRequest, JoinRequest};
use crate::provider::ChunkExport;
use log::warn;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::JoinHandle;
use std::time::Duration;
use tonic::Request;

pub fn spawn(
    addr: SocketAddr,
    node_id: u16,
    epoch: u32,
    listen: String,
    chunks: Vec<ChunkExport>,
    stop: Arc<AtomicBool>,
) -> JoinHandle<()> {
    std::thread::spawn(move || {
        let chunks: Vec<ChunkReport> = chunks
            .into_iter()
            .map(|chunk| ChunkReport {
                gid: chunk.gid,
                epoch: chunk.epoch,
                raddr: chunk.raddr,
                rkey: chunk.rkey,
                size_mb: chunk.size_mb,
            })
            .collect();
        let rt = match tokio::runtime::Builder::new_current_thread()
            .enable_io()
            .enable_time()
            .build()
        {
            Ok(rt) => rt,
            Err(err) => {
                warn!("registration runtime failed: {err}");
                return;
            }
        };
        rt.block_on(register_loop(addr, node_id, epoch, listen, chunks, stop));
    })
}

async fn register_loop(
    addr: SocketAddr,
    node_id: u16,
    epoch: u32,
    listen: String,
    chunks: Vec<ChunkReport>,
    stop: Arc<AtomicBool>,
) {
    while !stop.load(Ordering::Relaxed) {
        if let Err(err) = one_session(addr, node_id, epoch, &listen, &chunks, &stop).await {
            warn!("controller registration failed: {err}");
        }
        if wait_stop(&stop, Duration::from_secs(1)).await {
            break;
        }
    }
}

async fn one_session(
    addr: SocketAddr,
    node_id: u16,
    epoch: u32,
    listen: &str,
    chunks: &[ChunkReport],
    stop: &AtomicBool,
) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let mut client = ClusterServiceClient::connect(format!("http://{addr}")).await?;
    client
        .join(Request::new(JoinRequest {
            node_id: u32::from(node_id),
            node_epoch: epoch,
            listen_addr: listen.to_string(),
        }))
        .await?;
    let descs: Vec<ChunkDesc> = chunks
        .iter()
        .map(|chunk| ChunkDesc {
            gid: chunk.gid,
            epoch: chunk.epoch,
            raddr: chunk.raddr,
            rkey: chunk.rkey,
            size_mb: chunk.size_mb,
        })
        .collect();
    while !stop.load(Ordering::Relaxed) {
        client
            .heartbeat(Request::new(HeartbeatRequest {
                node_id: u32::from(node_id),
                node_epoch: epoch,
                listen_addr: listen.to_string(),
                chunks: descs.clone(),
            }))
            .await?;
        if wait_stop(stop, Duration::from_secs(1)).await {
            break;
        }
    }
    Ok(())
}

async fn wait_stop(stop: &AtomicBool, dur: Duration) -> bool {
    let step = Duration::from_millis(50);
    let mut left = dur;
    while left > Duration::ZERO {
        if stop.load(Ordering::Relaxed) {
            return true;
        }
        let slice = step.min(left);
        tokio::time::sleep(slice).await;
        left = left.saturating_sub(slice);
    }
    stop.load(Ordering::Relaxed)
}
