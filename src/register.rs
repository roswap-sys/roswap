// SPDX-License-Identifier: GPL-2.0
use crate::controller::ChunkReport;
use crate::genl::NodeHb;
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
    hb: NodeHb,
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
        rt.block_on(register_loop(
            addr, node_id, epoch, listen, chunks, hb, stop,
        ));
    })
}

async fn register_loop(
    addr: SocketAddr,
    node_id: u16,
    epoch: u32,
    listen: String,
    chunks: Vec<ChunkReport>,
    hb: NodeHb,
    stop: Arc<AtomicBool>,
) {
    while !stop.load(Ordering::Relaxed) {
        if let Err(err) = one_session(addr, node_id, epoch, &listen, &chunks, hb, &stop).await {
            warn!("controller registration failed: {err}");
        }
        if wait_stop(&stop, Duration::from_secs(1)).await {
            break;
        }
    }
}

pub(crate) fn join_request(node_id: u16, epoch: u32, listen: &str, hb: NodeHb) -> JoinRequest {
    JoinRequest {
        node_id: u32::from(node_id),
        node_epoch: epoch,
        listen_addr: listen.to_string(),
        hb_raddr: hb.raddr,
        hb_rkey: hb.rkey,
        boot_id_lo: hb.boot_id,
    }
}

pub(crate) fn heartbeat_request(
    node_id: u16,
    epoch: u32,
    listen: &str,
    chunks: &[ChunkReport],
    hb: NodeHb,
) -> HeartbeatRequest {
    HeartbeatRequest {
        node_id: u32::from(node_id),
        node_epoch: epoch,
        listen_addr: listen.to_string(),
        chunks: chunks
            .iter()
            .map(|chunk| ChunkDesc {
                gid: chunk.gid,
                epoch: chunk.epoch,
                raddr: chunk.raddr,
                rkey: chunk.rkey,
                size_mb: chunk.size_mb,
            })
            .collect(),
        hb_raddr: hb.raddr,
        hb_rkey: hb.rkey,
        boot_id_lo: hb.boot_id,
    }
}

