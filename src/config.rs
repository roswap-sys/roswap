// SPDX-License-Identifier: GPL-2.0
use crate::provider::ProviderConfig;
use serde::Deserialize;
use std::fmt;
use std::fs;
use std::io;
use std::net::SocketAddr;
use std::path::Path;

#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum Role {
    Provider,
    Controller,
}

impl fmt::Display for Role {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Role::Provider => "provider",
            Role::Controller => "controller",
        })
    }
}

#[derive(Clone, Debug, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Config {
    pub role: Role,
    #[serde(default)]
    pub provider: ProviderConfig,
    #[serde(default)]
    pub controller: ControllerConfig,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Eq)]
#[serde(default, deny_unknown_fields)]
pub struct ControllerConfig {
    pub listen_addr: SocketAddr,
}

impl Default for ControllerConfig {
    fn default() -> Self {
        Self {
            listen_addr: SocketAddr::from(([0, 0, 0, 0], 12221)),
        }
    }
}

impl Config {
    pub fn load(path: impl AsRef<Path>) -> io::Result<Self> {
        let path = path.as_ref();
        let text = fs::read_to_string(path)?;
        toml::from_str(&text).map_err(|e| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                format!("invalid config {}: {e}", path.display()),
            )
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::{SocketAddr, SocketAddrV4};
    use std::path::PathBuf;

    #[test]
    fn full_provider_config_parses() {
        let cfg: Config = toml::from_str(
            r#"
            role = "provider"

            [provider]
            device_name = ""
            listen_addr = "0.0.0.0:18515"
            grpc_addr = "0.0.0.0:12220"
            mem_dir = "/dev/shm/roswap"
            chunk_size_mb = 256
            num_chunks = 2
            node_epoch = 3
            "#,
        )
        .unwrap();
        assert_eq!(cfg.role, Role::Provider);
        assert_eq!(cfg.provider.device_name, None);
        assert_eq!(
            cfg.provider.listen_addr,
            "0.0.0.0:18515".parse::<SocketAddrV4>().unwrap()
        );
        assert_eq!(
            cfg.provider.grpc_addr,
            "0.0.0.0:12220".parse::<SocketAddr>().unwrap()
        );
        assert_eq!(cfg.provider.mem_dir, PathBuf::from("/dev/shm/roswap"));
        assert_eq!(cfg.provider.chunk_size_mb, 256);
        assert_eq!(cfg.provider.num_chunks, 2);
        assert_eq!(cfg.provider.node_epoch, 3);
    }

    #[test]
    fn missing_provider_fields_use_defaults() {
        let cfg: Config = toml::from_str("role = \"provider\"\n").unwrap();
        assert_eq!(cfg.provider, ProviderConfig::default());
    }

    #[test]
    fn empty_device_name_selects_automatically() {
        let cfg: Config = toml::from_str(
            r#"
            role = "provider"
            [provider]
            device_name = ""
            "#,
        )
        .unwrap();
        assert_eq!(cfg.provider.device_name, None);
    }

    #[test]
    fn unknown_field_is_rejected() {
        let err = toml::from_str::<Config>("role = \"provider\"\nextra = 1\n").unwrap_err();
        assert!(err.to_string().contains("extra"));
    }

    #[test]
    fn provider_registration_fields_default_and_parse() {
        let cfg: Config = toml::from_str("role = \"provider\"\n").unwrap();
        assert_eq!(cfg.provider.node_id, 1);
        assert_eq!(cfg.provider.controller_addr, "");
        assert_eq!(cfg.provider.controller_endpoint().unwrap(), None);
        assert_eq!(cfg.controller, ControllerConfig::default());
        assert_eq!(
            cfg.controller.listen_addr,
            "0.0.0.0:12221".parse::<SocketAddr>().unwrap()
        );

        let cfg: Config = toml::from_str(
            r#"
            role = "provider"
            [provider]
            node_id = 6
            controller_addr = "10.0.0.1:12221"
            [controller]
            listen_addr = "127.0.0.1:9"
            "#,
        )
        .unwrap();
        assert_eq!(cfg.provider.node_id, 6);
        assert_eq!(
            cfg.provider.controller_endpoint().unwrap(),
            Some("10.0.0.1:12221".parse().unwrap())
        );
        assert_eq!(
            cfg.controller.listen_addr,
            "127.0.0.1:9".parse::<SocketAddr>().unwrap()
        );
        let mut bad = cfg.provider.clone();
        bad.controller_addr = "not-an-address".to_string();
        assert!(bad.controller_endpoint().unwrap_err().to_string().contains("controller_addr"));
    }

    #[test]
    fn controller_role_rejects_unknown_listen_fields() {
        let cfg: Config = toml::from_str("role = \"controller\"\n").unwrap();
        assert_eq!(cfg.role, Role::Controller);
        assert_eq!(cfg.provider, ProviderConfig::default());
        let err = toml::from_str::<Config>(
            "role = \"controller\"\n[controller]\nextra = 1\n",
        )
        .unwrap_err();
        assert!(err.to_string().contains("extra"));
    }
}
