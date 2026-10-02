use anyhow::Context;
use anyhow::{Result, bail};
use base64::Engine as _;
use base64::engine::general_purpose::STANDARD as BASE64;
use chrono::{Duration, Utc};
use clap::{Parser, ValueEnum};
use hmac::{Hmac, Mac};
use http_body_util::BodyExt;
use hyper::Uri;
use serde_json::json;
use sha2::Sha256;
use std::collections::HashMap;
use std::fs;
use std::path::PathBuf;
use std::{env, io};
use tracing::{error, info};

use crate::verify::build_https_client;
use crate::verify::upload_file;

mod verify;

type HmacSha256 = Hmac<Sha256>;

fn hmac_sha256(key: &[u8], data: &str) -> Vec<u8> {
    let mut mac = HmacSha256::new_from_slice(key).expect("HMAC can take key of any size");
    mac.update(data.as_bytes());
    mac.finalize().into_bytes().to_vec()
}

/// Rough Rust equivalent of boto3's `generate_presigned_post`.
///
/// Returns:
///   - `url`   = S3 endpoint to POST to
///   - `fields` = form fields for your multipart/form-data POST
#[allow(clippy::too_many_arguments)]
pub fn generate_presigned_post(
    now: chrono::DateTime<Utc>,
    access_key: &str,
    secret_key: &str,
    session_token: Option<&str>, // for STS/assumed roles
    region: &str,
    bucket: &str,
    key: &str,
    expires_in_secs: i64,
    max_content_length: i64,
) -> (String, HashMap<String, String>) {
    // 1. Time / credential scope
    let amz_date = now.format("%Y%m%dT%H%M%SZ").to_string(); // e.g. 20251123T120000Z
    let date_stamp = now.format("%Y%m%d").to_string(); // e.g. 20251123

    let credential_scope = format!("{}/{}/s3/aws4_request", date_stamp, region);
    let credential = format!("{}/{}", access_key, credential_scope);
    let algorithm = "AWS4-HMAC-SHA256";

    // ISO8601 expiration timestamp for the policy
    let expiration = (now + Duration::seconds(expires_in_secs))
        .to_rfc3339_opts(chrono::SecondsFormat::Secs, true);

    // 2. Build the policy document
    let mut conditions = vec![
        if let Some(prefix) = key.strip_suffix("${filename}") {
            json!(["starts-with", "$key", prefix])
        } else {
            json!({"key": key})
        },
        json!(["content-length-range", 0, max_content_length]),
        json!({ "bucket": bucket }),
        json!({ "x-amz-algorithm": algorithm }),
        json!({ "x-amz-credential": credential }),
        json!({ "x-amz-date": amz_date }),
    ];

    if let Some(token) = session_token {
        conditions.push(json!({ "x-amz-security-token": token }));
    }

    let policy_doc = json!({
        "expiration": expiration,
        "conditions": conditions,
    });

    let policy_str = policy_doc.to_string();
    let policy_b64 = BASE64.encode(policy_str.as_bytes());

    // 3. Derive the SigV4 signing key
    let k_secret = format!("AWS4{}", secret_key).into_bytes();
    let k_date = hmac_sha256(&k_secret, &date_stamp);
    let k_region = hmac_sha256(&k_date, region);
    let k_service = hmac_sha256(&k_region, "s3");
    let k_signing = hmac_sha256(&k_service, "aws4_request");

    // 4. Sign the base64-encoded policy
    let signature_bytes = hmac_sha256(&k_signing, &policy_b64);
    let signature_hex = hex::encode(signature_bytes);

    // 5. Build the POST URL + fields
    let url = if region == "us-east-1" {
        format!("https://{}.s3.amazonaws.com", bucket)
    } else {
        format!("https://{}.s3.{}.amazonaws.com", bucket, region)
    };

    let mut fields = HashMap::new();
    fields.insert("key".into(), key.to_string());
    fields.insert("policy".into(), policy_b64);
    fields.insert("x-amz-algorithm".into(), algorithm.to_string());
    fields.insert("x-amz-credential".into(), credential);
    fields.insert("x-amz-date".into(), amz_date);
    fields.insert("x-amz-signature".into(), signature_hex);

    if let Some(token) = session_token {
        fields.insert("x-amz-security-token".into(), token.to_string());
    }

    (url, fields)
}

#[derive(Parser)]
#[clap(about = "Generate S3 presign and emit config")]
struct Cli {
    /// Path to terraform JSON output (optional)
    #[clap(long, value_parser)]
    config: Option<PathBuf>,

    /// Bucket name (if no json)
    #[clap(long)]
    bucket: Option<String>,

    /// AWS region
    #[clap(long, default_value = "eu-central-1")]
    region: String,

    /// upload prefix (client->server)
    #[clap(long, default_value = "c2s")]
    upload_prefix: String,

    /// download prefix (server->client)
    #[clap(long, default_value = "s2c")]
    download_prefix: String,

    /// Output format
    #[clap(long, value_enum, default_value_t = OutputFormat::Migrate)]
    output: OutputFormat,

    /// Presign expiration in seconds
    #[clap(long, default_value_t = 7 * 24 * 60 * 60)]
    expires: i64,

