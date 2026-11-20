// SPDX-License-Identifier: GPL-2.0
use crate::genl::{is_already_exists, GenlClient, KernelEvent, NodeHb, NodeInfo};
use crate::proto::cluster_service_server::{ClusterService, ClusterServiceServer};
use crate::proto::{
    ChunkDesc, GetClusterRequest, GetClusterResponse, HeartbeatRequest, HeartbeatResponse,
    JoinRequest, JoinResponse, NodeView, ReportChunksRequest, ReportChunksResponse,
};
use log::{info, warn};
use std::collections::{BTreeMap, BTreeSet};
use std::io;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
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
        node_id: u16,
        gid: u32,
        epoch: u32,
        raddr: u64,
        rkey: u32,
    },
    NodeSetState {
        node_id: u16,
        state: u32,
    },
    ChunkRetire {
        node_id: u16,
        gid: u32,
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
    has_hb: bool,
    hb_raddr: u64,
    hb_rkey: u32,
    boot_id: u64,
    kernel_hb: Option<(u64, u32, u64)>,
    last_grpc_ms: Option<u64>,
    suspect_since_ms: Option<u64>,
    kernel_state: u8,
    boot_changed: bool,
    condemned: bool,
    void_epoch: Option<u32>,
    void_boot: u64,
    dead_sent: bool,
    retired: BTreeSet<u32>,
}

pub struct Registry {
    nodes: BTreeMap<u16, NodeState>,
    dead_after_ms: u64,
    hints: BTreeMap<u16, (u8, u64)>,
}

impl Default for Registry {
    fn default() -> Self {
        Self {
            nodes: BTreeMap::new(),
            dead_after_ms: 10_000,
            hints: BTreeMap::new(),
        }
    }
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

    pub fn set_dead_after_s(&mut self, secs: u64) {
        let secs = if secs == 0 { 10 } else { secs };
        self.dead_after_ms = secs.saturating_mul(1000);
    }

    pub fn condemned(&self, node_id: u16) -> bool {
        self.nodes.get(&node_id).is_some_and(|node| node.condemned)
    }

    pub fn heartbeat_of(&self, node_id: u16) -> Option<NodeHb> {
        let node = self.nodes.get(&node_id)?;
        if !node.has_hb {
            return None;
        }
        Some(NodeHb {
            raddr: node.hb_raddr,
            rkey: node.hb_rkey,
            boot_id: node.boot_id,
        })
    }

    /// Records one gRPC contact. Returns false when the epoch was already voided.
    pub fn observe(
        &mut self,
        node_id: u16,
        epoch: u32,
        listen: &str,
        chunks: &[ChunkReport],
        hb: Option<NodeHb>,
        now_ms: u64,
    ) -> bool {
        let voided = self
            .nodes
            .get(&node_id)
            .and_then(|node| node.void_epoch.map(|epoch| (epoch, node.void_boot)));
        if let Some((voided, void_boot)) = voided {
            let fresh_boot = hb.is_some_and(|item| item.boot_id != 0 && item.boot_id != void_boot);
            if voided == epoch && !fresh_boot {
                return false;
            }
            self.nodes.remove(&node_id);
            self.hints.remove(&node_id);
        }
        self.upsert(node_id, epoch, listen, chunks);
        if let Some(node) = self.nodes.get_mut(&node_id) {
            if let Some(hb) = hb {
                note_hb(node, hb);
            }
            node.last_grpc_ms = Some(now_ms);
        }
        self.apply_hint(node_id);
        true
    }

    pub fn note_kernel(&mut self, node_id: u16, state: u32, boot_id: u64, now_ms: u64) {
        let Some(node) = self.nodes.get_mut(&node_id) else {
            return;
        };
        if boot_id != 0 && node.boot_id != 0 && boot_id != node.boot_id {
            node.boot_changed = true;
            condemn(node);
        } else if boot_id != 0 && node.boot_id == 0 {
            node.boot_id = boot_id;
        }
        if node.condemned {
            if state == crate::genl::RS_NODE_DEAD {
                node.kernel_state = state as u8;
            }
            return;
        }
        node.kernel_state = state as u8;
        if state == crate::genl::RS_NODE_SUSPECT {
            if node.suspect_since_ms.is_none() {
                node.suspect_since_ms = Some(now_ms);
            }
        } else if state == crate::genl::RS_NODE_UP {
            node.suspect_since_ms = None;
        } else if state == crate::genl::RS_NODE_DEAD {
            condemn(node);
        }
    }

