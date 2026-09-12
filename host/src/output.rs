use crate::{
    protocol::{CONTROL_VERSION, Info},
    scenario::{Mode, Scenario},
};
use anyhow::Result;
use chrono::{SecondsFormat, Utc};
use serde::{Deserialize, Serialize};
use std::{
    fs,
    path::{Path, PathBuf},
};

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[allow(dead_code)]
pub struct Percentiles {
    pub min_us: u64,
    pub mean_us: f64,
    pub p50_us: u64,
    pub p90_us: u64,
    pub p95_us: u64,
    pub p99_us: u64,
    pub p999_us: u64,
    pub max_us: u64,
    pub stddev_us: f64,
}

#[allow(dead_code)]
pub fn percentile(samples: &mut [u64], q: f64) -> u64 {
    if samples.is_empty() {
        return 0;
    }
    samples.sort_unstable();
    let rank = (samples.len() as f64 * q).ceil().max(1.0) as usize;
    samples[(rank - 1).min(samples.len() - 1)]
}
#[allow(dead_code)]
pub fn summarize_samples(samples: &[u64]) -> Percentiles {
    if samples.is_empty() {
        return Percentiles::default();
    }
    let mean = samples.iter().map(|&x| x as f64).sum::<f64>() / samples.len() as f64;
    let var = samples
        .iter()
        .map(|&x| {
            let d = x as f64 - mean;
            d * d
        })
        .sum::<f64>()
        / samples.len() as f64;
    let mut s = samples.to_vec();
    Percentiles {
        min_us: *samples.iter().min().unwrap(),
        mean_us: mean,
        p50_us: percentile(&mut s, 0.5),
        p90_us: percentile(&mut s, 0.9),
        p95_us: percentile(&mut s, 0.95),
        p99_us: percentile(&mut s, 0.99),
        p999_us: percentile(&mut s, 0.999),
        max_us: *samples.iter().max().unwrap(),
        stddev_us: var.sqrt(),
    }
}

#[derive(Serialize)]
pub struct Metadata<'a> {
    pub host_version: &'static str,
    pub git_commit: Option<String>,
    pub start_time: String,
    pub requested_duration_ms: u128,
    pub scenario: &'a Scenario,
    pub nodes: [&'a Info; 2],
}

fn stream_tx_counters(result: &serde_json::Value, stream_id: u64) -> (u64, u64) {
    let stream = result
        .pointer("/streams")
        .and_then(|v| v.as_array())
        .and_then(|streams| {
            streams
                .iter()
                .find(|s| s.get("stream_id").and_then(|v| v.as_u64()) == Some(stream_id))
        });
    let counter = |name| {
        stream
            .and_then(|s| s.get(name))
            .and_then(|v| v.as_u64())
            .unwrap_or(0)
    };
    (counter("tx_requested"), counter("tx_submitted"))
}

