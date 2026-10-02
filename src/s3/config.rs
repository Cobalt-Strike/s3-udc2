use std::time::Duration;

#[derive(Debug)]
pub struct S3C2Config {
    pub bucket: String,
    pub buffer_size: usize,
    pub consumer_cfg: ConsumerBackend,
    pub producer_cfg: ProduceCfg,
}

#[derive(Debug, Clone)]
pub struct PollingConsumerCfg {
    pub s3_client: aws_sdk_s3::Client,
    pub poll_every: Duration,
    pub max_concurrency: usize,
    pub prefix: String,
}

#[derive(Debug, Clone)]
pub struct SqsConsumerCfg {
    pub s3_client: aws_sdk_s3::Client,
    pub sqs_client: aws_sdk_sqs::Client,
    pub queue_url: String,
    pub max_concurrency: usize,
    pub prefix: String,
}

#[derive(Debug, Clone)]
pub enum ConsumerBackend {
    Polling(PollingConsumerCfg),
    Sqs(SqsConsumerCfg),
}

#[derive(Debug)]
pub struct ProduceCfg {
    pub s3_client: aws_sdk_s3::Client,
    pub prefix: String,
}
