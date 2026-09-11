use crate::protocol::{CONTROL_VERSION, Command, Envelope, Info};
use anyhow::{Context, Result, anyhow, bail};
use serde::Serialize;
use serialport::SerialPort;
use std::{
    io::{BufRead, BufReader, Write},
    time::{Duration, Instant},
};

const PROBE_TIMEOUT: Duration = Duration::from_millis(650);
const COMMAND_TIMEOUT: Duration = Duration::from_secs(3);

#[derive(Debug, Clone)]
pub struct Discovered {
    pub port: String,
    pub info: Info,
}

pub struct Device {
    pub port_name: String,
    reader: BufReader<Box<dyn SerialPort>>,
    next_id: u64,
    pub info: Info,
}

pub fn discover(baud: u32) -> Result<Vec<Discovered>> {
    let mut found = Vec::new();
    for p in serialport::available_ports().context("enumerate serial ports")? {
        if let Ok(mut d) = Device::open_and_probe(&p.port_name, baud, PROBE_TIMEOUT) {
            found.push(Discovered {
                port: p.port_name,
                info: d.info.clone(),
            });
            let _ = d.reader.get_mut().flush();
        }
    }
    Ok(found)
}

impl Device {
    pub fn open(port: &str, baud: u32) -> Result<Self> {
        Self::open_and_probe(port, baud, COMMAND_TIMEOUT)
    }
    fn open_and_probe(port: &str, baud: u32, timeout: Duration) -> Result<Self> {
        let serial = serialport::new(port, baud)
            .timeout(Duration::from_millis(80))
            .open()
            .with_context(|| format!("open serial port {port}"))?;
        let mut d = Self {
            port_name: port.into(),
            reader: BufReader::new(serial),
            next_id: 1,
            info: placeholder_info(),
        };
        d.drain(Duration::from_millis(80));
        let env = d.request_with_timeout("info", serde_json::json!({}), timeout)?;
        let info = env
            .info
            .ok_or_else(|| anyhow!("{port}: response is not harness info"))?;
        if info.protocol_version != CONTROL_VERSION {
            bail!(
                "{port}: incompatible protocol version {}",
                info.protocol_version
            );
        }
        d.info = info;
        Ok(d)
    }
    fn drain(&mut self, duration: Duration) {
        let end = Instant::now() + duration;
        let mut line = String::new();
        while Instant::now() < end {
            line.clear();
            let _ = self.reader.read_line(&mut line);
        }
    }
    pub fn request<T: Serialize>(&mut self, cmd: &str, body: T) -> Result<Envelope> {
        self.request_with_timeout(cmd, body, COMMAND_TIMEOUT)
    }
    pub fn request_with_timeout<T: Serialize>(
        &mut self,
        cmd: &str,
        body: T,
        timeout: Duration,
    ) -> Result<Envelope> {
        let id = self.next_id;
        self.next_id += 1;
        let wire = serde_json::to_string(&Command {
            version: CONTROL_VERSION,
            id,
            cmd,
            body,
        })?;
        writeln!(self.reader.get_mut(), "{wire}")
            .with_context(|| format!("{} disconnected while writing", self.port_name))?;
        self.reader.get_mut().flush()?;
        let deadline = Instant::now() + timeout;
        let mut line = String::new();
        while Instant::now() < deadline {
            line.clear();
            match self.reader.read_line(&mut line) {
                Ok(0) => continue,
                Ok(_) => {}
                Err(e) if e.kind() == std::io::ErrorKind::TimedOut => continue,
                Err(e) => {
                    return Err(e)
                        .with_context(|| format!("{} disconnected while reading", self.port_name));
                }
            }
            let Ok(env) = serde_json::from_str::<Envelope>(line.trim()) else {
                continue;
            }; // boot/ROM logs are ignored
            if env.kind == "event" {
                continue;
            }
            if env.id != Some(id) {
                continue;
            }
            if env.ok == Some(false) {
                bail!(
                    "{} {}: {}",
                    env.code.unwrap_or_else(|| "node_error".into()),
                    cmd,
                    env.message.unwrap_or_default()
                );
            }
            return Ok(env);
        }
        bail!("{} timed out waiting for {cmd}", self.port_name)
    }
}

fn placeholder_info() -> Info {
    Info {
        protocol_version: 0,
        mac: String::new(),
        chip: String::new(),
        firmware_version: String::new(),
        idf_version: String::new(),
        capabilities: crate::protocol::Capabilities {
            max_packet_size: 0,
            max_streams: 0,
            max_tx_window: 1,
            bands: vec![],
            phy_rates: vec![],
            encryption: false,
        },
        boot_id: 0,
    }
}
