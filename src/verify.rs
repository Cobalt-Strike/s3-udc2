use anyhow::{Context, Result};
use http_body_util::Full;
use hyper::body::{Bytes, Incoming};
use hyper::header::{CONTENT_LENGTH, CONTENT_TYPE};
use hyper::{Method, Request, Response, Uri};
use hyper_rustls::{HttpsConnector, HttpsConnectorBuilder};
use hyper_util::client::legacy::Client;
use hyper_util::client::legacy::connect::HttpConnector;
use hyper_util::rt::TokioExecutor;
use std::collections::HashMap;

fn build_multipart_body(
    fields: &HashMap<String, String>,
    file_field_name: &str,
    file_name: &str,
    file_bytes: &[u8],
    boundary: &str,
) -> Vec<u8> {
    let mut body = Vec::new();

    // Add text fields first
    for (k, v) in fields {
        body.extend_from_slice(format!("--{}\r\n", boundary).as_bytes());
        body.extend_from_slice(
            format!("Content-Disposition: form-data; name=\"{}\"\r\n\r\n", k).as_bytes(),
        );
        body.extend_from_slice(v.as_bytes());
        body.extend_from_slice(b"\r\n");
    }

    // Add the file part
    body.extend_from_slice(format!("--{}\r\n", boundary).as_bytes());
    body.extend_from_slice(
        format!(
            "Content-Disposition: form-data; name=\"{}\"; filename=\"{}\"\r\n\
             Content-Type: application/octet-stream\r\n\r\n",
            file_field_name, file_name
        )
        .as_bytes(),
    );
    body.extend_from_slice(file_bytes);
    body.extend_from_slice(b"\r\n");

    // Closing boundary
    body.extend_from_slice(format!("--{}--\r\n", boundary).as_bytes());

    body
}

pub fn build_https_client() -> Result<Client<HttpsConnector<HttpConnector>, Full<Bytes>>> {
    let https = HttpsConnectorBuilder::new()
        .with_native_roots()
        .context("no native root CA certificates found")?
        .https_or_http()
        .enable_http1()
        .build();

    Ok(Client::builder(TokioExecutor::new()).build(https))
}

pub async fn upload_file(
    client: &Client<HttpsConnector<HttpConnector>, Full<Bytes>>,
    url: &Uri,
    fields: &HashMap<String, String>,
    file_name: &str,
    file_bytes: &[u8],
) -> Result<Response<Incoming>> {
    let boundary = format!("----hyper-boundary-{}", uuid::Uuid::new_v4());
    let body_bytes = build_multipart_body(fields, "file", file_name, file_bytes, &boundary);

    let req = Request::builder()
        .method(Method::POST)
        .uri(url.clone())
        .header(
            CONTENT_TYPE,
            format!("multipart/form-data; boundary={}", boundary),
        )
        .header(CONTENT_LENGTH, body_bytes.len())
        .body(Full::new(Bytes::from(body_bytes)))?;

    let resp = client.request(req).await?;
    Ok(resp)
}
