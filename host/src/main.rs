mod device;
mod output;
mod protocol;
mod runner;
mod scenario;

use std::path::PathBuf;

use anyhow::Result;
use clap::{Parser, Subcommand};

#[derive(Parser)]
#[command(version, about = "ESP-NOW benchmark harness for ESP32-C5")]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Probe serial ports using the read-only info command.
    Devices {
        #[arg(long, default_value_t = 115_200)]
        baud: u32,
    },
    /// Run a TOML scenario using two identical harness nodes.
    Run {
        scenario: PathBuf,
        #[arg(long)]
        node_a: Option<String>,
        #[arg(long)]
        node_b: Option<String>,
        #[arg(long, default_value = "results")]
        output: PathBuf,
        #[arg(long, default_value_t = 115_200)]
        baud: u32,
    },
    /// Validate and normalize a scenario without touching serial devices.
    Validate { scenario: PathBuf },
}

fn main() -> Result<()> {
    match Cli::parse().command {
        Command::Devices { baud } => {
            let devices = device::discover(baud)?;
            println!("NODE\tPORT\tMAC\tFW\tIDF");
            for (i, d) in devices.iter().enumerate() {
                println!(
                    "{}\t{}\t{}\t{}\t{}",
                    i, d.port, d.info.mac, d.info.firmware_version, d.info.idf_version
                );
            }
            if devices.is_empty() {
                println!("(no harness nodes found)");
            }
        }
        Command::Run {
            scenario,
            node_a,
            node_b,
            output,
            baud,
        } => {
            let scenario = scenario::Scenario::load(&scenario)?;
            let saved = runner::run(
                &scenario,
                node_a.as_deref(),
                node_b.as_deref(),
                baud,
                &output,
            )?;
            println!("results: {}", saved.display());
        }
        Command::Validate { scenario } => {
            let scenario = scenario::Scenario::load(&scenario)?;
            println!("{}", toml::to_string_pretty(&scenario)?);
        }
    }
    Ok(())
}
