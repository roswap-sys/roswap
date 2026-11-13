// SPDX-License-Identifier: GPL-2.0
use crate::genl::{is_already_exists, GenlClient};
use crate::proto::cluster_service_server::{ClusterService, ClusterServiceServer};
use crate::proto::{
    ChunkDesc, GetClusterRequest, GetClusterResponse, HeartbeatRequest, HeartbeatResponse,
    JoinRequest, JoinResponse, NodeView, ReportChunksRequest, ReportChunksResponse,
};
use log::info;
use std::collections::BTreeMap;
use std::io;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tonic::{Request, Response, Status};

use crate::config::ControllerConfig;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ChunkReport {
    pub gid: u32,
    pub epoch: u32,
    pub raddr: u64,
    pub rkey: u32,
    pub size_mb: u32,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum KernelOp {
    NodeAdd {
        node_id: u16,
        addr: String,
    },
    ChunkAdd {
        node_id: u16,
        gid: u32,
        epoch: u32,
        raddr: u64,
        rkey: u32,
        size_mb: u32,
    },
    ChunkUpdate {
        gid: u32,
        epoch: u32,
        raddr: u64,
        rkey: u32,
    },
}

#[derive(Clone, Debug, Default)]
struct ChunkState {
    desired: ChunkReport,
    kernel: Option<ChunkReport>,
}

#[derive(Clone, Debug, Default)]
struct NodeState {
    epoch: u32,
    listen: String,
    kernel_listen: Option<String>,
    chunks: BTreeMap<u32, ChunkState>,
}

#[derive(Default)]
pub struct Registry {
    nodes: BTreeMap<u16, NodeState>,
}

impl Registry {
    pub fn upsert(&mut self, node_id: u16, epoch: u32, listen: &str, chunks: &[ChunkReport]) {
        let node = self.nodes.entry(node_id).or_default();
        node.epoch = epoch;
        if node.listen != listen {
            node.listen = listen.to_string();
            if node.kernel_listen.as_deref() != Some(listen) {
                node.kernel_listen = None;
            }
        }
        for chunk in chunks {
            let state = node.chunks.entry(chunk.gid).or_default();
            state.desired = chunk.clone();
        }
    }

    pub fn pending(&self) -> Vec<KernelOp> {
        let mut ops = Vec::new();
        for (node_id, node) in &self.nodes {
            if node.listen.is_empty() {
                continue;
            }
            if node.kernel_listen.as_deref() != Some(node.listen.as_str()) {
                ops.push(KernelOp::NodeAdd {
                    node_id: *node_id,
                    addr: node.listen.clone(),
                });
            }
            for state in node.chunks.values() {
                match &state.kernel {
                    Some(installed) if installed == &state.desired => {}
                    Some(_) => ops.push(KernelOp::ChunkUpdate {
                        gid: state.desired.gid,
                        epoch: state.desired.epoch,
                        raddr: state.desired.raddr,
                        rkey: state.desired.rkey,
                    }),
                    None => ops.push(KernelOp::ChunkAdd {
                        node_id: *node_id,
                        gid: state.desired.gid,
                        epoch: state.desired.epoch,
                        raddr: state.desired.raddr,
                        rkey: state.desired.rkey,
                        size_mb: state.desired.size_mb,
                    }),
                }
            }
        }
        ops
    }

    pub fn commit(&mut self, op: &KernelOp) {
        match op {
            KernelOp::NodeAdd { node_id, addr } => {
                if let Some(node) = self.nodes.get_mut(node_id) {
                    node.kernel_listen = Some(addr.clone());
                }
            }
            KernelOp::ChunkAdd {
                node_id,
                gid,
                epoch,
                raddr,
                rkey,
                size_mb,
            } => {
                if let Some(state) = self
                    .nodes
                    .get_mut(node_id)
                    .and_then(|node| node.chunks.get_mut(gid))
                {
                    state.kernel = Some(ChunkReport {
                        gid: *gid,
                        epoch: *epoch,
                        raddr: *raddr,
                        rkey: *rkey,
                        size_mb: *size_mb,
                    });
                }
            }
            KernelOp::ChunkUpdate {
                gid,
                epoch,
                raddr,
                rkey,
            } => {
                for node in self.nodes.values_mut() {
                    if let Some(state) = node.chunks.get_mut(gid) {
                        let size_mb = state.desired.size_mb;
                        state.kernel = Some(ChunkReport {
                            gid: *gid,
                            epoch: *epoch,
                            raddr: *raddr,
                            rkey: *rkey,
                            size_mb,
                        });
                    }
                }
            }
        }
    }

    fn views(&self) -> Vec<NodeView> {
        self.nodes
            .iter()
            .map(|(id, node)| {
                let chunks: Vec<ChunkDesc> = node
                    .chunks
                    .values()
                    .map(|state| ChunkDesc {
                        gid: state.desired.gid,
                        epoch: state.desired.epoch,
                        raddr: state.desired.raddr,
                        rkey: state.desired.rkey,
                        size_mb: state.desired.size_mb,
                    })
                    .collect();
                let kernel_synced = node.kernel_listen.as_deref() == Some(node.listen.as_str())
                    && !node.listen.is_empty()
                    && node
                        .chunks
                        .values()
                        .all(|state| state.kernel.as_ref() == Some(&state.desired));
                NodeView {
                    node_id: u32::from(*id),
                    node_epoch: node.epoch,
                    listen_addr: node.listen.clone(),
                    chunks,
                    kernel_synced,
                }
            })
            .collect()
    }
}

pub struct Controller {
    registry: Registry,
    genl: GenlClient,
}

impl Controller {
    pub fn open() -> io::Result<Self> {
        Ok(Self {
            registry: Registry::default(),
            genl: GenlClient::open()?,
        })
    }

    pub fn join(&mut self, node_id: u16, epoch: u32, listen: &str) {
        self.registry.upsert(node_id, epoch, listen, &[]);
    }

    pub fn report(
        &mut self,
        node_id: u16,
        epoch: u32,
        listen: &str,
        chunks: &[ChunkReport],
    ) -> io::Result<()> {
        self.registry.upsert(node_id, epoch, listen, chunks);
        self.sync_kernel()
    }

    fn sync_kernel(&mut self) -> io::Result<()> {
        let ops = self.registry.pending();
        for op in ops {
            match &op {
                KernelOp::NodeAdd { node_id, addr } => {
                    self.genl.node_add(*node_id, addr)?;
                }
                KernelOp::ChunkAdd {
                    node_id,
                    gid,
                    epoch,
                    raddr,
                    rkey,
                    size_mb,
                } => {
                    if let Err(err) = self
                        .genl
                        .chunk_add(*node_id, *gid, *epoch, *raddr, *rkey, *size_mb)
                    {
                        if !is_already_exists(&err) {
                            return Err(err);
                        }
                        self.genl.chunk_update(*gid, *epoch, *raddr, *rkey)?;
                    }
                }
                KernelOp::ChunkUpdate {
                    gid,
                    epoch,
                    raddr,
                    rkey,
                } => self.genl.chunk_update(*gid, *epoch, *raddr, *rkey)?,
            }
            self.registry.commit(&op);
        }
        Ok(())
    }
}

struct Svc {
    controller: Arc<Mutex<Controller>>,
}

fn chunks_from(descs: &[ChunkDesc]) -> Vec<ChunkReport> {
    descs
        .iter()
        .map(|desc| ChunkReport {
            gid: desc.gid,
            epoch: desc.epoch,
            raddr: desc.raddr,
            rkey: desc.rkey,
            size_mb: desc.size_mb,
        })
        .collect()
}

#[tonic::async_trait]
impl ClusterService for Svc {
    async fn join(&self, request: Request<JoinRequest>) -> Result<Response<JoinResponse>, Status> {
        let req = request.into_inner();
        let node_id =
            u16::try_from(req.node_id).map_err(|_| Status::invalid_argument("node_id"))?;
        self.controller
            .lock()
            .unwrap_or_else(|err| err.into_inner())
            .join(node_id, req.node_epoch, &req.listen_addr);
        info!("node {node_id} joined from {}", req.listen_addr);
        Ok(Response::new(JoinResponse {}))
    }

    async fn report_chunks(
        &self,
        request: Request<ReportChunksRequest>,
    ) -> Result<Response<ReportChunksResponse>, Status> {
        let req = request.into_inner();
        self.push(req.node_id, req.node_epoch, req.listen_addr, req.chunks)?;
        Ok(Response::new(ReportChunksResponse {}))
    }

    async fn heartbeat(
        &self,
        request: Request<HeartbeatRequest>,
    ) -> Result<Response<HeartbeatResponse>, Status> {
        let req = request.into_inner();
        self.push(req.node_id, req.node_epoch, req.listen_addr, req.chunks)?;
        Ok(Response::new(HeartbeatResponse {}))
    }

    async fn get_cluster(
        &self,
        _request: Request<GetClusterRequest>,
    ) -> Result<Response<GetClusterResponse>, Status> {
        let views = self
            .controller
            .lock()
            .unwrap_or_else(|err| err.into_inner())
            .registry
            .views();
        Ok(Response::new(GetClusterResponse { nodes: views }))
    }
}

impl Svc {
    fn push(
        &self,
        node_id: u32,
        epoch: u32,
        listen: String,
        chunks: Vec<ChunkDesc>,
    ) -> Result<(), Status> {
        let node_id = u16::try_from(node_id).map_err(|_| Status::invalid_argument("node_id"))?;
        let chunks = chunks_from(&chunks);
        self.controller
            .lock()
            .unwrap_or_else(|err| err.into_inner())
            .report(node_id, epoch, &listen, &chunks)
            .map_err(|err| Status::unavailable(err.to_string()))
    }
}

pub fn run(cfg: &ControllerConfig, stop: Arc<AtomicBool>) -> io::Result<()> {
    let controller = Arc::new(Mutex::new(Controller::open()?));
    info!("controller listening on {}", cfg.listen_addr);
    let rt = tokio::runtime::Builder::new_current_thread()
        .enable_io()
        .enable_time()
        .build()?;
    rt.block_on(async {
        tonic::transport::Server::builder()
            .add_service(ClusterServiceServer::new(Svc { controller }))
            .serve_with_shutdown(cfg.listen_addr, wait_stop(stop))
            .await
    })
    .map_err(io::Error::other)
}

async fn wait_stop(stop: Arc<AtomicBool>) {
    while !stop.load(Ordering::Relaxed) {
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn chunk(gid: u32, rkey: u32) -> ChunkReport {
        ChunkReport {
            gid,
            epoch: 3,
            raddr: 0x1000 + u64::from(gid),
            rkey,
            size_mb: 64,
        }
    }

    #[test]
    fn first_report_adds_node_and_chunk() {
        let mut registry = Registry::default();
        registry.upsert(2, 3, "10.0.0.2:18515", &[chunk(0, 9)]);
        assert_eq!(
            registry.pending(),
            vec![
                KernelOp::NodeAdd {
                    node_id: 2,
                    addr: "10.0.0.2:18515".to_string(),
                },
                KernelOp::ChunkAdd {
                    node_id: 2,
                    gid: 0,
                    epoch: 3,
                    raddr: 0x1000,
                    rkey: 9,
                    size_mb: 64,
                },
            ]
        );
    }

    #[test]
    fn committed_chunk_updates_only_the_rkey() {
        let mut registry = Registry::default();
        registry.upsert(2, 3, "10.0.0.2:18515", &[chunk(0, 9)]);
        for op in registry.pending() {
            registry.commit(&op);
        }
        assert!(registry.pending().is_empty());
        registry.upsert(2, 3, "10.0.0.2:18515", &[chunk(0, 10)]);
        assert_eq!(
            registry.pending(),
            vec![KernelOp::ChunkUpdate {
                gid: 0,
                epoch: 3,
                raddr: 0x1000,
                rkey: 10,
            }]
        );
    }

    #[test]
    fn view_is_synced_only_after_commit() {
        let mut registry = Registry::default();
        registry.upsert(2, 3, "10.0.0.2:18515", &[chunk(0, 9)]);
        assert!(!registry.views()[0].kernel_synced);
        for op in registry.pending() {
            registry.commit(&op);
        }
        let view = &registry.views()[0];
        assert!(view.kernel_synced);
        assert_eq!(view.chunks[0].rkey, 9);
    }

    fn commit_all(registry: &mut Registry) {
        for op in registry.pending() {
            registry.commit(&op);
        }
    }

    #[test]
    fn nodes_are_ordered_and_a_second_chunk_does_not_repeat_node_add() {
        let mut registry = Registry::default();
        registry.upsert(5, 1, "10.0.0.5:18515", &[chunk(1, 1)]);
        registry.upsert(1, 1, "10.0.0.1:18515", &[chunk(0, 1)]);
        let ids: Vec<u16> = registry
            .pending()
            .into_iter()
            .filter_map(|op| match op {
                KernelOp::NodeAdd { node_id, .. } => Some(node_id),
                _ => None,
            })
            .collect();
        assert_eq!(ids, vec![1, 5]);
        commit_all(&mut registry);
        registry.upsert(1, 1, "10.0.0.1:18515", &[chunk(0, 1), chunk(2, 4)]);
        assert_eq!(
            registry.pending(),
            vec![KernelOp::ChunkAdd {
                node_id: 1,
                gid: 2,
                epoch: 3,
                raddr: 0x1002,
                rkey: 4,
                size_mb: 64,
            }]
        );
    }

    #[test]
    fn listen_change_repeats_node_add_and_keeps_installed_chunks() {
        let mut registry = Registry::default();
        registry.upsert(2, 3, "10.0.0.2:18515", &[chunk(0, 9)]);
        commit_all(&mut registry);
        registry.upsert(2, 4, "10.0.0.9:18515", &[chunk(0, 9)]);
        assert_eq!(
            registry.pending(),
            vec![KernelOp::NodeAdd {
                node_id: 2,
                addr: "10.0.0.9:18515".to_string(),
            }]
        );
        assert!(!registry.views()[0].kernel_synced);
        assert_eq!(registry.views()[0].node_epoch, 4);
        commit_all(&mut registry);
        assert!(registry.views()[0].kernel_synced);
    }

    #[test]
    fn size_change_updates_and_a_missing_chunk_stays_installed() {
        let mut registry = Registry::default();
        let mut bigger = chunk(0, 9);
        registry.upsert(2, 3, "10.0.0.2:18515", &[chunk(0, 9), chunk(1, 8)]);
        commit_all(&mut registry);
        bigger.size_mb = 256;
        registry.upsert(2, 3, "10.0.0.2:18515", &[bigger]);
        assert_eq!(
            registry.pending(),
            vec![KernelOp::ChunkUpdate {
                gid: 0,
                epoch: 3,
                raddr: 0x1000,
                rkey: 9,
            }]
        );
        commit_all(&mut registry);
        let view = &registry.views()[0];
        assert!(view.kernel_synced);
        assert_eq!(view.chunks.len(), 2);
        assert_eq!(view.chunks[0].size_mb, 256);
        assert_eq!(view.chunks[1].rkey, 8);
    }

    #[test]
    fn empty_listen_holds_the_chunk_until_an_address_arrives() {
        let mut registry = Registry::default();
        registry.upsert(1, 1, "", &[chunk(0, 1)]);
        assert!(registry.pending().is_empty());
        assert!(!registry.views()[0].kernel_synced);
        registry.upsert(1, 1, "10.0.0.1:18515", &[]);
        assert_eq!(registry.pending().len(), 2);
        commit_all(&mut registry);
        let view = &registry.views()[0];
        assert!(view.kernel_synced);
        assert_eq!(view.chunks.len(), 1);
        assert_eq!(view.chunks[0].rkey, 1);
    }

    #[test]
    fn a_node_without_chunks_can_sync() {
        let mut registry = Registry::default();
        registry.upsert(1, 1, "10.0.0.1:18515", &[]);
        commit_all(&mut registry);
        let view = &registry.views()[0];
        assert!(view.kernel_synced);
        assert!(view.chunks.is_empty());
    }

    #[test]
    fn identical_report_after_commit_is_idle() {
        let mut registry = Registry::default();
        registry.upsert(2, 3, "10.0.0.2:18515", &[chunk(0, 9)]);
        commit_all(&mut registry);
        registry.upsert(2, 3, "10.0.0.2:18515", &[chunk(0, 9)]);
        assert!(registry.pending().is_empty());
        assert!(Registry::default().views().is_empty());
    }

    #[tokio::test]
    async fn join_is_visible_before_any_kernel_command() {
        let controller = Arc::new(Mutex::new(Controller::open().expect("netlink socket")));
        let svc = Svc {
            controller: Arc::clone(&controller),
        };
        svc.join(Request::new(JoinRequest {
            node_id: 4,
            node_epoch: 9,
            listen_addr: "10.1.1.4:18515".to_string(),
        }))
        .await
        .unwrap();
        let view = svc
            .get_cluster(Request::new(GetClusterRequest {}))
            .await
            .unwrap()
            .into_inner();
        assert_eq!(view.nodes.len(), 1);
        assert_eq!(view.nodes[0].node_id, 4);
        assert_eq!(view.nodes[0].node_epoch, 9);
        assert_eq!(view.nodes[0].listen_addr, "10.1.1.4:18515");
        assert!(!view.nodes[0].kernel_synced);
        assert!(view.nodes[0].chunks.is_empty());

        let rejected = svc
            .heartbeat(Request::new(HeartbeatRequest {
                node_id: u32::from(u16::MAX) + 1,
                node_epoch: 1,
                listen_addr: "10.1.1.4:18515".to_string(),
                chunks: Vec::new(),
            }))
            .await
            .unwrap_err();
        assert_eq!(rejected.code(), tonic::Code::InvalidArgument);
    }
}
