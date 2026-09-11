use crate::{
    device::{self, Device},
    output,
    scenario::{Mode, Scenario},
};
use anyhow::{Context, Result, bail};
use chrono::{SecondsFormat, Utc};
use serde_json::json;
use std::{
    path::{Path, PathBuf},
    thread,
    time::Duration,
};

pub fn run(
    s: &Scenario,
    a_port: Option<&str>,
    b_port: Option<&str>,
    baud: u32,
    root: &Path,
) -> Result<PathBuf> {
    s.validate()?;
    let (ap, bp) = match (a_port, b_port) {
        (Some(a), Some(b)) => (a.to_owned(), b.to_owned()),
        (None, None) => {
            let d = device::discover(baud)?;
            if d.len() != 2 {
                bail!(
                    "expected exactly two harness nodes, found {}; specify --node-a and --node-b",
                    d.len()
                )
            }
            (d[0].port.clone(), d[1].port.clone())
        }
        _ => bail!("specify both --node-a and --node-b"),
    };
    if ap == bp {
        bail!("node A and B ports must differ")
    }
    let mut a = Device::open(&ap, baud)?;
    let mut b = Device::open(&bp, baud)?;
    let a_boot = a.info.boot_id;
    let b_boot = b.info.boot_id;
    let run_id = unique_run_id();
    let initiator = matches!(s.traffic.mode, Mode::Ping | Mode::LatencyUnderLoad);
    let a_role = if initiator {
        "ping_initiator"
    } else {
        "generator"
    };
    let b_role = if initiator { "echo_responder" } else { "sink" };
    let common = |role: &str, peer: &str| json!({"run_id":run_id,"role":role,"peer_mac":peer,"radio":s.radio,"traffic":s.traffic,"duration_us":u64::try_from(s.duration.as_micros()).unwrap_or(u64::MAX)});
    a.request("configure", common(a_role, &b.info.mac))
        .context("configure node A")?;
    b.request("configure", common(b_role, &a.info.mac))
        .context("configure node B")?;
    b.request("arm", json!({"run_id":run_id}))?;
    a.request("arm", json!({"run_id":run_id}))?;
    if s.settle_ms > 0 {
        thread::sleep(Duration::from_millis(s.settle_ms));
    }
    // Sink is started first. Generator uses its local start timestamp, so serial skew does not affect RTT.
    let started = Utc::now().to_rfc3339_opts(SecondsFormat::Millis, true);
    b.request("start", json!({"run_id":run_id}))?;
    a.request("start", json!({"run_id":run_id}))?;
    // No serial traffic is generated during the measurement interval.
    thread::sleep(s.duration + Duration::from_millis(20));
    let _ = a.request("stop", json!({"run_id":run_id}));
    let _ = b.request("stop", json!({"run_id":run_id}));
    let ar = a
        .request("result", json!({"run_id":run_id}))?
        .result
        .ok_or_else(|| anyhow::anyhow!("node A omitted result"))?;
    let br = b
        .request("result", json!({"run_id":run_id}))?
        .result
        .ok_or_else(|| anyhow::anyhow!("node B omitted result"))?;
    let ai = a
        .request("info", json!({}))?
        .info
        .context("node A info missing")?;
    let bi = b
        .request("info", json!({}))?
        .info
        .context("node B info missing")?;
    if ai.boot_id != a_boot || bi.boot_id != b_boot {
        bail!("node reboot detected")
    }
    output::save(root, s, &a.info, &b.info, &ar, &br, &started)
}
fn unique_run_id() -> u32 {
    let n = Utc::now().timestamp_micros() as u64;
    ((n >> 32) ^ (n & 0xffff_ffff)) as u32
}