    /// Max allowed content length for uploads
    #[clap(long, default_value_t = 10_000)]
    max_content: i64,

    /// User agent string to embed in C header
    #[clap(long, default_value = "s3-udc2")]
    user_agent: String,

    /// Maximum frame retries
    #[clap(long, default_value_t = 10)]
    max_frame_retries: i32,

    /// Frame retry delay in milliseconds
    #[clap(long, default_value_t = 2000)]
    frame_retry_delay_ms: i32,

    /// Try to upload a test file into S3
    #[clap(long)]
    verify: bool,
}

#[derive(Copy, Clone, PartialEq, Eq, PartialOrd, Ord, ValueEnum)]
enum OutputFormat {
    Migrate,
    Header,
}

fn read_config(path: &PathBuf) -> Result<HashMap<String, String>> {
    let data = fs::read_to_string(path)
        .context(format!("Could not read: {}", path.to_str().unwrap_or("")))?;
    let mut m = HashMap::new();

    // Choose parser by extension if present, otherwise try JSON then TOML
    let is_toml = path
        .extension()
        .and_then(|s| s.to_str())
        .map(|s| s.eq_ignore_ascii_case("toml"))
        .unwrap_or(false);

    if is_toml {
        let v: toml::Value = toml::from_str(&data)?;
        if let toml::Value::Table(table) = v {
            for (k, val) in table.into_iter() {
                match val {
                    toml::Value::String(s) => {
                        m.insert(k, s);
                    }
                    toml::Value::Table(mut t) => {
                        if let Some(tv) = t.remove("value") {
                            match tv {
                                toml::Value::String(s) => {
                                    m.insert(k, s);
                                }
                                other => {
                                    m.insert(k, other.to_string());
                                }
                            }
                        } else {
                            m.insert(k, toml::Value::Table(t).to_string());
                        }
                    }
                    other => {
                        m.insert(k, other.to_string());
                    }
                }
            }
        }
    } else {
        // try json
        let v: serde_json::Value = serde_json::from_str(&data)?;
        if let serde_json::Value::Object(map) = v {
            for (k, val) in map.iter() {
                if let Some(inner) = val.get("value") {
                    if inner.is_string() {
                        m.insert(k.clone(), inner.as_str().unwrap().to_string());
                    } else {
                        m.insert(k.clone(), inner.to_string());
                    }
                } else if val.is_string() {
                    m.insert(k.clone(), val.as_str().unwrap().to_string());
                }
            }
        }
    }

    Ok(m)
}

