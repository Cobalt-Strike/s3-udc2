use anyhow::{Context, Result, anyhow};
use aws_sdk_s3::config::{Credentials, ProvideCredentials};
use clap::{Args as ClapArgs, Parser, Subcommand};
use serde::Deserialize;
use std::collections::HashMap;
use std::path::PathBuf;
use std::time::Duration;
use tracing::warn;

use aws_config::{BehaviorVersion, Region};
use udc2_relay::Udc2ListenerConfig;

use crate::s3::config::{
    ConsumerBackend, PollingConsumerCfg, ProduceCfg, S3C2Config, SqsConsumerCfg,
};

#[derive(Debug, ClapArgs)]
pub struct CommonArgs {
    /// S3 bucket to use
    #[arg(long, env = "S3_BUCKET")]
    pub bucket: Option<String>,

    /// Buffer size for messages
    #[arg(long, env = "C2_BUFFER_SIZE", default_value_t = 128)]
    pub buffer_size: usize,

    /// Max concurrent consumer tasks
    #[arg(long, env = "S3_CONSUMER_MAX_CONCURRENCY", default_value_t = 8)]
    pub consumer_max_concurrency: usize,

    /// Key prefix for incoming objects
    #[arg(long, env = "S3_INCOMING_PREFIX")]
    pub incoming_prefix: Option<String>,

    /// Key prefix for outgoing objects
    #[arg(long, env = "S3_OUTGOING_PREFIX")]
    pub outgoing_prefix: Option<String>,

    /// Host for UDC2 listener
    #[arg(long, env = "UDC2_LISTENER_HOST", default_value = "127.0.0.1")]
    pub udc2_listener_host: String,

    /// Port for UDC2 listener
    #[arg(long, env = "UDC2_LISTENER_PORT", default_value_t = 3333)]
    pub udc2_listener_port: u16,
}

#[derive(Debug, ClapArgs)]
pub struct PollingArgs {
    /// How often to poll for new messages (seconds)
    #[arg(long, env = "S3_CONSUMER_POLL_EVERY_SECS", default_value_t = 3)]
    pub consumer_poll_every_secs: u64,
}

#[derive(Debug, ClapArgs)]
pub struct SqsArgs {
    /// SQS queue for S3 upload notifications
    #[arg(long, env = "S3_NOTIFICATION_QUEUE")]
    pub notification_queue: Option<String>,
}

#[derive(Debug, Subcommand)]
pub enum Mode {
    /// Use plain polling mode
    Poll(PollingArgs),

    /// Listen SQS notifications from S3
    Sqs(SqsArgs),
}

#[derive(Debug, Parser)]
#[command(name = "s3-udc2", about = "User-Defined C2 over AWS S3", version)]
pub struct Args {
    /// Terraform output JSON file
    #[arg(long, value_name = "FILE")]
    pub config: Option<PathBuf>,

    #[command(flatten)]
    pub common: CommonArgs,

    #[command(subcommand)]
    pub mode: Mode,
}

#[derive(Debug, Deserialize)]
struct TerraformEntry {
    #[serde(default)]
    value: String,
}

impl Args {
    /// Convert parsed CLI arguments into a full S3 UDC2 runtime configuration.
    pub async fn into_config(self) -> Result<(S3C2Config, Udc2ListenerConfig)> {
        let file_map: HashMap<String, TerraformEntry> = if let Some(path) = &self.config {
            let s = tokio::fs::read_to_string(path)
                .await
                .with_context(|| format!("failed to read config file {:?}", path))?;
            serde_json::from_str(&s).context("failed to parse config file as JSON")?
        } else {
            HashMap::new()
        };

        let cfg_val = |key: &str| -> Option<String> { file_map.get(key).map(|x| x.value.clone()) };

        let ensure_slash = |s: String| {
            if s.ends_with('/') {
                s
            } else {
                format!("{}/", s)
            }
        };

        // Merge precedence: CLI/env -> Terraform output -> defaults
        let merge = |arg: Option<String>, key, default: Option<&'static str>, err: &'static str| {
            arg.clone()
                .or_else(|| cfg_val(key))
                .or_else(|| default.map(|x| x.to_string()))
                .ok_or_else(|| anyhow!(err))
        };

        let bucket = merge(
            self.common.bucket,
            "bucket_name",
            None,
            "bucket must be provided via --bucket, S3_BUCKET env, or --config JSON file",
        )?;

        let incoming_prefix = merge(
            self.common.incoming_prefix,
            "client_to_server_prefix",
            Some("c2s/"),
            "",
        )
        .map(ensure_slash)?;

        let outgoing_prefix = merge(
            self.common.outgoing_prefix,
            "server_to_client_prefix",
            Some("s2c"),
            "",
        )
        .map(ensure_slash)?;

        let config = match (
            cfg_val("relay_user_access_key_id"),
            cfg_val("relay_user_secret_access_key"),
            cfg_val("aws_region"),
        ) {
            (Some(access_key_id), Some(secret_access_key), Some(region)) => {
                aws_config::defaults(BehaviorVersion::latest())
                    .credentials_provider(Credentials::new(
                        access_key_id,
                        secret_access_key,
                        None,
                        None,
                        "Static",
                    ))
                    .region(Region::new(region))
                    .load()
                    .await
            }
            _ => {
                let config = aws_config::defaults(BehaviorVersion::latest()).load().await;
                let provider = config
                    .credentials_provider()
                    .ok_or_else(|| anyhow!("could not get AWS Access Key ID"))?
                    .provide_credentials()
                    .await?;
                let access_key_id = provider.access_key_id();
                warn!("using the default AWS credentials: {access_key_id}");
                config
            }
        };

        let s3_client = aws_sdk_s3::Client::new(&config);

        let consumer_cfg = match self.mode {
            Mode::Poll(p) => ConsumerBackend::Polling(PollingConsumerCfg {
                s3_client: s3_client.clone(),
                poll_every: Duration::from_secs(p.consumer_poll_every_secs),
                max_concurrency: self.common.consumer_max_concurrency,
                prefix: incoming_prefix.clone(),
            }),
            Mode::Sqs(s) => {
                let queue_url = merge(
                    s.notification_queue,
                    "s3_events_queue",
                    None,
                    "SQS queue must be provided via --notification-queue, S3_NOTIFICATION_QUEUE env, or --config JSON file (s3_events_queue)",
                )?;

                ConsumerBackend::Sqs(SqsConsumerCfg {
                    s3_client: s3_client.clone(),
                    sqs_client: aws_sdk_sqs::Client::new(&config),
                    queue_url,
                    max_concurrency: self.common.consumer_max_concurrency,
                    prefix: incoming_prefix.clone(),
                })
            }
        };

        let s3_cfg = S3C2Config {
            bucket,
            buffer_size: self.common.buffer_size,
            consumer_cfg,
            producer_cfg: ProduceCfg {
                s3_client,
                prefix: outgoing_prefix,
            },
        };

        let udc2_cfg = Udc2ListenerConfig {
            host: self.common.udc2_listener_host,
            port: self.common.udc2_listener_port,
        };

        Ok((s3_cfg, udc2_cfg))
    }

    pub async fn from_cli() -> Result<(S3C2Config, Udc2ListenerConfig)> {
        let args = Args::parse();
        args.into_config().await
    }
}
