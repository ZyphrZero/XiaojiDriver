use std::ffi::{c_char, c_int, CStr};
use std::fmt;

mod ffi {
    use super::{c_char, c_int};

    unsafe extern "C" {
        pub fn XiaojiGetAbiVersion() -> u32;
        pub fn XiaojiLastError() -> *const c_char;
        pub fn XiaojiGetDevices(started_mask: *mut u32) -> c_int;
        pub fn XiaojiCheckConnection(device: u32) -> c_int;
        pub fn XiaojiTapKey(modifiers: u8, usage: u8) -> c_int;
        pub fn XiaojiTypeAscii(text: *const c_char, byte_count: u32) -> c_int;
        pub fn XiaojiMoveMouse(dx: i32, dy: i32, wheel: i32) -> c_int;
    }
}

#[repr(u32)]
#[derive(Debug, Clone, Copy)]
pub enum Device {
    Keyboard = 1,
    Mouse = 2,
}

#[derive(Debug)]
pub struct Devices {
    pub keyboard: bool,
    pub mouse: bool,
}

#[derive(Debug)]
pub enum Error {
    AbiMismatch(u32),
    InvalidArgument(String),
    Driver { code: i32, message: String },
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::AbiMismatch(version) => write!(formatter, "Expected xiaoji ABI 1, got {version}"),
            Self::InvalidArgument(message) => write!(formatter, "Invalid argument: {message}"),
            Self::Driver { code, message } => write!(formatter, "xiaoji error {code}: {message}"),
        }
    }
}

impl std::error::Error for Error {}

fn check(code: c_int) -> Result<(), Error> {
    if code == 0 {
        return Ok(());
    }
    let message = unsafe { CStr::from_ptr(ffi::XiaojiLastError()) }
        .to_string_lossy()
        .into_owned();
    if code == 1 {
        Err(Error::InvalidArgument(message))
    } else {
        Err(Error::Driver { code, message })
    }
}

pub struct Client(());

impl Client {
    pub fn new() -> Result<Self, Error> {
        let version = unsafe { ffi::XiaojiGetAbiVersion() };
        if version != 1 {
            return Err(Error::AbiMismatch(version));
        }
        Ok(Self(()))
    }

    pub fn devices(&self) -> Result<Devices, Error> {
        let mut mask = 0;
        check(unsafe { ffi::XiaojiGetDevices(&mut mask) })?;
        Ok(Devices {
            keyboard: mask & Device::Keyboard as u32 != 0,
            mouse: mask & Device::Mouse as u32 != 0,
        })
    }

    pub fn check_connection(&self, device: Device) -> Result<(), Error> {
        check(unsafe { ffi::XiaojiCheckConnection(device as u32) })
    }

    pub fn tap_key(&self, modifiers: u8, usage: u8) -> Result<(), Error> {
        check(unsafe { ffi::XiaojiTapKey(modifiers, usage) })
    }

    pub fn type_ascii(&self, text: &str) -> Result<(), Error> {
        if !(1..=256).contains(&text.len()) {
            return Err(Error::InvalidArgument("Expected 1..256 ASCII bytes".into()));
        }
        check(unsafe { ffi::XiaojiTypeAscii(text.as_ptr().cast(), text.len() as u32) })
    }

    pub fn move_mouse(&self, dx: i32, dy: i32, wheel: i32) -> Result<(), Error> {
        check(unsafe { ffi::XiaojiMoveMouse(dx, dy, wheel) })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn query_without_input() -> Result<(), Error> {
        Client::new()?.devices()?;
        Ok(())
    }

    #[test]
    fn reject_invalid_input_and_copy_native_error_text() -> Result<(), Error> {
        let client = Client::new()?;
        let key_error = client.tap_key(0, 0).unwrap_err();
        assert!(matches!(
            client.move_mouse(128, 0, 0),
            Err(Error::InvalidArgument(_))
        ));
        assert!(matches!(key_error, Error::InvalidArgument(message) if message.contains("usage")));
        for text in ["", "a\0", "中文", &"a".repeat(257)] {
            assert!(matches!(
                client.type_ascii(text),
                Err(Error::InvalidArgument(_))
            ));
        }
        Ok(())
    }
}
