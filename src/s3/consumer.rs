use crate::s3::{
    actions::{delete_object, get_object},
    config::{PollingConsumerCfg, SqsConsumerCfg},
};
use anyhow::{Context, Result, bail};
use bytes::Bytes;
use futures::{StreamExt, TryStreamExt, stream};
use serde::Deserialize;
use serde_json::Value;
use tokio::sync::mpsc::Sender;
use tracing::{debug, error, info, warn};
use udc2_relay::Message;

/// Representation of an S3 event delivered via SQS.
#[derive(Debug, Deserialize)]
struct S3Event {
    #[serde(rename = "Records")]
    pub records: Vec<S3EventRecord>,
}

/// A single record within an S3 event.
#[derive(Debug, Deserialize)]
struct S3EventRecord {
    pub s3: S3Entity,
}

/// The `s3` entity from an S3 event record.
#[derive(Debug, Deserialize)]
struct S3Entity {
    pub bucket: S3Bucket,
    pub object: S3Object,
}

/// Bucket information from an S3 event.
#[derive(Debug, Deserialize)]
struct S3Bucket {
    pub name: String,
}

/// Object information from an S3 event.
#[derive(Debug, Deserialize)]
struct S3Object {
    pub key: String,
}

/// Consume SQS notifications that contain S3 event payloads.
///
/// This function continuously receives messages from the configured SQS
/// queue. For each message that parses as an S3 event it spawns a task to
/// fetch and process the referenced S3 objects. After handling a message it
/// attempts to delete it from the queue.
pub async fn consume_notify(tx: Sender<Message>, cfg: SqsConsumerCfg) -> Result<()> {
    let s3_client = cfg.s3_client;
    let sqs_client = cfg.sqs_client;

    while !tx.is_closed() {
        let resp = sqs_client
            .receive_message()
            .queue_url(&cfg.queue_url)
            .max_number_of_messages(cfg.max_concurrency as i32)
            .wait_time_seconds(20)
            .send()
            .await
            .with_context(|| "could not receive queue messages")?;

        for msg in resp.messages() {
            if let Some(event) = parse_s3_event(msg) {
                for record in event.records {
                    let bucket = record.s3.bucket.name;
                    let key = record.s3.object.key;

                    if key.starts_with(&cfg.prefix) {
                        let s3_client = s3_client.clone();
                        let tx = tx.clone();
                        let prefix_len = cfg.prefix.len();

                        tokio::spawn(async move {
                            if let Err(err) =
                                process_frame_object(&s3_client, &bucket, &key, prefix_len, &tx)
                                    .await
                            {
                                error!(?err, %bucket, %key, "could not process the S3 object");
                            }
                        });
                    } else {
                        warn!(%key, prefix=cfg.prefix, "received object where the key does not start with the correct prefix")
                    }
                }
            }

            if let Some(receipt) = msg.receipt_handle() {
                sqs_client
                    .delete_message()
                    .queue_url(&cfg.queue_url)
                    .receipt_handle(receipt)
                    .send()
                    .await
                    .with_context(|| "could not delete the message")?;
            }
        }
    }

    Ok(())
}

