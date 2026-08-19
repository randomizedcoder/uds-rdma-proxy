//! Benchmark configuration (§30.6) — modes, verify levels, validation.

use crate::frame::{HDR_SIZE, MSG_MAX};
use crate::Error;
use std::str::FromStr;

pub const BATCH_MAX: u32 = 1024;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    Blocking,
    UringRw,
    UringFixed,
    UringBufring,
    UringSqpoll,
    UringSendzc,
}

impl Mode {
    pub fn as_str(&self) -> &'static str {
        match self {
            Mode::Blocking => "blocking",
            Mode::UringRw => "uring-rw",
            Mode::UringFixed => "uring-fixed",
            Mode::UringBufring => "uring-bufring",
            Mode::UringSqpoll => "uring-sqpoll",
            Mode::UringSendzc => "uring-sendzc",
        }
    }
}

impl FromStr for Mode {
    type Err = Error;

    fn from_str(s: &str) -> Result<Self, Error> {
        match s {
            "blocking" => Ok(Mode::Blocking),
            "uring-rw" => Ok(Mode::UringRw),
            "uring-fixed" => Ok(Mode::UringFixed),
            "uring-bufring" => Ok(Mode::UringBufring),
            "uring-sqpoll" => Ok(Mode::UringSqpoll),
            "uring-sendzc" => Ok(Mode::UringSendzc),
            _ => Err(Error::Invalid),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Verify {
    None,
    Header,
    Full,
}

impl Verify {
    pub fn as_str(&self) -> &'static str {
        match self {
            Verify::None => "none",
            Verify::Header => "header",
            Verify::Full => "full",
        }
    }
}

impl FromStr for Verify {
    type Err = Error;

    fn from_str(s: &str) -> Result<Self, Error> {
        match s {
            "none" => Ok(Verify::None),
            "header" => Ok(Verify::Header),
            "full" => Ok(Verify::Full),
            _ => Err(Error::Invalid),
        }
    }
}

/// App protocol, orthogonal to [`Mode`] (§34.4). `Echo` is the symmetric RTT
/// reflector (default); `Stream` is one-way bulk — the connect side sources,
/// the listen side sinks, and goodput is measured at the sink.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Pattern {
    Echo,
    Stream,
}

impl Pattern {
    pub fn as_str(&self) -> &'static str {
        match self {
            Pattern::Echo => "echo",
            Pattern::Stream => "stream",
        }
    }
}

impl FromStr for Pattern {
    type Err = Error;

