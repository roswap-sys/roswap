// SPDX-License-Identifier: GPL-2.0
use std::net::SocketAddr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;
use crate::proto::provider_service_server::{ProviderService, ProviderServiceServer};
use crate::proto::{GetInfoRequest, GetInfoResponse};
use tonic::{Request, Response, Status};

pub struct Info;

#[tonic::async_trait]
impl ProviderService for Info {
    async fn get_info(
        &self,
        _request: Request<GetInfoRequest>,
    ) -> Result<Response<GetInfoResponse>, Status> {
        Ok(Response::new(GetInfoResponse {
            version: "info version: 1.0.0".to_string(),
        }))
    }
}

pub async fn serve_provider(
    addr: SocketAddr,
    stop: Arc<AtomicBool>,
) -> Result<(), tonic::transport::Error> {
    tonic::transport::Server::builder()
        .add_service(ProviderServiceServer::new(Info))
        .serve_with_shutdown(addr, wait_stop(stop))
        .await
}

async fn wait_stop(stop: Arc<AtomicBool>) {
    while !stop.load(Ordering::Relaxed) {
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
}

#[cfg(test)]
mod tests {
    use super::{serve_provider, Info};
    use crate::proto::provider_service_client::ProviderServiceClient;
    use crate::proto::provider_service_server::ProviderServiceServer;
    use crate::proto::GetInfoRequest;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;
    use std::time::Duration;
    use tokio::net::TcpListener;
    use tokio_stream::wrappers::TcpListenerStream;
    use tonic::transport::Server;
    use tonic::Request;

    #[tokio::test]
    async fn get_info_uses_the_real_service_and_stops() {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        let stop = Arc::new(AtomicBool::new(false));
        let stop_srv = Arc::clone(&stop);
        let server = tokio::spawn(async move {
            Server::builder()
                .add_service(ProviderServiceServer::new(Info))
                .serve_with_incoming_shutdown(
                    TcpListenerStream::new(listener),
                    super::wait_stop(stop_srv),
                )
                .await
        });

        let mut client = ProviderServiceClient::connect(format!("http://{addr}"))
            .await
            .unwrap();
        let response = client
            .get_info(Request::new(GetInfoRequest {}))
            .await
            .unwrap()
            .into_inner();
        assert_eq!(response.version, "info version: 1.0.0");

        stop.store(true, Ordering::Relaxed);
        tokio::time::timeout(Duration::from_secs(2), server)
            .await
            .expect("server did not stop")
            .unwrap()
            .unwrap();
    }

    #[tokio::test]
    async fn serve_provider_returns_when_stop_is_set() {
        let addr = "127.0.0.1:0".parse().unwrap();
        let stop = Arc::new(AtomicBool::new(false));
        let stop_srv = Arc::clone(&stop);
        let server = tokio::spawn(async move { serve_provider(addr, stop_srv).await });
        tokio::time::sleep(Duration::from_millis(50)).await;
        stop.store(true, Ordering::Relaxed);
        tokio::time::timeout(Duration::from_secs(2), server)
            .await
            .expect("serve_provider did not return")
            .unwrap()
            .unwrap();
    }
}