/// Poll an S3 bucket for objects and process them.
///
/// This consumer periodically lists objects (using the provided `cfg.prefix`)
/// and processes each object concurrently (bounded by `cfg.max_concurrency`).
/// The function runs until the provided `tx` sender is closed.
pub async fn consume_poll(
    bucket: String,
    tx: Sender<Message>,
    cfg: PollingConsumerCfg,
) -> Result<()> {
    let mut interval = tokio::time::interval(cfg.poll_every);
    let client = cfg.s3_client;

    loop {
        tokio::select! {
            _ = tx.closed() => break,
            _ = interval.tick() => {}
        }

        let mut page = client
            .list_objects_v2()
            .bucket(&bucket)
            .prefix(&cfg.prefix)
            .into_paginator()
            .send();

        while let Some(result) = page.next().await {
            match result {
                Ok(objs) => {
                    let keys: Vec<String> = objs
                        .contents()
                        .iter()
                        .filter_map(|obj| obj.key().map(str::to_owned))
                        .collect();

                    let tasks = keys.into_iter().map(|key| {
                        let client = client.clone();
                        let tx = tx.clone();
                        let bucket = bucket.clone();
                        let prefix_len = cfg.prefix.len();

                        async move {
                            process_frame_object(&client, &bucket, &key, prefix_len, &tx).await
                        }
                    });

                    let process = stream::iter(tasks)
                        .buffer_unordered(cfg.max_concurrency)
                        .try_for_each(|_| async { Ok(()) });

                    tokio::select! {
                        _ = tx.closed() => break,
                        res = process => {
                            if let Err(err) = res {
                                warn!(?err, bucket, "could not fetch an object from the bucket");
                            }
                        }
                    }
                }
                Err(err) => {
                    return Err(err).with_context(|| "could not list content in the bucket");
                }
            }
        }
    }

    Ok(())
}

/// Fetch an S3 object, delete it from the bucket, convert it to a `Message`
/// and forward it on `tx`.
async fn process_frame_object(
    s3_client: &aws_sdk_s3::Client,
    bucket: &str,
    key: &str,
    prefix_len: usize,
    tx: &Sender<Message>,
) -> anyhow::Result<()> {
    debug!(bucket, key, "fetching the object from S3");
    let bytes = get_object(s3_client, bucket, key).await?;

    debug!(bucket, key, "deleting the object from S3");
    delete_object(s3_client, bucket, key).await?;

    let ctx = key[prefix_len..].to_string();
    if ctx.starts_with("verify_") && bytes.len() == 0 {
        info!(%ctx, "received a test object");
        return Ok(());
    }

    match to_message(bytes, ctx) {
        Ok(msg) => tx.send(msg).await?,
        Err(err) => return Err(err).with_context(|| "could not parse the paylaod"),
    }

    Ok(())
}

/// Convert raw object bytes into a `Message`.
///
/// The expected payload format is a 4-byte little-endian subject id followed
/// by the frame payload. The subject id maps to different `Message` variants:
/// - 0 => Metadata
/// - 1 => Frame
/// - 2 => Exit
fn to_message(mut payload: Bytes, ctx: String) -> Result<Message> {
    if payload.len() < 4 {
        bail!("frame contains less than 4 bytes: {}", payload.len());
    }
    let frame = payload.split_off(4);
    let subject = u32::from_le_bytes(payload[..].try_into()?);

    debug!(%ctx, %subject);
    match subject {
        0 => Ok(Message::Metadata { ctx, frame }),
        1 => Ok(Message::Frame { ctx, frame }),
        2 => Ok(Message::Exit { ctx }),
        id => {
            error!(%ctx, %id, "unkown frame type");
            Ok(Message::Exit { ctx })
        }
    }
}

/// Parse an SQS message body into an `S3Event` if possible.
fn parse_s3_event(msg: &aws_sdk_sqs::types::Message) -> Option<S3Event> {
    let body = msg.body()?;

    let parsed: Value = match serde_json::from_str(body) {
        Ok(v) => v,
        Err(err) => {
            error!(?err, %body, "Failed to parse S3 event");
            return None;
        }
    };

    if is_s3_test_event(&parsed) {
        return None;
    }

    match serde_json::from_value::<S3Event>(parsed) {
        Ok(event) => Some(event),
        Err(err) => {
            error!(?err, %body, "Failed to parse S3 event");
            None
        }
    }
}

/// Detect whether a JSON blob is the S3 test event sent by AWS.
fn is_s3_test_event(blob: &Value) -> bool {
    blob.get("Event")
        .and_then(|v| v.as_str())
        .map(|s| s == "s3:TestEvent")
        .unwrap_or(false)
}
