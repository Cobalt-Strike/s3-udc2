use crate::{args::Args, s3::c2::spawn_channel};
use anyhow::Result;

mod args;
mod s3;

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt::init();
    let (c2_cfg, listener_cfg) = Args::from_cli().await?;
    let c2 = spawn_channel(c2_cfg).await;
    udc2_relay::clue(c2, listener_cfg).await?;
    Ok(())
}