    fn from_str(s: &str) -> Result<Self, Error> {
        match s {
            "echo" => Ok(Pattern::Echo),
            "stream" => Ok(Pattern::Stream),
            _ => Err(Error::Invalid),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Role {
    Listen,
    Connect,
}

#[derive(Debug, Clone)]
pub struct Config {
    /// `None` mirrors the C core's `BENCH_ROLE_NONE` (invalid).
    pub role: Option<Role>,
    pub id: u16,
    pub mode: Mode,
    pub verify: Verify,
    /// `Echo` (default) or `Stream` (§34.4).
    pub pattern: Pattern,
    /// Total wire bytes including the header.
    pub msg_size: u32,
    pub batch: u32,
    /// 0 = use `duration_s`.
    pub count: u64,
    /// 0 = use `count`.
    pub duration_s: u32,
    pub defer_taskrun: bool,
}

impl Config {
    pub fn validate(&self) -> Result<(), Error> {
        if self.role.is_none() {
            return Err(Error::Invalid);
        }
        if self.msg_size < HDR_SIZE as u32 || self.msg_size > MSG_MAX {
            return Err(Error::Invalid);
        }
        if self.batch < 1 || self.batch > BATCH_MAX {
            return Err(Error::Invalid);
        }
        if self.count == 0 && self.duration_s == 0 {
            return Err(Error::Invalid);
        }
        if self.count != 0 && self.duration_s != 0 {
            return Err(Error::Invalid);
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn good() -> Config {
        Config {
            role: Some(Role::Connect),
            id: 1,
            mode: Mode::UringRw,
            verify: Verify::Header,
            pattern: Pattern::Echo,
            msg_size: 4076,
            batch: 32,
            count: 1000,
            duration_s: 0,
            defer_taskrun: false,
        }
    }

    #[test]
    fn validate_table() {
        // Mirror of C test_config: same cases, same expectations.
        type Mutate = fn(&mut Config);
        let cases: &[(&str, Mutate, Result<(), Error>)] = &[
            ("good", |_| {}, Ok(())),
            ("msg-below-hdr", |c| c.msg_size = 23, Err(Error::Invalid)),
            ("msg-exact-hdr", |c| c.msg_size = 24, Ok(())),
            ("msg-at-cap", |c| c.msg_size = MSG_MAX, Ok(())),
            (
                "msg-over-cap",
                |c| c.msg_size = MSG_MAX + 1,
                Err(Error::Invalid),
            ),
            ("batch-zero", |c| c.batch = 0, Err(Error::Invalid)),
            ("batch-one", |c| c.batch = 1, Ok(())),
            ("batch-max", |c| c.batch = BATCH_MAX, Ok(())),
            (
                "batch-over",
                |c| c.batch = BATCH_MAX + 1,
                Err(Error::Invalid),
            ),
            ("role-none", |c| c.role = None, Err(Error::Invalid)),
            ("role-listen", |c| c.role = Some(Role::Listen), Ok(())),
            (
                "neither-count-nor-dur",
                |c| {
                    c.count = 0;
                    c.duration_s = 0;
                },
                Err(Error::Invalid),
            ),
            (
                "both-count-and-dur",
                |c| {
                    c.count = 5;
                    c.duration_s = 5;
                },
                Err(Error::Invalid),
            ),
            (
                "duration-only",
                |c| {
                    c.count = 0;
                    c.duration_s = 5;
                },
                Ok(()),
            ),
        ];
        for (name, mutate, want) in cases {
            let mut c = good();
            mutate(&mut c);
            assert_eq!(c.validate(), *want, "case {name}");
        }
    }

    #[test]
    fn mode_verify_parse_table() {
        let cases: &[(&str, Result<Mode, Error>)] = &[
            ("blocking", Ok(Mode::Blocking)),
            ("uring-rw", Ok(Mode::UringRw)),
            ("uring-fixed", Ok(Mode::UringFixed)),
            ("uring-bufring", Ok(Mode::UringBufring)),
            ("uring-sqpoll", Ok(Mode::UringSqpoll)),
            ("uring-sendzc", Ok(Mode::UringSendzc)),
            ("bogus", Err(Error::Invalid)),
            ("", Err(Error::Invalid)),
            ("URING-RW", Err(Error::Invalid)), // case-sensitive
        ];
        for (s, want) in cases {
            let got = s.parse::<Mode>();
            assert_eq!(&got, want, "mode {s:?}");
            if let Ok(m) = got {
                assert_eq!(m.as_str(), *s, "mode {s:?} str roundtrip");
            }
        }

        assert_eq!("none".parse::<Verify>(), Ok(Verify::None));
        assert_eq!("header".parse::<Verify>(), Ok(Verify::Header));
        assert_eq!("full".parse::<Verify>(), Ok(Verify::Full));
        assert_eq!("nope".parse::<Verify>(), Err(Error::Invalid));
    }

    #[test]
    fn pattern_parse_table() {
        // Mirror of C test_pattern_parse: P/N/B/C.
        let cases: &[(&str, Result<Pattern, Error>)] = &[
            ("echo", Ok(Pattern::Echo)),
            ("stream", Ok(Pattern::Stream)),
            ("bogus", Err(Error::Invalid)),
            ("", Err(Error::Invalid)),
            ("STREAM", Err(Error::Invalid)), // case-sensitive
        ];
        for (s, want) in cases {
            let got = s.parse::<Pattern>();
            assert_eq!(&got, want, "pattern {s:?}");
            if let Ok(p) = got {
                assert_eq!(p.as_str(), *s, "pattern {s:?} str roundtrip");
            }
        }
    }
}
