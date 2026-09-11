use crate::protocol::BENCH_HEADER_LEN;
use anyhow::{Context, Result, bail};
use serde::{Deserialize, Serialize};
use std::{fs, path::Path, time::Duration};

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Scenario {
    pub name: String,
    #[serde(with = "humantime_serde")]
    pub duration: Duration,
    #[serde(default)]
    pub settle_ms: u64,
    pub radio: Radio,
    pub traffic: Traffic,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Radio {
    pub band: Band,
    #[serde(default = "default_country")]
    pub country: String,
    pub channel: u8,
    #[serde(default = "default_phy")]
    pub phy_rate: String,
    #[serde(default = "default_tx_power")]
    pub tx_power_dbm: f32,
    #[serde(default)]
    pub power_save: bool,
    #[serde(default)]
    pub encryption: bool,
    #[serde(default)]
    pub key: Option<String>,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Band {
    #[serde(rename = "2.4ghz")]
    Ghz2,
    #[serde(rename = "5ghz")]
    Ghz5,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum Mode {
    Ping,
    Stream,
    MultiStream,
    Saturation,
    LatencyUnderLoad,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Traffic {
    pub mode: Mode,
    #[serde(default = "one")]
    pub streams: u16,
    #[serde(default = "one")]
    pub tx_window: u16,
    pub packet_size: u16,
    #[serde(default)]
    pub packet_rate_per_stream: u32,
    #[serde(default)]
    pub probe_rate: u32,
    #[serde(default)]
    pub background_packet_size: u16,
    #[serde(default)]
    pub background_rate: u32,
}
fn one() -> u16 {
    1
}
fn default_phy() -> String {
    "mcs0".into()
}
fn default_country() -> String {
    "JP".into()
}
fn default_tx_power() -> f32 {
    20.0
}

impl Scenario {
    pub fn load(path: &Path) -> Result<Self> {
        let text = fs::read_to_string(path).with_context(|| format!("read {}", path.display()))?;
        let s: Self = toml::from_str(&text).context("parse scenario TOML")?;
        s.validate()?;
        Ok(s)
    }
    pub fn validate(&self) -> Result<()> {
        if self.name.trim().is_empty() || self.name.contains(['/', '\\']) {
            bail!("name must be a non-empty filename-safe value");
        }
        if self.duration.is_zero() || self.duration > Duration::from_secs(86_400) {
            bail!("duration must be 1us..24h");
        }
        if !(BENCH_HEADER_LEN as u16..=250).contains(&self.traffic.packet_size) {
            bail!(
                "packet_size must be {BENCH_HEADER_LEN}..250 and means the complete ESP-NOW application frame"
            );
        }
        if !(1..=16).contains(&self.traffic.streams) {
            bail!("streams must be 1..16");
        }
        if !(1..=32).contains(&self.traffic.tx_window) {
            bail!("tx_window must be 1..32");
        }
        let channel_ok = match self.radio.band {
            Band::Ghz2 => (1..=14).contains(&self.radio.channel),
            Band::Ghz5 => matches!(
                self.radio.channel,
                36 | 40 | 44 | 48 | 149 | 153 | 157 | 161 | 165
            ),
        };
        if !channel_ok {
            bail!("unsupported or DFS channel for selected band");
        }
        if self.radio.country.len() != 2
            || !self.radio.country.bytes().all(|b| b.is_ascii_uppercase())
        {
            bail!("radio.country must be a two-letter uppercase country code");
        }
        if self.radio.tx_power_dbm < 2.0
            || self.radio.tx_power_dbm > 20.0
            || (self.radio.tx_power_dbm * 4.0).fract() != 0.0
        {
            bail!("tx_power_dbm must be 2..20 in 0.25 dBm units");
        }
        if self.radio.encryption
            && !self
                .radio
                .key
                .as_deref()
                .is_some_and(|k| k.len() == 32 && k.bytes().all(|b| b.is_ascii_hexdigit()))
        {
            bail!("encrypted scenarios require a 32-hex-digit key (16-byte PMK/LMK material)");
        }
        let valid_rate = matches!(
            self.radio.phy_rate.as_str(),
            "1m" | "6m" | "24m" | "54m" | "mcs0" | "mcs7" | "he-mcs0" | "he-mcs7"
        );
        if !valid_rate || matches!(self.radio.band, Band::Ghz5) && self.radio.phy_rate == "1m" {
            bail!("unsupported PHY rate for selected band");
        }
        match self.traffic.mode {
            Mode::Saturation => {}
            Mode::LatencyUnderLoad
                if self.traffic.probe_rate > 0
                    && self.traffic.background_rate > 0
                    && self.traffic.background_packet_size >= BENCH_HEADER_LEN as u16
                    && self.traffic.background_packet_size <= 250 => {}
            Mode::LatencyUnderLoad => bail!(
                "latency-under-load needs probe_rate, background_rate, and background_packet_size 28..250"
            ),
            _ if self.traffic.packet_rate_per_stream == 0 => {
                bail!("packet_rate_per_stream must be positive")
            }
            _ => {}
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn valid() -> Scenario {
        Scenario {
            name: "x".into(),
            duration: Duration::from_secs(1),
            settle_ms: 0,
            radio: Radio {
                band: Band::Ghz2,
                country: "JP".into(),
                channel: 6,
                phy_rate: "mcs0".into(),
                tx_power_dbm: 20.0,
                power_save: false,
                encryption: false,
                key: None,
            },
            traffic: Traffic {
                mode: Mode::Stream,
                streams: 1,
                tx_window: 1,
                packet_size: 64,
                packet_rate_per_stream: 100,
                probe_rate: 0,
                background_packet_size: 0,
                background_rate: 0,
            },
        }
    }
    #[test]
    fn validates_basics() {
        assert!(valid().validate().is_ok());
    }
    #[test]
    fn rejects_small_packet() {
        let mut s = valid();
        s.traffic.packet_size = 27;
        assert!(s.validate().is_err());
    }
    #[test]
    fn rejects_band_channel_mismatch() {
        let mut s = valid();
        s.radio.band = Band::Ghz5;
        assert!(s.validate().is_err());
    }
    #[test]
    fn rejects_invalid_tx_window() {
        let mut s = valid();
        s.traffic.tx_window = 0;
        assert!(s.validate().is_err());
        s.traffic.tx_window = 33;
        assert!(s.validate().is_err());
    }
    #[test]
    fn tx_window_defaults_to_one() {
        let t = "name='x'\nduration='1s'\n[radio]\nband='2.4ghz'\nchannel=6\n[traffic]\nmode='stream'\npacket_size=64\npacket_rate_per_stream=1";
        let scenario: Scenario = toml::from_str(t).unwrap();
        assert_eq!(scenario.traffic.tx_window, 1);
    }
    #[test]
    fn rejects_unknown_field() {
        let t = "name='x'\nduration='1s'\nwat=1\n[radio]\nband='2.4ghz'\nchannel=6\n[traffic]\nmode='stream'\npacket_size=64\npacket_rate_per_stream=1";
        assert!(toml::from_str::<Scenario>(t).is_err());
    }
}