fn c_escape(s: &str) -> String {
    s.replace('\\', "\\\\").replace('"', "\\\"")
}

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt().with_writer(io::stderr).init();

    let cli = Cli::parse();

    // Load values either from terraform json or from args/env
    let (bucket, upload_prefix, download_prefix, access_key, secret_key, session_token) =
        if let Some(path) = cli.config.as_ref() {
            let map = read_config(path)?;
            let bucket = map
                .get("bucket_name")
                .or_else(|| map.get("bucket"))
                .cloned()
                .expect("bucket_name not found in json");
            let upload_prefix = map
                .get("client_to_server_prefix")
                .or_else(|| map.get("upload_prefix"))
                .cloned()
                .unwrap_or_else(|| cli.upload_prefix.clone());
            let download_prefix = map
                .get("server_to_client_prefix")
                .or_else(|| map.get("download_prefix"))
                .cloned()
                .unwrap_or_else(|| cli.download_prefix.clone());

            let access_key = map
                .get("write_only_user_access_key_id")
                .or_else(|| map.get("relay_user_access_key_id"))
                .cloned()
                .expect("access key not found in json");
            let secret_key = map
                .get("write_only_user_secret_access_key")
                .or_else(|| map.get("relay_user_secret_access_key"))
                .cloned()
                .expect("secret key not found in json");

            (
                bucket,
                upload_prefix,
                download_prefix,
                access_key,
                secret_key,
                None,
            )
        } else {
            let bucket = cli
                .bucket
                .clone()
                .expect("--bucket must be provided when no json is given");
            let upload_prefix = cli.upload_prefix.clone();
            let download_prefix = cli.download_prefix.clone();
            let access_key = env::var("AWS_ACCESS_KEY_ID").expect("AWS_ACCESS_KEY_ID not set");
            let secret_key =
                env::var("AWS_SECRET_ACCESS_KEY").expect("AWS_SECRET_ACCESS_KEY not set");
            let session_token = env::var("AWS_SESSION_TOKEN").ok();
            (
                bucket,
                upload_prefix,
                download_prefix,
                access_key,
                secret_key,
                session_token,
            )
        };

    let key = format!("{}/${{filename}}", upload_prefix);

    let now = Utc::now();
    let (url, fields) = generate_presigned_post(
        now,
        &access_key,
        &secret_key,
        session_token.as_deref(),
        &cli.region,
        &bucket,
        &key,
        cli.expires,
        cli.max_content,
    );

    let credential = fields
        .get("x-amz-credential")
        .map(|s| s.as_str())
        .unwrap_or("");
    let amz_date = fields.get("x-amz-date").map(|s| s.as_str()).unwrap_or("");
    let signature = fields
        .get("x-amz-signature")
        .map(|s| s.as_str())
        .unwrap_or("");
    let policy = fields.get("policy").map(|s| s.as_str()).unwrap_or("");

    if cli.verify {
        let upload_url: Uri = url.try_into()?;
        let client = build_https_client()?;
        let file_name = format!("verify_{}", uuid::Uuid::new_v4());
        let content = [];
        let resp = upload_file(&client, &upload_url, &fields, &file_name, &content)
            .await
            .context("could not upload the test file into S3")?;
        match resp.status().as_u16() {
            204 => info!("succesfully uploaded the test file into S3"),
            code => {
                let bytes = resp.into_body().collect().await?.to_bytes();
                let body = String::from_utf8(bytes.to_vec())?;
                error!(%code, %body, "could not upload the test file into S3");
                bail!("could not upload the test file into S3");
            }
        }
    }

    if matches!(cli.output, OutputFormat::Migrate) {
        // plain key=value
        let host = if cli.region == "us-east-1" {
            format!("{}.s3.amazonaws.com", bucket)
        } else {
            format!("{}.s3.{}.amazonaws.com", bucket, cli.region)
        };
        println!("s3-host={}", host);
        println!("s3-upload-key={}", upload_prefix);
        println!("s3-download-key={}", download_prefix);
        println!("s3-x-amz-credential={}", credential);
        println!("s3-x-amz-date={}", amz_date);
        println!("s3-x-amz-signature={}", signature);
        println!("s3-x-amz-policy={}", policy);
    } else {
        // C header
        println!("#pragma once");
        println!();
        println!("#define C2_STATE_KEY         \".s3-udc2.state\"");
        println!(
            "#define USER_AGENT           L\"{}\"",
            c_escape(&cli.user_agent)
        );
        println!("#define MAX_FRAME_RETRIES    {}", cli.max_frame_retries);
        println!("#define FRAME_RETRY_DELAY_MS {}", cli.frame_retry_delay_ms);
        println!();
        let host = if cli.region == "us-east-1" {
            format!("{}.s3.amazonaws.com", bucket)
        } else {
            format!("{}.s3.{}.amazonaws.com", bucket, cli.region)
        };
        println!("#define S3_HOST          L\"{}\"", c_escape(&host));
        println!("#define S3_UPLOAD_KEY    \"{}\"", c_escape(&upload_prefix));
        println!(
            "#define S3_DOWNLOAD_KEY  \"{}\"",
            c_escape(&download_prefix)
        );
        println!("#define X_AMZ_CREDENTIAL \"{}\"", c_escape(credential));
        println!("#define X_AMZ_DATE       \"{}\"", c_escape(amz_date));
        println!("#define X_AMZ_SIGNATURE  \"{}\"", c_escape(signature));
        println!("#define X_AMZ_POLICY     \"{}\"", c_escape(policy));
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value;

    fn fixed_now() -> chrono::DateTime<Utc> {
        let naive = chrono::NaiveDate::from_ymd_opt(2025, 12, 24)
            .unwrap()
            .and_hms_opt(12, 0, 0)
            .unwrap();
        chrono::DateTime::from_naive_utc_and_offset(naive, Utc)
    }

    #[test]
    fn policy_contains_starts_with_for_filename() {
        let now = fixed_now();
        let access = "AKIDEXAMPLE";
        let secret = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
        let region = "us-west-2";
        let bucket = "my-bucket";
        let key = "c2s/${filename}";

        let (_url, fields) =
            generate_presigned_post(now, access, secret, None, region, bucket, key, 3600, 10_000);

        let policy_b64 = fields.get("policy").expect("policy exists");
        let policy_bytes = BASE64.decode(policy_b64.as_bytes()).expect("base64 decode");
        let policy_json: Value = serde_json::from_slice(&policy_bytes).expect("valid json");

        let conditions = policy_json
            .get("conditions")
            .expect("conditions")
            .as_array()
            .expect("array");

        // First condition should be starts-with for the key prefix
        let first = &conditions[0];
        let arr = first.as_array().expect("starts-with array");
        assert_eq!(arr[0].as_str().unwrap(), "starts-with");
        assert_eq!(arr[1].as_str().unwrap(), "$key");
        assert_eq!(arr[2].as_str().unwrap(), "c2s/");
    }

    #[test]
    fn credential_has_expected_scope() {
        let now = fixed_now();
        let access = "AKIDEXAMPLE";
        let secret = "secret";
        let region = "eu-central-1";
        let bucket = "b";
        let key = "c2s/${filename}";

        let (_url, fields) =
            generate_presigned_post(now, access, secret, None, region, bucket, key, 3600, 10_000);

        let date_stamp = now.format("%Y%m%d").to_string();
        let expected_scope = format!("{}/{}/s3/aws4_request", date_stamp, region);
        let expected_credential = format!("{}/{}", access, expected_scope);

        assert_eq!(
            fields.get("x-amz-credential").unwrap(),
            &expected_credential
        );
    }
}