pub fn save(
    root: &Path,
    scenario: &Scenario,
    a: &Info,
    b: &Info,
    a_result: &serde_json::Value,
    b_result: &serde_json::Value,
    start: &str,
) -> Result<PathBuf> {
    let stamp = Utc::now()
        .to_rfc3339_opts(SecondsFormat::Secs, true)
        .replace(':', "");
    let safe: String = scenario
        .name
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '-' || c == '_' {
                c
            } else {
                '_'
            }
        })
        .collect();
    let dir = root.join(format!("{stamp}-{safe}"));
    fs::create_dir_all(&dir)?;
    fs::write(dir.join("scenario.toml"), toml::to_string_pretty(scenario)?)?;
    let git_commit = std::process::Command::new("git")
        .args(["rev-parse", "HEAD"])
        .output()
        .ok()
        .filter(|o| o.status.success())
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_owned());
    let meta = Metadata {
        host_version: env!("CARGO_PKG_VERSION"),
        git_commit,
        start_time: start.into(),
        requested_duration_ms: scenario.duration.as_millis(),
        scenario,
        nodes: [a, b],
    };
    fs::write(dir.join("metadata.json"), serde_json::to_vec_pretty(&meta)?)?;
    let tx = a_result
        .pointer("/totals/tx_submitted")
        .and_then(|v| v.as_u64())
        .unwrap_or(0);
    let rx = b_result
        .pointer("/totals/rx_packets")
        .and_then(|v| v.as_u64())
        .unwrap_or(0);
    let dup = b_result
        .pointer("/totals/duplicate")
        .and_then(|v| v.as_u64())
        .unwrap_or(0);
    let unique_rx = rx.saturating_sub(dup);
    let loss = tx.saturating_sub(unique_rx);
    let rx_bytes = b_result
        .pointer("/totals/rx_bytes")
        .and_then(|v| v.as_u64())
        .unwrap_or(0);
    let tx_window_us = a_result
        .pointer("/totals/tx_window_us")
        .and_then(|v| v.as_u64())
        .unwrap_or(0);
    let rx_goodput_bps_tx_window = if tx_window_us > 0 {
        rx_bytes as f64 * 8_000_000.0 / tx_window_us as f64
    } else {
        0.0
    };
    let rtt = a_result
        .pointer("/rtt/sample_count")
        .and_then(|v| v.as_u64())
        .unwrap_or(0);
    let probe_stream = if matches!(scenario.traffic.mode, Mode::LatencyUnderLoad) {
        15
    } else {
        0
    };
    let (probe_requested, probe_submitted) = stream_tx_counters(a_result, probe_stream);
    let aggregate = serde_json::json!({"tx_packets":tx,"unique_rx_packets":unique_rx,"forward_packet_loss":loss,"forward_packet_loss_percent":if tx>0{loss as f64*100.0/tx as f64}else{0.0},"rx_goodput_bps_tx_window":rx_goodput_bps_tx_window,"tx_window_us":tx_window_us,"rtt_probes_requested":probe_requested,"rtt_probes_submitted":probe_submitted,"completed_rtt_probes":rtt,"local_probe_drop":probe_requested.saturating_sub(probe_submitted),"network_or_echo_probe_loss":probe_submitted.saturating_sub(rtt)});
    let combined = serde_json::json!({"protocol_version":CONTROL_VERSION,"aggregate":aggregate,"node_a":a_result,"node_b":b_result});
    fs::write(
        dir.join("result.json"),
        serde_json::to_vec_pretty(&combined)?,
    )?;
    let mut w = csv::Writer::from_path(dir.join("summary.csv"))?;
    w.write_record(["node", "metric", "value"])?;
    flatten_csv(&mut w, "node_a", a_result, "")?;
    flatten_csv(&mut w, "node_b", b_result, "")?;
    flatten_csv(&mut w, "aggregate", &aggregate, "")?;
    w.flush()?;
    println!("run '{}' complete", scenario.name);
    println!(
        "aggregate: tx={} unique_rx={} forward_loss={} ({:.3}%) aligned_rx_goodput={:.0} bps",
        tx,
        unique_rx,
        loss,
        if tx > 0 {
            loss as f64 * 100.0 / tx as f64
        } else {
            0.0
        },
        rx_goodput_bps_tx_window
    );
    if matches!(scenario.traffic.mode, Mode::Ping | Mode::LatencyUnderLoad) {
        println!(
            "RTT probes: requested={} submitted={} completed={} local_drop={} network_or_echo_loss={}",
            probe_requested,
            probe_submitted,
            rtt,
            probe_requested.saturating_sub(probe_submitted),
            probe_submitted.saturating_sub(rtt)
        );
    }
    print_summary("A", a_result);
    print_summary("B", b_result);
    Ok(dir)
}
fn flatten_csv(
    w: &mut csv::Writer<std::fs::File>,
    node: &str,
    v: &serde_json::Value,
    prefix: &str,
) -> Result<()> {
    match v {
        serde_json::Value::Object(m) => {
            for (k, x) in m {
                let p = if prefix.is_empty() {
                    k.clone()
                } else {
                    format!("{prefix}.{k}")
                };
                flatten_csv(w, node, x, &p)?
            }
        }
        serde_json::Value::Array(items) => {
            for (index, item) in items.iter().enumerate() {
                flatten_csv(w, node, item, &format!("{prefix}[{index}]"))?;
            }
        }
        _ => w.write_record([node, prefix, &v.to_string()])?,
    }
    Ok(())
}
fn print_summary(node: &str, v: &serde_json::Value) {
    let g = |k| v.pointer(k).cloned().unwrap_or(serde_json::Value::Null);
    println!(
        "node {node}: tx={} rx={} loss={} send_fail={} slot_timeout={} rssi_mean={} dBm rtt_p99={} us dispatch_lateness_p99={} us active_rx_goodput={} bps",
        g("/totals/tx_submitted"),
        g("/totals/rx_packets"),
        g("/totals/estimated_loss"),
        g("/totals/send_cb_failure"),
        g("/totals/tx_slot_timeout"),
        g("/rssi/mean"),
        g("/rtt/p99_us"),
        g("/dispatch_lateness/p99_us"),
        g("/totals/goodput_bps_active")
    );
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn percentiles_are_nearest_rank() {
        let s = (1..=1000).collect::<Vec<_>>();
        let p = summarize_samples(&s);
        assert_eq!(
            (p.p50_us, p.p99_us, p.p999_us, p.max_us),
            (500, 990, 999, 1000)
        );
    }
    #[test]
    fn percentiles_empty() {
        assert_eq!(summarize_samples(&[]).max_us, 0);
    }
    #[test]
    fn result_serializes() {
        assert!(
            serde_json::to_string(&summarize_samples(&[1, 2, 3]))
                .unwrap()
                .contains("p99_us")
        );
    }
    #[test]
    fn extracts_requested_and_submitted_per_stream() {
        let result = serde_json::json!({"streams":[
            {"stream_id":0,"tx_requested":100,"tx_submitted":97},
            {"stream_id":15,"tx_requested":20,"tx_submitted":18}
        ]});
        assert_eq!(stream_tx_counters(&result, 15), (20, 18));
        assert_eq!(stream_tx_counters(&result, 7), (0, 0));
    }
}
