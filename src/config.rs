// SPDX-License-Identifier: GPL-2.0
use crate::provider::ProviderConfig;
use serde::{Deserialize, Serialize};
use std::fmt;
use std::fs;
use std::io;
use std::path::Path;

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq)]
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

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Config {
    pub role: Role,
    #[serde(default)]
    pub provider: ProviderConfig,
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

/* 

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::SocketAddrV4;
    use std::path::PathBuf;

    #[test]
    fn repo_config_parses() {
        let cfg: Config = toml::from_str(include_str!("../config.toml")).unwrap();
        assert_eq!(cfg.role, Role::Provider);
        assert_eq!(cfg.provider.device_name, None);
        assert_eq!(
            cfg.provider.listen_addr,
            "0.0.0.0:18515".parse::<SocketAddrV4>().unwrap()
        );
        assert_eq!(cfg.provider.mem_dir, PathBuf::from("/dev/shm/roswap"));
        assert_eq!(cfg.provider.chunk_size_mb, 64);
        assert_eq!(cfg.provider.num_chunks, 1);
        assert_eq!(cfg.provider.node_epoch, 1);
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
}
*/