async fn one_session(
    addr: SocketAddr,
    node_id: u16,
    epoch: u32,
    listen: &str,
    chunks: &[ChunkReport],
    hb: NodeHb,
    stop: &AtomicBool,
) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let mut client = ClusterServiceClient::connect(format!("http://{addr}")).await?;
    client
        .join(Request::new(join_request(node_id, epoch, listen, hb)))
        .await?;
    while !stop.load(Ordering::Relaxed) {
        client
            .heartbeat(Request::new(heartbeat_request(
                node_id, epoch, listen, chunks, hb,
            )))
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::proto::cluster_service_server::{ClusterService, ClusterServiceServer};
    use crate::proto::{
        GetClusterRequest, GetClusterResponse, HeartbeatResponse, JoinResponse,
        ReportChunksRequest, ReportChunksResponse,
    };
    use std::sync::atomic::AtomicUsize;
    use std::time::Duration;
    use tonic::{Response, Status};

    #[test]
    fn heartbeat_carries_the_remote_page() {
        let chunk = ChunkReport {
            gid: 1,
            epoch: 2,
            raddr: 3,
            rkey: 4,
            size_mb: 5,
        };
        let hb = NodeHb {
            raddr: 0xabc,
            rkey: 9,
            boot_id: 42,
        };
        let request = heartbeat_request(7, 2, "10.0.0.7:18515", &[chunk], hb);
        assert_eq!(request.hb_raddr, 0xabc);
        assert_eq!(request.hb_rkey, 9);
        assert_eq!(request.boot_id_lo, 42);
        assert_eq!(request.chunks.len(), 1);
        let join = join_request(7, 2, "10.0.0.7:18515", hb);
        assert_eq!(join.hb_raddr, 0xabc);
        assert_eq!(join.boot_id_lo, 42);
    }

    struct Gate {
        open: Arc<AtomicBool>,
        joins: Arc<AtomicUsize>,
        beats: Arc<AtomicUsize>,
    }

    #[tonic::async_trait]
    impl ClusterService for Gate {
        async fn join(
            &self,
            _request: tonic::Request<crate::proto::JoinRequest>,
        ) -> Result<Response<JoinResponse>, Status> {
            if !self.open.load(Ordering::Relaxed) {
                return Err(Status::unavailable("closed"));
            }
            self.joins.fetch_add(1, Ordering::Relaxed);
            Ok(Response::new(JoinResponse {}))
        }

        async fn report_chunks(
            &self,
            _request: tonic::Request<ReportChunksRequest>,
        ) -> Result<Response<ReportChunksResponse>, Status> {
            Ok(Response::new(ReportChunksResponse {}))
        }

        async fn heartbeat(
            &self,
            _request: tonic::Request<crate::proto::HeartbeatRequest>,
        ) -> Result<Response<HeartbeatResponse>, Status> {
            if !self.open.load(Ordering::Relaxed) {
                return Err(Status::unavailable("closed"));
            }
            self.beats.fetch_add(1, Ordering::Relaxed);
            Ok(Response::new(HeartbeatResponse {}))
        }

        async fn get_cluster(
            &self,
            _request: tonic::Request<GetClusterRequest>,
        ) -> Result<Response<GetClusterResponse>, Status> {
            Ok(Response::new(GetClusterResponse { nodes: Vec::new() }))
        }
    }

    #[tokio::test]
    async fn registration_retries_across_a_closed_controller() {
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        let open = Arc::new(AtomicBool::new(false));
        let joins = Arc::new(AtomicUsize::new(0));
        let beats = Arc::new(AtomicUsize::new(0));
        let gate = Gate {
            open: Arc::clone(&open),
            joins: Arc::clone(&joins),
            beats: Arc::clone(&beats),
        };
        let stop_srv = Arc::new(AtomicBool::new(false));
        let stop_wait = Arc::clone(&stop_srv);
        let server = tokio::spawn(async move {
            tonic::transport::Server::builder()
                .add_service(ClusterServiceServer::new(gate))
                .serve_with_incoming_shutdown(
                    tokio_stream::wrappers::TcpListenerStream::new(listener),
                    async move {
                        while !stop_wait.load(Ordering::Relaxed) {
                            tokio::time::sleep(Duration::from_millis(20)).await;
                        }
                    },
                )
                .await
        });
        let stop = Arc::new(AtomicBool::new(false));
        let stop_reg = Arc::clone(&stop);
        let hb = crate::genl::NodeHb {
            raddr: 0x1000,
            rkey: 1,
            boot_id: 7,
        };
        let worker = tokio::spawn(async move {
            register_loop(
                addr,
                3,
                1,
                "10.0.0.3:18515".to_string(),
                Vec::new(),
                hb,
                stop_reg,
            )
            .await;
        });
        tokio::time::sleep(Duration::from_millis(200)).await;
        assert_eq!(joins.load(Ordering::Relaxed), 0);
        open.store(true, Ordering::Relaxed);
        let mut ready = false;
        for _ in 0..40 {
            if joins.load(Ordering::Relaxed) >= 1 && beats.load(Ordering::Relaxed) >= 1 {
                ready = true;
                break;
            }
            tokio::time::sleep(Duration::from_millis(50)).await;
        }
        assert!(ready, "registration did not recover");
        let seen = beats.load(Ordering::Relaxed);
        open.store(false, Ordering::Relaxed);
        tokio::time::sleep(Duration::from_millis(300)).await;
        open.store(true, Ordering::Relaxed);
        let mut grew = false;
        for _ in 0..40 {
            if beats.load(Ordering::Relaxed) > seen {
                grew = true;
                break;
            }
            tokio::time::sleep(Duration::from_millis(50)).await;
        }
        assert!(grew, "heartbeat did not resume");
        stop.store(true, Ordering::Relaxed);
        tokio::time::timeout(Duration::from_secs(3), worker)
            .await
            .expect("register loop")
            .unwrap();
        stop_srv.store(true, Ordering::Relaxed);
        tokio::time::timeout(Duration::from_secs(2), server)
            .await
            .expect("server")
            .unwrap()
            .unwrap();
    }
}
