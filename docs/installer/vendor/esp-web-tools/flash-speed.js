// Local adaptation for the vendored ESP Web Tools installer.
// Keep ROM synchronization at 115200; accelerate only the UART transfer.
// Espressif's native USB ports do not benefit from a UART baud-rate change.
export async function initializeFlasher(createSession, onFallback) {
  let session = createSession();
  await session.loader.main();
  await session.loader.flashId();

  if (session.transport.device.getInfo().usbVendorId === 0x303a) {
    return session;
  }

  try {
    session.loader.baudrate = 460800;
    await session.loader.changeBaud();
    // Confirm communication after reopening the port at the faster speed.
    await session.loader.flashId();
    return session;
  } catch (error) {
    onFallback(error);
    // A baud change may fail before or after the host port is reopened.
    // Reset and initialize a fresh loader so its stub and transport state
    // cannot leak into the slower attempt. No flash has been erased/written.
    if (session.transport.device.readable || session.transport.device.writable) {
      await session.transport.disconnect();
    }
    session = createSession();
    await session.loader.main();
    await session.loader.flashId();
    return session;
  }
}
