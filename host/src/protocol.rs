use anyhow::{Result, bail};
use serde::{Deserialize, Serialize};

pub const CONTROL_VERSION: u8 = 2;
pub const BENCH_PROTOCOL_VERSION: u8 = 1;
#[allow(dead_code)]
pub const BENCH_MAGIC: u32 = 0x4553_504e;
pub const BENCH_HEADER_LEN: usize = 28;

#[derive(Debug, Clone, PartialEq, Eq)]
#[allow(dead_code)]
pub struct BenchHeader {
    pub packet_type: u8,
    pub flags: u16,
    pub run_id: u32,
    pub stream_id: u16,
    pub packet_len: u16,
    pub seq: u32,
    pub timestamp_us: u64,
}

impl BenchHeader {
    #[allow(dead_code)]
    pub fn encode(&self) -> [u8; BENCH_HEADER_LEN] {
        let mut b = [0; BENCH_HEADER_LEN];
        b[0..4].copy_from_slice(&BENCH_MAGIC.to_be_bytes());
        b[4] = BENCH_PROTOCOL_VERSION;
        b[5] = self.packet_type;
        b[6..8].copy_from_slice(&self.flags.to_be_bytes());
        b[8..12].copy_from_slice(&self.run_id.to_be_bytes());
        b[12..14].copy_from_slice(&self.stream_id.to_be_bytes());
        b[14..16].copy_from_slice(&self.packet_len.to_be_bytes());
        b[16..20].copy_from_slice(&self.seq.to_be_bytes());
        b[20..28].copy_from_slice(&self.timestamp_us.to_be_bytes());
        b
    }

    #[allow(dead_code)]
    pub fn decode(b: &[u8]) -> Result<Self> {
        if b.len() < BENCH_HEADER_LEN {
            bail!("benchmark packet shorter than {BENCH_HEADER_LEN}");
        }
        if u32::from_be_bytes(b[0..4].try_into()?) != BENCH_MAGIC {
            bail!("bad benchmark magic");
        }
        if b[4] != BENCH_PROTOCOL_VERSION {
            bail!("unsupported benchmark protocol {}", b[4]);
        }
        let packet_len = u16::from_be_bytes(b[14..16].try_into()?);
        if packet_len as usize != b.len() {
            bail!("packet_len field does not match frame length");
        }
        Ok(Self {
            packet_type: b[5],
            flags: u16::from_be_bytes(b[6..8].try_into()?),
            run_id: u32::from_be_bytes(b[8..12].try_into()?),
            stream_id: u16::from_be_bytes(b[12..14].try_into()?),
            packet_len,
            seq: u32::from_be_bytes(b[16..20].try_into()?),
            timestamp_us: u64::from_be_bytes(b[20..28].try_into()?),
        })
    }
}

#[derive(Debug, Serialize)]
pub struct Command<'a, T: Serialize> {
    pub version: u8,
    pub id: u64,
    pub cmd: &'a str,
    #[serde(flatten)]
    pub body: T,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Capabilities {
    pub max_packet_size: u16,
    pub max_streams: u16,
    #[serde(default = "default_max_tx_window")]
    pub max_tx_window: u16,
    pub bands: Vec<String>,
    pub phy_rates: Vec<String>,
    pub encryption: bool,
}

fn default_max_tx_window() -> u16 {
    1
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Info {
    pub protocol_version: u8,
    pub mac: String,
    pub chip: String,
    pub firmware_version: String,
    pub idf_version: String,
    pub capabilities: Capabilities,
    #[serde(default)]
    pub boot_id: u32,
}

#[derive(Debug, Deserialize)]
pub struct Envelope {
    #[serde(rename = "type")]
    pub kind: String,
    #[serde(default)]
    pub id: Option<u64>,
    #[serde(default)]
    pub ok: Option<bool>,
    #[serde(default)]
    pub code: Option<String>,
    #[serde(default)]
    pub message: Option<String>,
    #[serde(default)]
    pub info: Option<Info>,
    #[serde(default)]
    pub result: Option<serde_json::Value>,
}

#[derive(Default, Debug, Clone)]
#[allow(dead_code)]
pub struct SequenceTracker {
    initialized: bool,
    max_seq: u32,
    seen: std::collections::HashSet<u32>,
    pub received: u64,
    pub duplicate: u64,
    pub out_of_order: u64,
}

impl SequenceTracker {
    #[allow(dead_code)]
    pub fn observe(&mut self, seq: u32) {
        self.received += 1;
        if !self.seen.insert(seq) {
            self.duplicate += 1;
            return;
        }
        if self.initialized && seq < self.max_seq {
            self.out_of_order += 1;
        }
        if !self.initialized || seq > self.max_seq {
            self.max_seq = seq;
            self.initialized = true;
        }
    }
    #[allow(dead_code)]
    pub fn estimated_missing(&self) -> u64 {
        if !self.initialized {
            0
        } else {
            (self.max_seq as u64 + 1).saturating_sub(self.seen.len() as u64)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn header_round_trip_and_endian() {
        let h = BenchHeader {
            packet_type: 2,
            flags: 0x1234,
            run_id: 42,
            stream_id: 7,
            packet_len: 28,
            seq: 0x10203040,
            timestamp_us: 99,
        };
        let b = h.encode();
        assert_eq!(&b[0..4], &[0x45, 0x53, 0x50, 0x4e]);
        assert_eq!(b[4], BENCH_PROTOCOL_VERSION);
        assert_ne!(BENCH_PROTOCOL_VERSION, CONTROL_VERSION);
        assert_eq!(BenchHeader::decode(&b).unwrap(), h);
    }
    #[test]
    fn rejects_length_and_version() {
        let mut b = BenchHeader {
            packet_type: 1,
            flags: 0,
            run_id: 1,
            stream_id: 0,
            packet_len: 28,
            seq: 0,
            timestamp_us: 0,
        }
        .encode();
        b[4] = 9;
        assert!(BenchHeader::decode(&b).is_err());
    }
    #[test]
    fn sequence_classification() {
        let mut s = SequenceTracker::default();
        for n in [0, 2, 2, 1] {
            s.observe(n)
        }
        assert_eq!(
            (s.duplicate, s.out_of_order, s.estimated_missing()),
            (1, 1, 0)
        );
    }
    #[test]
    fn detects_gap() {
        let mut s = SequenceTracker::default();
        s.observe(0);
        s.observe(3);
        assert_eq!(s.estimated_missing(), 2);
    }
}
