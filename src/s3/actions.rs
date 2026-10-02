//! Utilities for interacting with Amazon S3.
use anyhow::Result;
use aws_sdk_s3::{Client, operation::put_object::PutObjectOutput, primitives::ByteStream};
use bytes::Bytes;

/// Retrieves an object from S3 and returns its full contents as `Bytes`.
pub async fn get_object(client: &Client, bucket: &str, key: &str) -> Result<Bytes> {
    let object = client.get_object().bucket(bucket).key(key).send().await?;
    let bytes = object.body.collect().await.map(|data| data.into_bytes())?;
    Ok(bytes)
}

/// Deletes an object from the specified S3 bucket.
pub async fn delete_object(client: &Client, bucket: &str, key: &str) -> Result<()> {
    let _ = client
        .delete_object()
        .bucket(bucket)
        .key(key)
        .send()
        .await?;
    Ok(())
}

/// Uploads the provided `Bytes` to S3 at the given bucket and key.
pub async fn upload_object(
    client: &aws_sdk_s3::Client,
    bucket: &str,
    key: &str,
    data: Bytes,
) -> Result<PutObjectOutput> {
    let body = ByteStream::from(data);
    let object = client
        .put_object()
        .bucket(bucket)
        .key(key)
        .body(body)
        .send()
        .await?;
    Ok(object)
}
