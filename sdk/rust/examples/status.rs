use xiaoji_client::{Client, Error};

fn main() -> Result<(), Error> {
    let devices = Client::new()?.devices()?;
    println!("keyboard={}, mouse={}", devices.keyboard, devices.mouse);
    Ok(())
}