    pub fn seed_kernel(&mut self, nodes: &[NodeInfo], now_ms: u64) {
        for info in nodes {
            self.nodes.entry(info.id).or_default();
            self.hints.insert(info.id, (info.state as u8, now_ms));
            self.apply_hint(info.id);
        }
    }

    pub fn evaluate(&mut self, now_ms: u64) {
        let members: Vec<u16> = self
            .nodes
            .iter()
            .filter(|(_, node)| {
                !node.listen.is_empty()
                    || node.kernel_state == crate::genl::RS_NODE_SUSPECT as u8
                    || node.condemned
            })
            .map(|(id, _)| *id)
            .collect();
        let suspects = members
            .iter()
            .filter(|id| {
                let node = &self.nodes[id];
                node.kernel_state == crate::genl::RS_NODE_SUSPECT as u8 && !node.condemned
            })
            .count();
        let partition = members.len() >= 2 && suspects * 2 > members.len();
        let limit = self.dead_after_ms;
        for id in members {
            let Some(node) = self.nodes.get_mut(&id) else {
                continue;
            };
            if node.boot_changed && !node.condemned {
                condemn(node);
                continue;
            }
            if node.condemned || node.kernel_state != crate::genl::RS_NODE_SUSPECT as u8 {
                continue;
            }
            let Some(since) = node.suspect_since_ms else {
                continue;
            };
            if now_ms.saturating_sub(since) < limit {
                continue;
            }
            let grpc_stale = node
                .last_grpc_ms
                .is_none_or(|seen| now_ms.saturating_sub(seen) >= limit);
            if grpc_stale && !partition {
                condemn(node);
            }
        }
    }

    fn apply_hint(&mut self, node_id: u16) {
        if !self.nodes.contains_key(&node_id) {
            return;
        }
        let Some((state, since)) = self.hints.remove(&node_id) else {
            return;
        };
        let Some(node) = self.nodes.get_mut(&node_id) else {
            return;
        };
        if node.condemned {
            return;
        }
        node.kernel_state = state;
        if state == crate::genl::RS_NODE_SUSPECT as u8 && node.suspect_since_ms.is_none() {
            node.suspect_since_ms = Some(since);
        }
        if state == crate::genl::RS_NODE_DEAD as u8 {
            condemn(node);
        }
    }

