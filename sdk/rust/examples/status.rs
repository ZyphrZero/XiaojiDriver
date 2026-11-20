use xiaoji_client::{Client, Error};

fn main() -> Result<(), Error> {
    let devices = Client::new()?.devices()?;
    println!("keyboard={}, mouse={}", devices.keyboard, devices.mouse);
    // let mut session = xiaoji_client::Session::open(1, 1000, 200)?;
    // session.key(0x1b, true)?;
    // session.key(0x1b, false)?;
    // session.close()?;
    Ok(())
}
