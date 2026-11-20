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
            let mut json = false;
            let mut addr = None;
            for arg in args {
                if arg == "--json" {
                    json = true;
                } else if addr.is_none() {
                    addr = Some(arg);
                } else {
                    eprintln!(
                        "Usage: roswapctl status | roswapctl cluster [--json] [http://host:port]"
                    );
                    process::exit(2);
                }
            }
            let addr = addr.unwrap_or_else(|| "http://127.0.0.1:12221".to_string());
            cluster_status(&normalize_http(&addr), json)
        }
        _ => {
            eprintln!("Usage: roswapctl status | roswapctl cluster [--json] [http://host:port]");
            process::exit(2);
        }
    };
    if let Err(err) = result {
        if err.kind() == io::ErrorKind::BrokenPipe {
            process::exit(0);
        }
        eprintln!("roswapctl: {err}");
        process::exit(1);
    }
}

fn kernel_status() -> io::Result<()> {
    let mut genl = GenlClient::open()?;
    let stats = genl.stats()?;
    let mut out = io::stdout().lock();
    writeln!(
        out,
        "slots {} used {} free_extents {} nodes {} chunks {} links_up {} degraded {} lost {} hedge_sent {} hedge_won {} key_wait {}",
        stats.nr_slots,
        stats.used_slots,
        stats.free_extents,
        stats.nr_nodes,
        stats.nr_chunks,
        stats.links_up,
        stats.degraded_slots,
        stats.lost_slots,
        stats.hedge_sent,
        stats.hedge_won,
        stats.key_wait_hits
    )?;
    for node in &stats.nodes {
        let name = match node.state {
            1 => "suspect",
            2 => "dead",
            _ => "up",
        };
        writeln!(out, "node {} {}", node.id, name)?;
    }
    Ok(())
}

fn json_escape(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for ch in text.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out
}

fn cluster_status(addr: &str, json: bool) -> io::Result<()> {
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
        if json {
            write!(out, "{{\"nodes\":[")?;
            for (i, node) in response.nodes.iter().enumerate() {
                if i > 0 {
                    write!(out, ",")?;
                }
                write!(
                    out,
                    "{{\"node_id\":{},\"epoch\":{},\"listen\":\"{}\",\"synced\":{},\"state\":{},\"boot\":{},\"chunks\":[",
                    node.node_id,
                    node.node_epoch,
                    json_escape(&node.listen_addr),
                    if node.kernel_synced { "true" } else { "false" },
                    node.node_state,
                    node.boot_id_lo
                )?;
                for (j, chunk) in node.chunks.iter().enumerate() {
                    if j > 0 {
                        write!(out, ",")?;
                    }
                    write!(
                        out,
                        "{{\"gid\":{},\"epoch\":{},\"raddr\":{},\"rkey\":{},\"size_mb\":{}}}",
                        chunk.gid, chunk.epoch, chunk.raddr, chunk.rkey, chunk.size_mb
                    )?;
                }
                write!(out, "]}}")?;
            }
            writeln!(out, "]}}")?;
            return Ok::<(), Box<dyn std::error::Error>>(());
        }
        if response.nodes.is_empty() {
            writeln!(out, "no nodes")?;
        }
        for node in response.nodes {
            writeln!(
                out,
                "node {} epoch {} listen {} synced {} state {} boot {:#x}",
                node.node_id,
                node.node_epoch,
                node.listen_addr,
                node.kernel_synced,
                node.node_state,
                node.boot_id_lo
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
    .map_err(|err| io::Error::other(err.to_string()))
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
    use super::{json_escape, normalize_http};

    #[test]
    fn cluster_address_keeps_a_scheme_and_adds_http() {
        assert_eq!(
            normalize_http("http://127.0.0.1:12221"),
            "http://127.0.0.1:12221"
        );
        assert_eq!(
            normalize_http("https://controller:12221"),
            "https://controller:12221"
        );
        assert_eq!(normalize_http("127.0.0.1:12221"), "http://127.0.0.1:12221");
        assert_eq!(normalize_http("[::1]:12221"), "http://[::1]:12221");
    }

    #[test]
    fn json_escape_quotes_and_controls() {
        assert_eq!(json_escape("a\"b\\c"), "a\\\"b\\\\c");
        assert_eq!(json_escape("line\n"), "line\\u000a");
    }
}