    pub fn pending(&self) -> Vec<KernelOp> {
        let mut ops = Vec::new();
        for (node_id, node) in &self.nodes {
            if node.condemned {
                if !node.dead_sent {
                    ops.push(KernelOp::NodeSetState {
                        node_id: *node_id,
                        state: crate::genl::RS_NODE_DEAD,
                    });
                }
                for state in node.chunks.values() {
                    if !node.retired.contains(&state.desired.gid) {
                        ops.push(KernelOp::ChunkRetire {
                            node_id: *node_id,
                            gid: state.desired.gid,
                        });
                    }
                }
                continue;
            }
            if node.listen.is_empty() {
                continue;
            }
            let hb_dirty =
                node.has_hb && node.kernel_hb != Some((node.hb_raddr, node.hb_rkey, node.boot_id));
            if node.kernel_listen.as_deref() != Some(node.listen.as_str()) || hb_dirty {
                ops.push(KernelOp::NodeAdd {
                    node_id: *node_id,
                    addr: node.listen.clone(),
                });
            }
            for state in node.chunks.values() {
                match &state.kernel {
                    Some(installed) if installed == &state.desired => {}
                    Some(_) => ops.push(KernelOp::ChunkUpdate {
                        node_id: *node_id,
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
                    if node.has_hb {
                        node.kernel_hb = Some((node.hb_raddr, node.hb_rkey, node.boot_id));
                    }
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
                node_id,
                gid,
                epoch,
                raddr,
                rkey,
            } => {
                if let Some(state) = self
                    .nodes
                    .get_mut(node_id)
                    .and_then(|node| node.chunks.get_mut(gid))
                {
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
            KernelOp::NodeSetState { node_id, state } => {
                if let Some(node) = self.nodes.get_mut(node_id) {
                    if *state == crate::genl::RS_NODE_DEAD {
                        node.dead_sent = true;
                        node.kernel_state = crate::genl::RS_NODE_DEAD as u8;
                    }
                }
            }
            KernelOp::ChunkRetire { node_id, gid } => {
                if let Some(node) = self.nodes.get_mut(node_id) {
                    node.retired.insert(*gid);
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
                let hb_synced = !node.has_hb
                    || node.kernel_hb == Some((node.hb_raddr, node.hb_rkey, node.boot_id));
                let kernel_synced = if node.condemned {
                    node.dead_sent && node.chunks.keys().all(|gid| node.retired.contains(gid))
                } else {
                    node.kernel_listen.as_deref() == Some(node.listen.as_str())
                        && !node.listen.is_empty()
                        && hb_synced
                        && node
                            .chunks
                            .values()
                            .all(|state| state.kernel.as_ref() == Some(&state.desired))
                };
                NodeView {
                    node_id: u32::from(*id),
                    node_epoch: node.epoch,
                    listen_addr: node.listen.clone(),
                    chunks,
                    kernel_synced,
                    node_state: if node.condemned {
                        crate::genl::RS_NODE_DEAD
                    } else {
                        u32::from(node.kernel_state)
                    },
                    boot_id_lo: node.boot_id,
                }
            })
            .collect()
    }
}

fn condemn(node: &mut NodeState) {
    if node.condemned {
        return;
    }
    node.condemned = true;
    node.void_epoch = Some(node.epoch);
    node.void_boot = node.boot_id;
    node.kernel_state = crate::genl::RS_NODE_DEAD as u8;
}

fn note_hb(node: &mut NodeState, hb: NodeHb) {
    if hb.raddr == 0 {
        return;
    }
    if node.boot_id != 0 && hb.boot_id != 0 && node.boot_id != hb.boot_id {
        node.boot_changed = true;
        condemn(node);
    }
    node.has_hb = true;
    node.hb_raddr = hb.raddr;
    node.hb_rkey = hb.rkey;
    if node.boot_id == 0 {
        node.boot_id = hb.boot_id;
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

    pub fn set_dead_after_s(&mut self, secs: u64) {
        self.registry.set_dead_after_s(secs);
    }

    pub fn join(&mut self, node_id: u16, epoch: u32, listen: &str, hb: Option<NodeHb>) {
        self.registry
            .observe(node_id, epoch, listen, &[], hb, now_ms());
    }

    pub fn report(
        &mut self,
        node_id: u16,
        epoch: u32,
        listen: &str,
        chunks: &[ChunkReport],
        hb: Option<NodeHb>,
    ) -> io::Result<()> {
        self.registry
            .observe(node_id, epoch, listen, chunks, hb, now_ms());
        self.sync_kernel()
    }

    fn sync_kernel(&mut self) -> io::Result<()> {
        let ops = self.registry.pending();
        for op in ops {
            match &op {
                KernelOp::NodeAdd { node_id, addr } => {
                    let hb = self.registry.heartbeat_of(*node_id);
                    self.genl.node_add(*node_id, addr, hb.as_ref())?;
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
                        self.genl
                            .chunk_update(*node_id, *gid, *epoch, *raddr, *rkey)?;
                    }
                }
                KernelOp::ChunkUpdate {
                    node_id,
                    gid,
                    epoch,
                    raddr,
                    rkey,
                } => self
                    .genl
                    .chunk_update(*node_id, *gid, *epoch, *raddr, *rkey)?,
                KernelOp::NodeSetState { node_id, state } => {
                    self.genl.node_set_state(*node_id, *state)?;
                }
                KernelOp::ChunkRetire { node_id, gid } => {
                    self.genl.chunk_retire(*node_id, *gid)?;
                }
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
            .join(
                node_id,
                req.node_epoch,
                &req.listen_addr,
                hb_from(req.hb_raddr, req.hb_rkey, req.boot_id_lo),
            );
        info!("node {node_id} joined from {}", req.listen_addr);
        Ok(Response::new(JoinResponse {}))
    }

    async fn report_chunks(
        &self,
        request: Request<ReportChunksRequest>,
    ) -> Result<Response<ReportChunksResponse>, Status> {
        let req = request.into_inner();
        self.push(
            req.node_id,
            req.node_epoch,
            req.listen_addr,
            req.chunks,
            None,
        )?;
        Ok(Response::new(ReportChunksResponse {}))
    }

    async fn heartbeat(
        &self,
        request: Request<HeartbeatRequest>,
    ) -> Result<Response<HeartbeatResponse>, Status> {
        let req = request.into_inner();
        self.push(
            req.node_id,
            req.node_epoch,
            req.listen_addr,
            req.chunks,
            hb_from(req.hb_raddr, req.hb_rkey, req.boot_id_lo),
        )?;
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
        hb: Option<NodeHb>,
    ) -> Result<(), Status> {
        let node_id = u16::try_from(node_id).map_err(|_| Status::invalid_argument("node_id"))?;
        let chunks = chunks_from(&chunks);
        self.controller
            .lock()
            .unwrap_or_else(|err| err.into_inner())
            .report(node_id, epoch, &listen, &chunks, hb)
            .map_err(|err| Status::unavailable(err.to_string()))
    }
}

fn hb_from(raddr: u64, rkey: u32, boot_id: u64) -> Option<NodeHb> {
    if raddr == 0 {
        None
    } else {
        Some(NodeHb {
            raddr,
            rkey,
            boot_id,
        })
    }
}

pub fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|dur| dur.as_millis() as u64)
        .unwrap_or(0)
}

pub fn run(cfg: &ControllerConfig, stop: Arc<AtomicBool>) -> io::Result<()> {
    let controller = Arc::new(Mutex::new(Controller::open()?));
    controller
        .lock()
        .unwrap_or_else(|err| err.into_inner())
        .set_dead_after_s(cfg.dead_after_s);
    info!("controller listening on {}", cfg.listen_addr);
    let halt = Arc::new(AtomicBool::new(false));
    let events = {
        let controller = Arc::clone(&controller);
        let stop = Arc::clone(&stop);
        let halt = Arc::clone(&halt);
        thread::spawn(move || event_loop(controller, stop, halt))
    };
    let eval = {
        let controller = Arc::clone(&controller);
        let stop = Arc::clone(&stop);
        let halt = Arc::clone(&halt);
        thread::spawn(move || eval_loop(controller, stop, halt))
    };
    let rt = tokio::runtime::Builder::new_current_thread()
        .enable_io()
        .enable_time()
        .build()?;
    let service = Arc::clone(&controller);
    let result = rt.block_on(async {
        tonic::transport::Server::builder()
            .add_service(ClusterServiceServer::new(Svc {
                controller: service,
            }))
            .serve_with_shutdown(cfg.listen_addr, wait_stop(stop))
            .await
    });
    halt.store(true, Ordering::Relaxed);
    let _ = events.join();
    let _ = eval.join();
    result.map_err(io::Error::other)
}

fn event_loop(controller: Arc<Mutex<Controller>>, stop: Arc<AtomicBool>, halt: Arc<AtomicBool>) {
    while !stopped(&stop, &halt) {
        let mut events = match GenlClient::open().and_then(|mut client| {
            client.join_events()?;
            Ok(client)
        }) {
            Ok(client) => client,
            Err(_) => {
                thread::sleep(Duration::from_millis(200));
                continue;
            }
        };
        if let Ok(mut cmd) = GenlClient::open() {
            if let Ok(stats) = cmd.stats() {
                controller
                    .lock()
                    .unwrap_or_else(|err| err.into_inner())
                    .registry
                    .seed_kernel(&stats.nodes, now_ms());
            }
        }
        while !stopped(&stop, &halt) {
            match events.wait_event(200) {
                Ok(Some(KernelEvent::NodeState {
                    node_id,
                    state,
                    boot_id,
                })) => {
                    let mut guard = controller.lock().unwrap_or_else(|err| err.into_inner());
                    guard
                        .registry
                        .note_kernel(node_id, state, boot_id, now_ms());
                    if let Err(err) = guard.sync_kernel() {
                        if err.raw_os_error() != Some(2) {
                            warn!("kernel sync after node event failed: {err}");
                        }
                    }
                }
                Ok(Some(KernelEvent::DataLost { node_id, lost })) => {
                    warn!("node {node_id} lost {lost} pages");
                }
                Ok(None) => {}
                Err(err) => {
                    warn!("node event socket failed: {err}");
                    break;
                }
            }
        }
    }
}

fn eval_loop(controller: Arc<Mutex<Controller>>, stop: Arc<AtomicBool>, halt: Arc<AtomicBool>) {
    while !stopped(&stop, &halt) {
        thread::sleep(Duration::from_millis(200));
        let mut guard = controller.lock().unwrap_or_else(|err| err.into_inner());
        guard.registry.evaluate(now_ms());
        if let Err(err) = guard.sync_kernel() {
            if err.raw_os_error() != Some(2) {
                warn!("kernel sync after dead check failed: {err}");
            }
        }
    }
}

fn stopped(stop: &AtomicBool, halt: &AtomicBool) -> bool {
    stop.load(Ordering::Relaxed) || halt.load(Ordering::Relaxed)
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
                node_id: 2,
                gid: 0,
                epoch: 3,
                raddr: 0x1000,
                rkey: 10,
            }]
        );
    }

    #[test]
    fn chunk_update_does_not_touch_another_nodes_same_gid() {
        let mut registry = Registry::default();
        registry.upsert(2, 3, "10.0.0.2:18515", &[chunk(0, 9)]);
        registry.upsert(5, 3, "10.0.0.5:18515", &[chunk(0, 4)]);
        commit_all(&mut registry);
        registry.upsert(2, 3, "10.0.0.2:18515", &[chunk(0, 11)]);
        assert_eq!(
            registry.pending(),
            vec![KernelOp::ChunkUpdate {
                node_id: 2,
                gid: 0,
                epoch: 3,
                raddr: 0x1000,
                rkey: 11,
            }]
        );
        commit_all(&mut registry);
        let views = registry.views();
        assert_eq!(views[0].chunks[0].rkey, 11);
        assert_eq!(views[1].chunks[0].rkey, 4);
        assert!(views[0].kernel_synced && views[1].kernel_synced);
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
                node_id: 2,
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
            hb_raddr: 0,
            hb_rkey: 0,
            boot_id_lo: 0,
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
                hb_raddr: 0,
                hb_rkey: 0,
                boot_id_lo: 0,
            }))
            .await
            .unwrap_err();
        assert_eq!(rejected.code(), tonic::Code::InvalidArgument);
    }

    fn live(registry: &mut Registry, id: u16, epoch: u32, boot: u64) {
        registry.observe(
            id,
            epoch,
            &format!("10.0.0.{id}:18515"),
            &[chunk(0, 1)],
            Some(NodeHb {
                raddr: 0x1000,
                rkey: 1,
                boot_id: boot,
            }),
            0,
        );
        commit_all(registry);
    }

    fn suspect(registry: &mut Registry, id: u16, at: u64) {
        registry.note_kernel(id, crate::genl::RS_NODE_SUSPECT, 0, at);
    }

    #[test]
    fn dead_timer_fires_at_the_limit_and_not_one_millisecond_earlier() {
        let mut registry = Registry::default();
        live(&mut registry, 1, 4, 8);
        suspect(&mut registry, 1, 1_000);
        registry.evaluate(1_000 + 10_000 - 1);
        assert!(!registry.condemned(1));
        registry.evaluate(1_000 + 10_000);
        assert!(registry.condemned(1));
        assert!(registry.pending().contains(&KernelOp::NodeSetState {
            node_id: 1,
            state: crate::genl::RS_NODE_DEAD,
        }));
        assert!(registry
            .pending()
            .contains(&KernelOp::ChunkRetire { node_id: 1, gid: 0 }));
    }

    #[test]
    fn grpc_or_kernel_recovery_prevents_dead() {
        let mut recovered = Registry::default();
        live(&mut recovered, 1, 4, 8);
        suspect(&mut recovered, 1, 1_000);
        assert!(recovered.observe(
            1,
            4,
            "10.0.0.1:18515",
            &[chunk(0, 1)],
            Some(NodeHb {
                raddr: 0x1000,
                rkey: 1,
                boot_id: 8,
            }),
            11_000,
        ));
        recovered.evaluate(11_000);
        assert!(!recovered.condemned(1));

        let mut kernel = Registry::default();
        live(&mut kernel, 1, 4, 8);
        suspect(&mut kernel, 1, 1_000);
        kernel.note_kernel(1, crate::genl::RS_NODE_UP, 0, 5_000);
        kernel.evaluate(30_000);
        assert!(!kernel.condemned(1));
    }

    #[test]
    fn only_a_strict_majority_blocks_dead() {
        let mut both = Registry::default();
        live(&mut both, 1, 1, 1);
        live(&mut both, 2, 1, 1);
        suspect(&mut both, 1, 0);
        suspect(&mut both, 2, 0);
        both.evaluate(10_000);
        assert!(!both.condemned(1) && !both.condemned(2));

        let mut two_of_three = Registry::default();
        live(&mut two_of_three, 1, 1, 1);
        live(&mut two_of_three, 2, 1, 1);
        live(&mut two_of_three, 3, 1, 1);
        suspect(&mut two_of_three, 1, 0);
        suspect(&mut two_of_three, 2, 0);
        two_of_three.evaluate(10_000);
        assert!(!two_of_three.condemned(1));

        let mut half = Registry::default();
        live(&mut half, 1, 1, 1);
        live(&mut half, 2, 1, 1);
        suspect(&mut half, 1, 0);
        half.evaluate(10_000);
        assert!(half.condemned(1));
        assert!(!half.condemned(2));

        let mut alone = Registry::default();
        live(&mut alone, 1, 1, 1);
        suspect(&mut alone, 1, 0);
        alone.evaluate(10_000);
        assert!(alone.condemned(1));
    }

    #[test]
    fn boot_change_condemns_and_a_new_epoch_rejoins() {
        let mut registry = Registry::default();
        live(&mut registry, 1, 4, 8);
        live(&mut registry, 2, 4, 8);
        suspect(&mut registry, 1, 0);
        suspect(&mut registry, 2, 0);
        registry.note_kernel(1, crate::genl::RS_NODE_SUSPECT, 99, 0);
        assert!(registry.condemned(1));
        assert!(!registry.condemned(2));
        assert!(!registry.observe(
            1,
            4,
            "10.0.0.1:18515",
            &[chunk(0, 1)],
            Some(NodeHb {
                raddr: 0x1000,
                rkey: 2,
                boot_id: 8,
            }),
            50_000,
        ));
        assert!(registry
            .pending()
            .iter()
            .all(|op| !matches!(op, KernelOp::NodeAdd { node_id: 1, .. })));
        assert!(registry.observe(
            1,
            5,
            "10.0.0.1:18515",
            &[chunk(0, 1)],
            Some(NodeHb {
                raddr: 0x2000,
                rkey: 3,
                boot_id: 99,
            }),
            50_000,
        ));
        assert!(!registry.condemned(1));
        assert!(registry.pending().contains(&KernelOp::NodeAdd {
            node_id: 1,
            addr: "10.0.0.1:18515".to_string(),
        }));
    }

    #[test]
    fn heartbeat_rkey_change_repeats_node_add() {
        let mut registry = Registry::default();
        live(&mut registry, 1, 4, 8);
        assert!(registry.pending().is_empty());
        registry.observe(
            1,
            4,
            "10.0.0.1:18515",
            &[chunk(0, 1)],
            Some(NodeHb {
                raddr: 0x1000,
                rkey: 6,
                boot_id: 8,
            }),
            10,
        );
        assert_eq!(
            registry.pending(),
            vec![KernelOp::NodeAdd {
                node_id: 1,
                addr: "10.0.0.1:18515".to_string(),
            }]
        );
    }

    #[test]
    fn seeded_suspect_is_not_immediately_dead() {
        let mut registry = Registry::default();
        registry.seed_kernel(
            &[NodeInfo {
                id: 1,
                state: crate::genl::RS_NODE_SUSPECT,
            }],
            5_000,
        );
        live(&mut registry, 1, 4, 8);
        registry.evaluate(5_000);
        assert!(!registry.condemned(1));
        registry.evaluate(15_000);
        assert!(registry.condemned(1));

        let mut seeded = Registry::default();
        seeded.seed_kernel(
            &[NodeInfo {
                id: 2,
                state: crate::genl::RS_NODE_SUSPECT,
            }],
            0,
        );
        seeded.evaluate(0);
        assert!(!seeded.condemned(2));
        seeded.evaluate(10_000);
        assert!(seeded.condemned(2));
        assert!(seeded.pending().contains(&KernelOp::NodeSetState {
            node_id: 2,
            state: crate::genl::RS_NODE_DEAD,
        }));
        assert!(seeded
            .pending()
            .iter()
            .all(|op| !matches!(op, KernelOp::NodeAdd { .. })));
    }
}
