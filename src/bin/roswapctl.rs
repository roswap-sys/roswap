// SPDX-License-Identifier: GPL-2.0
use roswap::genl::GenlClient;
use roswap::proto::cluster_service_client::ClusterServiceClient;
use roswap::proto::GetClusterRequest;
use std::env;
use std::io::{self, Write};
use std::process;
use tonic::Request;

fn main() {
    let mut args = env::args().skip(1);
    let cmd = args.next();
    let result = match cmd.as_deref() {
        Some("status") => kernel_status(),
        Some("cluster") => {
            let addr = args
                .next()
                .unwrap_or_else(|| "http://127.0.0.1:12221".to_string());
            cluster_status(&normalize_http(&addr))
        }
        _ => {
            eprintln!("Usage: roswapctl status | roswapctl cluster [http://host:port]");
            process::exit(2);
        }
    };
    if let Err(err) = result {
        eprintln!("roswapctl: {err}");
        process::exit(1);
    }
}

fn kernel_status() -> io::Result<()> {
    let mut genl = GenlClient::open()?;
    let stats = genl.stats()?;
    println!(
        "slots {} used {} free_extents {} nodes {} chunks {} links_up {}",
        stats.nr_slots,
        stats.used_slots,
        stats.free_extents,
        stats.nr_nodes,
        stats.nr_chunks,
        stats.links_up
    );
    Ok(())
}

fn cluster_status(addr: &str) -> io::Result<()> {
    let rt = tokio::runtime::Builder::new_current_thread()
        .enable_io()
        .enable_time()
        .build()?;
    rt.block_on(async {
        let mut client = ClusterServiceClient::connect(addr.to_string()).await?;
        let response = client
            .get_cluster(Request::new(GetClusterRequest {}))
            .await?
            .into_inner();
        let mut out = io::stdout().lock();
        if response.nodes.is_empty() {
            writeln!(out, "no nodes")?;
        }
        for node in response.nodes {
            writeln!(
                out,
                "node {} epoch {} listen {} synced {}",
                node.node_id, node.node_epoch, node.listen_addr, node.kernel_synced
            )?;
            for chunk in node.chunks {
                writeln!(
                    out,
                    "  chunk {} epoch {} raddr {:#x} rkey {:#x} {} MiB",
                    chunk.gid, chunk.epoch, chunk.raddr, chunk.rkey, chunk.size_mb
                )?;
            }
        }
        Ok::<(), Box<dyn std::error::Error>>(())
    })
    .map_err(|err| io::Error::new(io::ErrorKind::Other, err.to_string()))
}

fn normalize_http(addr: &str) -> String {
    if addr.starts_with("http://") || addr.starts_with("https://") {
        addr.to_string()
    } else {
        format!("http://{addr}")
    }
}

#[cfg(test)]
mod tests {
    use super::normalize_http;

    #[test]
    fn cluster_address_keeps_a_scheme_and_adds_http() {
        assert_eq!(normalize_http("http://127.0.0.1:12221"), "http://127.0.0.1:12221");
        assert_eq!(normalize_http("https://controller:12221"), "https://controller:12221");
        assert_eq!(normalize_http("127.0.0.1:12221"), "http://127.0.0.1:12221");
        assert_eq!(normalize_http("[::1]:12221"), "http://[::1]:12221");
    }
}
