// SPDX-License-Identifier: GPL-2.0

tonic::include_proto!("roswap");

#[cfg(test)]
mod tests {
    use super::provider_service_client::ProviderServiceClient;
    use super::provider_service_server::{ProviderService, ProviderServiceServer};
    use super::{GetInfoRequest, GetInfoResponse};
    use tokio::net::TcpListener;
    use tokio_stream::wrappers::TcpListenerStream;
    use tonic::transport::Server;
    use tonic::{Request, Response, Status};

    struct Info;

    #[tonic::async_trait]
    impl ProviderService for Info {
        async fn get_info(
            &self,
            _request: Request<GetInfoRequest>,
        ) -> Result<Response<GetInfoResponse>, Status> {
            Ok(Response::new(GetInfoResponse {
                version: env!("CARGO_PKG_VERSION").to_string(),
            }))
        }
    }

    #[tokio::test]
    async fn get_info_loopback() {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        tokio::spawn(async move {
            Server::builder()
                .add_service(ProviderServiceServer::new(Info))
                .serve_with_incoming(TcpListenerStream::new(listener))
                .await
                .unwrap();
        });

        let mut client = ProviderServiceClient::connect(format!("http://{addr}"))
            .await
            .unwrap();
        let response = client
            .get_info(Request::new(GetInfoRequest {}))
            .await
            .unwrap()
            .into_inner();
        assert_eq!(response.version, env!("CARGO_PKG_VERSION"));
    }
}
